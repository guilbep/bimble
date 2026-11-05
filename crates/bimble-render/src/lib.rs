//! Drawing-style renderer on wgpu: flat-shaded faces, black edges, ~12-colour palette, hatch
//! patterns per material family, no textures.
//!
//! The renderer draws into an offscreen `Target`. The viewer shows that texture in its window;
//! `bimble render` reads it back as a PNG. Both paths run the same code.

use bimble_core::MaterialCategory;
use bimble_geom::{Scene, SceneElement};
use glam::camera::rh::{proj::directx, view::look_at_mat4};
use glam::{Mat4, Vec3};
use wgpu::util::DeviceExt;

pub use wgpu;

pub const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
/// Plain (non-sRGB) target holding gamma-encoded colours: what egui expects for textures, what
/// PNG stores, and it needs no sRGB view formats (missing on some old OpenGL drivers).
pub const COLOR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

/// The palette: background, edges, selection, and one muted colour per material family.
pub mod palette {
    use bimble_core::MaterialCategory;

    pub const BACKGROUND: [f32; 3] = [0.97, 0.97, 0.96];
    pub const EDGE: [f32; 3] = [0.10, 0.10, 0.11];
    pub const SELECTION: [u8; 3] = [232, 126, 4];

    /// sRGB 0..255
    pub fn category(c: MaterialCategory) -> [u8; 3] {
        match c {
            MaterialCategory::Concrete => [214, 214, 210],
            MaterialCategory::Timber => [226, 196, 150],
            MaterialCategory::Steel => [150, 170, 196],
            MaterialCategory::Masonry => [206, 150, 128],
            MaterialCategory::Insulation => [236, 220, 120],
            MaterialCategory::Earth => [176, 160, 132],
            MaterialCategory::Glass => [176, 214, 226],
            MaterialCategory::Other => [238, 238, 236],
        }
    }

