//! Drawing-style renderer on wgpu: flat-shaded faces, edges, hatch patterns per material family,
//! no textures. Every colour, hatch and light parameter comes from a `GraphicStyle`.
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

pub mod style;
pub use style::{Fill, GraphicStyle, Lines, Rgb, Stipple};

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FaceVertex {
    pos: [f32; 3],
    normal: [f32; 3],
    /// rgb unused (kept for layout), a: style table slot. Colours come from the style uniform,
    /// so restyling never re-uploads the model.
    color: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    light: [f32; 4],
    edge: [f32; 4],
    params: [f32; 4],
    table: [style::GpuFill; style::SLOTS],
}

// ---------------------------------------------------------------- camera

/// Bounding sphere of a scene, in scene coordinates (mm).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sphere {
    pub center: Vec3,
    pub radius: f32,
}

impl Sphere {
    pub fn of(scene: &Scene) -> Sphere {
        match scene.bounds() {
            Some((lo, hi)) => {
                let (lo, hi) = (Vec3::from(lo), Vec3::from(hi));
                Sphere {
                    center: (lo + hi) / 2.0,
                    radius: ((hi - lo).length() / 2.0).max(1.0),
                }
            }
            None => Sphere {
                center: Vec3::ZERO,
                radius: 1.0,
            },
        }
    }
}

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

    /// Clip planes come from the scene's bounding sphere seen from the eye, not from the orbit
    /// target, so panning away and orbiting back never clips the model.
    pub fn view_proj(&self, aspect: f32, scene: &Sphere) -> Mat4 {
        let eye = self.eye();
        let up = if self.pitch.abs() > 1.55 {
            Vec3::Y
        } else {
            Vec3::Z
        };
        let view = look_at_mat4(eye, self.target, up);
        let r = scene.radius.max(1.0) * 1.01;
        // Depth of the sphere centre along the viewing direction: every point of the sphere has
        // a view depth within d ± r (the near and far planes are planes, not spheres).
        let forward = (self.target - eye).normalize_or(Vec3::NEG_Z);
        let d = (scene.center - eye).dot(forward);
        let far = (d + r).max(1.0);
        let proj = if self.orthographic {
            let h = self.distance * (self.fov_y / 2.0).tan();
            // Orthographic depth is linear: the near plane may sit behind the eye.
            directx::orthographic(-h * aspect, h * aspect, -h, h, d - r, far)
        } else {
            // Keep far/near under ~1e4 for depth precision; 1 mm minimum.
            let near = (d - r).max(far * 1e-4).max(1.0);
            directx::perspective(self.fov_y, aspect.max(1e-3), near, far.max(near * 2.0))
        };
        proj * view
    }

    /// World-space ray through a point in normalized device coords (x, y in -1..1, y up).
    pub fn ray(&self, ndc: [f32; 2], aspect: f32, scene: &Sphere) -> (Vec3, Vec3) {
        let inv = self.view_proj(aspect, scene).inverse();
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
    /// Bounds of the whole scene (not just the visible part), for stable clip planes.
    pub bounds: Sphere,
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
    /// Draw hatches (their intensity is `style.hatch_strength`).
    pub hatching: bool,
    pub edges: bool,
    /// Colours, hatches and light.
    pub style: GraphicStyle,
}