    /// Hatch pattern id used by the shader.
    pub fn hatch(c: MaterialCategory) -> u8 {
        match c {
            MaterialCategory::Concrete => 1,
            MaterialCategory::Timber => 2,
            MaterialCategory::Steel => 3,
            MaterialCategory::Masonry => 4,
            MaterialCategory::Insulation => 5,
            MaterialCategory::Earth => 6,
            MaterialCategory::Glass | MaterialCategory::Other => 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FaceVertex {
    pos: [f32; 3],
    normal: [f32; 3],
    color: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    light: [f32; 4],
    edge: [f32; 4],
    hatch: [f32; 4],
}

// ---------------------------------------------------------------- camera

/// Orbit camera around a target, Z up (IFC convention). Units: mm, scene coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Camera {
    pub target: Vec3,
    /// Radians around Z, from +X.
    pub yaw: f32,
    /// Radians above the horizon.
    pub pitch: f32,
    pub distance: f32,
    pub fov_y: f32,
    pub orthographic: bool,
}

impl Default for Camera {
    fn default() -> Self {
        Camera {
            target: Vec3::ZERO,
            yaw: (-55f32).to_radians(),
            pitch: 28f32.to_radians(),
            distance: 10_000.0,
            fov_y: 35f32.to_radians(),
            orthographic: false,
        }
    }
}

impl Camera {
    pub fn eye(&self) -> Vec3 {
        let (cp, sp) = (self.pitch.cos(), self.pitch.sin());
        self.target + self.distance * Vec3::new(cp * self.yaw.cos(), cp * self.yaw.sin(), sp)
    }

    /// Frame the whole scene.
    pub fn fit(&mut self, scene: &Scene, aspect: f32) {
        if let Some((lo, hi)) = scene.bounds() {
            let (lo, hi) = (Vec3::from(lo), Vec3::from(hi));
            self.target = (lo + hi) / 2.0;
            let r = ((hi - lo).length() / 2.0).max(1.0);
            // Half-angle of the narrower side of the view frustum.
            let half_v = self.fov_y / 2.0;
            let half_h = (half_v.tan() * aspect.max(1e-3)).atan();
            self.distance = r / half_v.min(half_h).sin() * 1.02;
        }
    }

    pub fn view_proj(&self, aspect: f32, scene_radius: f32) -> Mat4 {
        let eye = self.eye();
        let up = if self.pitch.abs() > 1.55 {
            Vec3::Y
        } else {
            Vec3::Z
        };
        let view = look_at_mat4(eye, self.target, up);
        let far = self.distance + 4.0 * scene_radius.max(1.0);
        let proj = if self.orthographic {
            let h = self.distance * (self.fov_y / 2.0).tan();
            directx::orthographic(-h * aspect, h * aspect, -h, h, -far, far)
        } else {
            let near = (self.distance - 2.0 * scene_radius)
                .max(self.distance * 0.002)
                .max(1.0);
            directx::perspective(self.fov_y, aspect.max(1e-3), near, far)
        };
        proj * view
    }

    /// World-space ray through a point in normalized device coords (x, y in -1..1, y up).
    pub fn ray(&self, ndc: [f32; 2], aspect: f32, scene_radius: f32) -> (Vec3, Vec3) {
        let inv = self.view_proj(aspect, scene_radius).inverse();
        let a = inv.project_point3(Vec3::new(ndc[0], ndc[1], 0.0));
        let b = inv.project_point3(Vec3::new(ndc[0], ndc[1], 1.0));
        (a, (b - a).normalize())
    }

    pub fn orbit(&mut self, dx: f32, dy: f32) {
        self.yaw -= dx;
        self.pitch = (self.pitch + dy).clamp(-1.57, 1.57);
    }

    /// Pan by a screen-space fraction of the view height.
    pub fn pan(&mut self, dx: f32, dy: f32) {
        let fwd = (self.target - self.eye()).normalize();
        let right = fwd.cross(Vec3::Z).normalize_or(Vec3::X);
        let up = right.cross(fwd);
        let h = 2.0 * self.distance * (self.fov_y / 2.0).tan();
        self.target += (-dx * right + dy * up) * h;
    }

    pub fn zoom(&mut self, factor: f32) {
        self.distance = (self.distance * factor).clamp(10.0, 1.0e8);
    }
}

// ---------------------------------------------------------------- GPU scene

/// How each element should be drawn this upload.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Style {
    Hidden,
    Normal,
    Selected,
}

pub struct GpuScene {
    faces: wgpu::Buffer,
    face_vertices: u32,
    lines: wgpu::Buffer,
    line_vertices: u32,
    pub radius: f32,
}

impl GpuScene {
    pub fn triangles(&self) -> u32 {
        self.face_vertices / 3
    }
}

// ---------------------------------------------------------------- target

/// Offscreen colour + depth (+ MSAA) textures.
pub struct Target {
    pub size: (u32, u32),
    pub color: wgpu::Texture,
    pub color_view: wgpu::TextureView,
    msaa_view: Option<wgpu::TextureView>,
    depth_view: wgpu::TextureView,
}

impl Target {
    pub fn new(device: &wgpu::Device, size: (u32, u32), samples: u32) -> Target {
        let size = (size.0.max(1), size.1.max(1));
        let extent = wgpu::Extent3d {
            width: size.0,
            height: size.1,
            depth_or_array_layers: 1,
        };
        let tex = |label, format, samples, usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: extent,
                mip_level_count: 1,
                sample_count: samples,
                dimension: wgpu::TextureDimension::D2,
                format,
                usage,
                view_formats: &[],
            })
        };
        let color = tex(
            "bimble color",
            COLOR_FORMAT,
            1,
            wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
        );
        let msaa_view = (samples > 1).then(|| {
            tex(
                "bimble msaa",
                COLOR_FORMAT,
                samples,
                wgpu::TextureUsages::RENDER_ATTACHMENT,
            )
            .create_view(&Default::default())
        });
        let depth = tex(
            "bimble depth",
            DEPTH_FORMAT,
            samples,
            wgpu::TextureUsages::RENDER_ATTACHMENT,
        );
        Target {
            size,
            color_view: color.create_view(&Default::default()),
            color,
            msaa_view,
            depth_view: depth.create_view(&Default::default()),
        }
    }
}

// ---------------------------------------------------------------- renderer

pub struct Renderer {
    face_pipeline: wgpu::RenderPipeline,
    line_pipeline: wgpu::RenderPipeline,
    uniforms: wgpu::Buffer,
    bind_group: wgpu::BindGroup,
    pub samples: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct DrawOptions {
    /// Hatch spacing multiplier in pixels (use the display scale factor on HiDPI).
    pub hatch_scale: f32,
    /// 0 = no hatching, 1 = full.
    pub hatch_strength: f32,
    pub edges: bool,
}

impl Default for DrawOptions {
    fn default() -> Self {
        DrawOptions {
            hatch_scale: 1.0,
            hatch_strength: 0.5,
            edges: true,
        }
    }
}

impl Renderer {
    pub fn new(device: &wgpu::Device, samples: u32) -> Renderer {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("bimble shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("shader.wgsl").into()),
        });
        let uniforms = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("bimble uniforms"),
            size: std::mem::size_of::<Uniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("bimble bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("bimble bg"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: uniforms.as_entire_binding(),
            }],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("bimble layout"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        let multisample = wgpu::MultisampleState {
            count: samples,
            mask: !0,
            alpha_to_coverage_enabled: false,
        };
        let target = [Some(wgpu::ColorTargetState {
            format: COLOR_FORMAT,
            blend: None,
            write_mask: wgpu::ColorWrites::ALL,
        })];

        let face_attrs = wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Unorm8x4];
        let face_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("bimble faces"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_face"),
                compilation_options: Default::default(),
                buffers: &[Some(wgpu::VertexBufferLayout {
                    array_stride: std::mem::size_of::<FaceVertex>() as u64,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &face_attrs,
                })],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                // Imported meshes don't always have consistent winding: draw both sides.
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Less),
                stencil: Default::default(),
                // Push faces back so the edges drawn afterwards win the depth test.
                bias: wgpu::DepthBiasState {
                    constant: 2,
                    slope_scale: 1.5,
                    clamp: 0.0,
                },
            }),
            multisample,
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_face"),
                compilation_options: Default::default(),
                targets: &target,
            }),
            multiview_mask: None,
            cache: None,
        });

        let line_attrs = wgpu::vertex_attr_array![0 => Float32x3];
        let line_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("bimble edges"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_line"),
                compilation_options: Default::default(),
                buffers: &[Some(wgpu::VertexBufferLayout {
                    array_stride: 12,
                    step_mode: wgpu::VertexStepMode::Vertex,
                    attributes: &line_attrs,
                })],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::LineList,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: Some(false),
                depth_compare: Some(wgpu::CompareFunction::LessEqual),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample,
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_line"),
                compilation_options: Default::default(),
                targets: &target,
            }),
            multiview_mask: None,
            cache: None,
        });

        Renderer {
            face_pipeline,
            line_pipeline,
            uniforms,
            bind_group,
            samples,
        }
    }

    /// Upload the visible part of a scene. `style(i, element)` decides how each element is drawn.
    pub fn upload(
        &self,
        device: &wgpu::Device,
        scene: &Scene,
        style: impl Fn(usize, &SceneElement) -> Style,
    ) -> GpuScene {
        let mut faces: Vec<FaceVertex> = Vec::new();
        let mut lines: Vec<[f32; 3]> = Vec::new();
        for (i, e) in scene.elements.iter().enumerate() {
            let s = style(i, e);
            if s == Style::Hidden {
                continue;
            }
            let rgb = if s == Style::Selected {
                palette::SELECTION
            } else {
                palette::category(e.category)
            };
            let pattern = if s == Style::Selected {
                0
            } else {
                palette::hatch(e.category)
            };
            let color = [rgb[0], rgb[1], rgb[2], pattern];
            for &k in &e.mesh.indices {
                let k = k as usize;
                faces.push(FaceVertex {
                    pos: e.mesh.positions[k],
                    normal: e.mesh.normals[k],
                    color,
                });
            }
            for seg in &e.mesh.edges {
                lines.extend_from_slice(seg);
            }
        }
        let radius = scene
            .bounds()
            .map(|(lo, hi)| (Vec3::from(hi) - Vec3::from(lo)).length() / 2.0)
            .unwrap_or(1.0)
            .max(1.0);
        let buf = |label, contents: &[u8]| {
            device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some(label),
                // Never create an empty buffer: some backends reject it.
                contents: if contents.is_empty() {
                    &[0u8; 16]
                } else {
                    contents
                },
                usage: wgpu::BufferUsages::VERTEX,
            })
        };
        GpuScene {
            faces: buf("bimble face vb", bytemuck::cast_slice(&faces)),
            face_vertices: faces.len() as u32,
            lines: buf("bimble line vb", bytemuck::cast_slice(&lines)),
            line_vertices: lines.len() as u32,
            radius,
        }
    }

    pub fn render(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &Target,
        scene: &GpuScene,
        camera: &Camera,
        opts: &DrawOptions,
    ) {
        let aspect = target.size.0 as f32 / target.size.1 as f32;
        let vp = camera.view_proj(aspect, scene.radius);
        // Light from over the viewer's left shoulder.
        let light = (camera.eye() - camera.target).normalize() + Vec3::new(-0.3, -0.2, 0.9);
        let u = Uniforms {
            view_proj: vp.to_cols_array_2d(),
            light: [light.x, light.y, light.z, 0.0],
            edge: [palette::EDGE[0], palette::EDGE[1], palette::EDGE[2], 1.0],
            hatch: [opts.hatch_scale, opts.hatch_strength, 0.0, 0.0],
        };
        queue.write_buffer(&self.uniforms, 0, bytemuck::bytes_of(&u));

        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("bimble"),
        });
        {
            // The target stores gamma-encoded values, so the sRGB palette is written as is.
            let bg = palette::BACKGROUND.map(f64::from);
            let (view, resolve) = match &target.msaa_view {
                Some(m) => (m, Some(&target.color_view)),
                None => (&target.color_view, None),
            };
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("bimble pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view,
                    depth_slice: None,
                    resolve_target: resolve,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color {
                            r: bg[0],
                            g: bg[1],
                            b: bg[2],
                            a: 1.0,
                        }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &target.depth_view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Discard,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_bind_group(0, &self.bind_group, &[]);
            if scene.face_vertices > 0 {
                pass.set_pipeline(&self.face_pipeline);
                pass.set_vertex_buffer(0, scene.faces.slice(..));
                pass.draw(0..scene.face_vertices, 0..1);
            }
            if opts.edges && scene.line_vertices > 0 {
                pass.set_pipeline(&self.line_pipeline);
                pass.set_vertex_buffer(0, scene.lines.slice(..));
                pass.draw(0..scene.line_vertices, 0..1);
            }
        }
        queue.submit([enc.finish()]);
    }
}