impl Default for DrawOptions {
    fn default() -> Self {
        DrawOptions {
            hatch_scale: 1.0,
            hatching: true,
            edges: true,
            style: GraphicStyle::default(),
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
            let slot = if s == Style::Selected {
                style::SELECTED_SLOT
            } else {
                style::slot(e.category)
            };
            let color = [0, 0, 0, slot];
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
        let bounds = Sphere::of(scene);
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
            bounds,
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
        let vp = camera.view_proj(aspect, &scene.bounds);
        // Light from over the viewer's left shoulder.
        let light = (camera.eye() - camera.target).normalize() + Vec3::new(-0.3, -0.2, 0.9);
        let st = &opts.style;
        let u = Uniforms {
            view_proj: vp.to_cols_array_2d(),
            light: [light.x, light.y, light.z, 0.0],
            edge: {
                let e = st.edges.unit();
                [e[0], e[1], e[2], 1.0]
            },
            params: [
                opts.hatch_scale,
                if opts.hatching {
                    st.hatch_strength
                } else {
                    0.0
                },
                st.ambient,
                0.0,
            ],
            table: style::gpu_table(st),
        };
        queue.write_buffer(&self.uniforms, 0, bytemuck::bytes_of(&u));

        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("bimble"),
        });
        {
            // The target stores gamma-encoded values, so the sRGB colour is written as is.
            let bg = st.background.0.map(|c| c as f64 / 255.0);
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

    /// Largest width or height a target can have on this device.
    pub fn max_target_size(&self) -> u32 {
        self.device.limits().max_texture_dimension_2d
    }

    /// Read a target back as tightly packed RGBA8 (sRGB) rows.
    pub fn read_rgba(&self, target: &Target) -> Result<Vec<u8>, HeadlessError> {
        let (w, h) = target.size;
        let row = w * 4;
        let padded =
            row.div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
        let buf = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("bimble readback"),
            size: padded as u64 * h as u64,
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
        let mut out = Vec::with_capacity(row as usize * h as usize);
        for y in 0..h as usize {
            let s = y * padded as usize;
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
        let (o, d) = cam.ray([0.0, 0.0], 1.5, &Sphere::of(&scene));
        assert!(pick(&scene, |_| true, o, d).is_some());
        // Looking away from the scene hits nothing.
        assert!(pick(&scene, |_| true, o, -d).is_none());
    }

    /// Review 2026-11-05: clip planes used to follow the orbit target, so pan + orbit clipped
    /// the model. Every scene corner must project inside the depth range from any camera.
    #[test]
    fn clip_planes_contain_the_scene_after_pan_and_orbit() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let bounds = Sphere::of(&scene);
        let (lo, hi) = scene.bounds().unwrap();
        let corners: Vec<Vec3> = (0..8)
            .map(|i| {
                Vec3::new(
                    if i & 1 == 0 { lo[0] } else { hi[0] },
                    if i & 2 == 0 { lo[1] } else { hi[1] },
                    if i & 4 == 0 { lo[2] } else { hi[2] },
                )
            })
            .collect();
        for ortho in [false, true] {
            let mut cam = Camera {
                orthographic: ortho,
                ..Default::default()
            };
            cam.fit(&scene, 1.5);
            cam.pan(2.0, 0.5);
            for step in 0..8 {
                cam.orbit(0.8, 0.1 * (step % 3) as f32 - 0.1);
                let vp = cam.view_proj(1.5, &bounds);
                let eye = cam.eye();
                for c in &corners {
                    let clip = vp * c.extend(1.0);
                    // Points behind a perspective eye can't be on screen anyway.
                    if !ortho && (*c - eye).dot(cam.target - eye) <= 0.0 {
                        continue;
                    }
                    let z = clip.z / clip.w;
                    assert!(
                        (-1e-4..=1.0 + 1e-4).contains(&z),
                        "ortho={ortho} step={step}: depth {z} for {c}"
                    );
                }
            }
        }
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
        let bg = GraphicStyle::default().background.0.map(i32::from);
        let differs = px
            .as_chunks::<4>()
            .0
            .iter()
            .filter(|p| (0..3).any(|k| (p[k] as i32 - bg[k]).abs() > 3))
            .count();
        let dark = px
            .as_chunks::<4>()
            .0
            .iter()
            .filter(|p| p[0] < 60 && p[1] < 60 && p[2] < 60)
            .count();
        // The frame covers a good part of the image and has edges drawn.
        assert!(
            differs > 320 * 200 / 10,
            "only {differs} non-background pixels"
        );
        assert!(dark > 100, "only {dark} edge pixels");
    }

    /// One 1 m box per material family, in a row.
    fn family_boxes() -> Scene {
        use bimble_geom::{ElementInfo, MeshBuilder, SceneBuilder};
        let mut sb = SceneBuilder::default();
        for (k, c) in MaterialCategory::ALL.into_iter().enumerate() {
            let (x0, x1) = (k as f64 * 1500.0, k as f64 * 1500.0 + 1000.0);
            let p = |i: usize| {
                [
                    if i & 1 == 0 { x0 } else { x1 },
                    if i & 2 == 0 { 0.0 } else { 1000.0 },
                    if i & 4 == 0 { 0.0 } else { 1000.0 },
                ]
            };
            let mut mb = MeshBuilder::default();
            // Six faces as quads (a, b, c, d), outward.
            for q in [
                [0, 2, 3, 1],
                [4, 5, 7, 6],
                [0, 1, 5, 4],
                [2, 6, 7, 3],
                [0, 4, 6, 2],
                [1, 3, 7, 5],
            ] {
                mb.add_triangle([p(q[0]), p(q[1]), p(q[2])]);
                mb.add_triangle([p(q[0]), p(q[2]), p(q[3])]);
            }
            sb.push(
                ElementInfo {
                    global_id: format!("box{k}"),
                    class: "IfcBuildingElementProxy".into(),
                    name: c.label().into(),
                    storey: String::new(),
                    material: c.label().into(),
                    category: c,
                },
                mb,
            );
        }
        sb.finish()
    }

    fn draw(gpu: &Headless, scene: &Scene, opts: &DrawOptions) -> Vec<[u8; 4]> {
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload(&gpu.device, scene, |_, _| Style::Normal);
        let target = Target::new(&gpu.device, (400, 200), 1);
        let mut cam = Camera::default();
        cam.fit(scene, 2.0);
        r.render(&gpu.device, &gpu.queue, &target, &gs, &cam, opts);
        let px = gpu.read_rgba(&target).unwrap();
        px.as_chunks::<4>().0.to_vec()
    }

    fn count(px: &[[u8; 4]], c: Rgb, tol: i32) -> usize {
        px.iter()
            .filter(|p| (0..3).all(|k| (p[k] as i32 - c.0[k] as i32).abs() <= tol))
            .count()
    }

    /// Every colour of the drawing comes from the style ("red lines instead of black").
    #[test]
    fn graphic_style_drives_every_colour() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = family_boxes();
        // Flat light and no hatch: each face shows exactly its family's fill.
        let mut style = GraphicStyle {
            background: Rgb([10, 200, 30]),
            edges: Rgb([220, 0, 0]),
            ambient: 1.0,
            hatch_strength: 0.0,
            ..GraphicStyle::default()
        };
        let fills = [
            [255, 0, 255],
            [0, 0, 255],
            [255, 255, 0],
            [0, 255, 255],
            [128, 0, 0],
            [0, 128, 128],
            [64, 64, 192],
            [192, 128, 0],
        ];
        for (c, rgb) in MaterialCategory::ALL.into_iter().zip(fills) {
            style.families.get_mut(c).color = Rgb(rgb);
        }
        let opts = DrawOptions {
            style,
            ..Default::default()
        };
        let px = draw(&gpu, &scene, &opts);
        assert!(
            count(&px, style.background, 0) > 400 * 200 / 4,
            "background"
        );
        assert!(count(&px, style.edges, 0) > 200, "red edges");
        assert_eq!(
            count(&px, GraphicStyle::default().edges, 8),
            0,
            "no default edges left"
        );
        for (c, rgb) in MaterialCategory::ALL.into_iter().zip(fills) {
            let n = count(&px, Rgb(rgb), 1);
            assert!(n > 100, "{}: only {n} pixels of its fill", c.label());
        }

        // Hatch strength changes faces only.
        let hatched = DrawOptions {
            style: GraphicStyle {
                hatch_strength: 1.0,
                ..style
            },
            ..opts
        };
        let px2 = draw(&gpu, &scene, &hatched);
        let changed = px.iter().zip(&px2).filter(|(a, b)| a != b).count();
        assert!(changed > 200, "hatching changed only {changed} pixels");
        assert_eq!(
            count(&px2, style.background, 0),
            count(&px, style.background, 0)
        );
    }
}