// ---------------------------------------------------------------- picking

/// Closest element hit by a ray (scene coordinates), with the hit distance.
pub fn pick(
    scene: &Scene,
    visible: impl Fn(usize) -> bool,
    origin: Vec3,
    dir: Vec3,
) -> Option<(usize, f32)> {
    let mut best: Option<(usize, f32)> = None;
    for (i, e) in scene.elements.iter().enumerate() {
        if !visible(i) {
            continue;
        }
        for t in e.mesh.indices.as_chunks::<3>().0 {
            let [a, b, c] = t.map(|k| Vec3::from(e.mesh.positions[k as usize]));
            if let Some(d) = ray_triangle(origin, dir, a, b, c)
                && best.is_none_or(|(_, bd)| d < bd)
            {
                best = Some((i, d));
            }
        }
    }
    best
}

/// Möller–Trumbore, two-sided.
fn ray_triangle(o: Vec3, d: Vec3, a: Vec3, b: Vec3, c: Vec3) -> Option<f32> {
    let (e1, e2) = (b - a, c - a);
    let p = d.cross(e2);
    let det = e1.dot(p);
    if det.abs() < 1e-12 {
        return None;
    }
    let inv = 1.0 / det;
    let s = o - a;
    let u = s.dot(p) * inv;
    if !(0.0..=1.0).contains(&u) {
        return None;
    }
    let q = s.cross(e1);
    let v = d.dot(q) * inv;
    if v < 0.0 || u + v > 1.0 {
        return None;
    }
    let t = e2.dot(q) * inv;
    (t > 0.0).then_some(t)
}

// ---------------------------------------------------------------- headless

#[derive(Debug)]
pub enum HeadlessError {
    NoAdapter(String),
    Device(String),
    Readback(String),
}

impl std::fmt::Display for HeadlessError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HeadlessError::NoAdapter(e) => write!(f, "no GPU adapter: {e}"),
            HeadlessError::Device(e) => write!(f, "GPU device: {e}"),
            HeadlessError::Readback(e) => write!(f, "readback: {e}"),
        }
    }
}

impl std::error::Error for HeadlessError {}

/// A GPU device without a window, for `bimble render` and tests.
pub struct Headless {
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub adapter_info: wgpu::AdapterInfo,
}

impl Headless {
    pub fn new() -> Result<Headless, HeadlessError> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::from_env().unwrap_or(wgpu::Backends::all()),
            ..wgpu::InstanceDescriptor::new_without_display_handle()
        });
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::LowPower,
            ..Default::default()
        }))
        .map_err(|e| HeadlessError::NoAdapter(e.to_string()))?;
        let adapter_info = adapter.get_info();
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("bimble headless"),
            required_limits: wgpu::Limits::downlevel_defaults().using_resolution(adapter.limits()),
            ..Default::default()
        }))
        .map_err(|e| HeadlessError::Device(e.to_string()))?;
        Ok(Headless {
            device,
            queue,
            adapter_info,
        })
    }

    /// Read a target back as tightly packed RGBA8 (sRGB) rows.
    pub fn read_rgba(&self, target: &Target) -> Result<Vec<u8>, HeadlessError> {
        let (w, h) = target.size;
        let row = w * 4;
        let padded =
            row.div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
        let buf = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("bimble readback"),
            size: (padded * h) as u64,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mut enc = self.device.create_command_encoder(&Default::default());
        enc.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &target.color,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &buf,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(padded),
                    rows_per_image: Some(h),
                },
            },
            wgpu::Extent3d {
                width: w,
                height: h,
                depth_or_array_layers: 1,
            },
        );
        self.queue.submit([enc.finish()]);
        let slice = buf.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        self.device
            .poll(wgpu::PollType::wait_indefinitely())
            .map_err(|e| HeadlessError::Readback(e.to_string()))?;
        rx.recv()
            .map_err(|e| HeadlessError::Readback(e.to_string()))?
            .map_err(|e| HeadlessError::Readback(e.to_string()))?;
        let data = slice
            .get_mapped_range()
            .map_err(|e| HeadlessError::Readback(e.to_string()))?;
        let mut out = Vec::with_capacity((row * h) as usize);
        for y in 0..h {
            let s = (y * padded) as usize;
            out.extend_from_slice(&data[s..s + row as usize]);
        }
        Ok(out)
    }
}

/// Write RGBA8 pixels as a PNG.
pub fn write_png(path: &std::path::Path, w: u32, h: u32, rgba: &[u8]) -> std::io::Result<()> {
    let file = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut enc = png::Encoder::new(file, w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    enc.set_source_srgb(png::SrgbRenderingIntent::Perceptual);
    let mut wr = enc.write_header().map_err(std::io::Error::other)?;
    wr.write_image_data(rgba).map_err(std::io::Error::other)?;
    wr.finish().map_err(std::io::Error::other)
}

/// Visibility filter used by the viewer and `bimble render --hide`.
pub fn hidden_by_category(
    hidden: &[MaterialCategory],
) -> impl Fn(usize, &SceneElement) -> Style + '_ {
    move |_, e| {
        if hidden.contains(&e.category) {
            Style::Hidden
        } else {
            Style::Normal
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    #[test]
    fn ray_hits_triangle() {
        let t = ray_triangle(
            Vec3::new(0.2, 0.2, 5.0),
            Vec3::NEG_Z,
            Vec3::ZERO,
            Vec3::X,
            Vec3::Y,
        );
        assert!((t.unwrap() - 5.0).abs() < 1e-6);
        assert!(
            ray_triangle(
                Vec3::new(2.0, 2.0, 5.0),
                Vec3::NEG_Z,
                Vec3::ZERO,
                Vec3::X,
                Vec3::Y
            )
            .is_none()
        );
    }

    #[test]
    fn pick_from_camera_hits_something_in_the_middle() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut cam = Camera::default();
        cam.fit(&scene, 1.5);
        let r = scene
            .bounds()
            .map(|(lo, hi)| (Vec3::from(hi) - Vec3::from(lo)).length() / 2.0)
            .unwrap();
        let (o, d) = cam.ray([0.0, 0.0], 1.5, r);
        assert!(pick(&scene, |_| true, o, d).is_some());
        // Looking away from the scene hits nothing.
        assert!(pick(&scene, |_| true, o, -d).is_none());
    }

    /// Renders on whatever adapter exists (a software one in CI containers). Skipped if none.
    #[test]
    fn headless_render_draws_the_frame() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let r = Renderer::new(&gpu.device, 4);
        let gs = r.upload(&gpu.device, &scene, |_, _| Style::Normal);
        let target = Target::new(&gpu.device, (320, 200), 4);
        let mut cam = Camera::default();
        cam.fit(&scene, 1.6);
        r.render(
            &gpu.device,
            &gpu.queue,
            &target,
            &gs,
            &cam,
            &DrawOptions::default(),
        );
        let px = gpu.read_rgba(&target).unwrap();
        assert_eq!(px.len(), 320 * 200 * 4);
        let bg = palette::BACKGROUND.map(|c| (c * 255.0).round() as i32);
        let differs = px
            .chunks_exact(4)
            .filter(|p| (0..3).any(|k| (p[k] as i32 - bg[k]).abs() > 3))
            .count();
        let dark = px
            .chunks_exact(4)
            .filter(|p| p[0] < 60 && p[1] < 60 && p[2] < 60)
            .count();
        // The frame covers a good part of the image and has edges drawn.
        assert!(
            differs > 320 * 200 / 10,
            "only {differs} non-background pixels"
        );
        assert!(dark > 100, "only {dark} edge pixels");
    }
}
