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

/// The graphic style is data in `bimble_core::style`; re-exported here for convenience.
pub use bimble_core::style::{self, Fill, GraphicStyle, Lines, Rgb, Stipple};
mod style_gpu;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FaceVertex {
    pos: [f32; 3],
    /// rgb: the face's normal (n × 0.5 + 0.5, unorm8: shading is flat and two-sided, so 8 bits
    /// and the sign don't show); a: style table slot. Colours come from the style uniform, so
    /// restyling never re-uploads the model. 16 bytes a vertex (was 28 with an f32 normal).
    normal_slot: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    light: [f32; 4],
    edge: [f32; 4],
    params: [f32; 4],
    clip_min: [f32; 4],
    clip_max: [f32; 4],
    /// xyz: camera position (scene coordinates), w: 1 for orthographic.
    eye: [f32; 4],
    /// xyz: view direction (unit).
    view_dir: [f32; 4],
    /// x: depth push (NDC, times w) of the layer.
    layer: [f32; 4],
    table: [style_gpu::GpuFill; style_gpu::SLOTS],
}

/// An edge end: position, and the normals of the edge's two faces (see `Mesh::edge_normals`).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct LineVertex {
    pos: [f32; 3],
    n1: [i8; 4],
    n2: [i8; 4],
}

/// An axis-aligned clipping box ("section box"), in scene coordinates (mm, relative to
/// `Scene::origin_mm`). Geometry outside is not drawn; solids it cuts show their cut surface.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClipBox {
    pub min: Vec3,
    pub max: Vec3,
}

impl ClipBox {
    pub fn contains(&self, p: Vec3) -> bool {
        p.cmpge(self.min).all() && p.cmple(self.max).all()
    }
    /// The bounding box of a scene, or of some of its elements.
    pub fn around(scene: &Scene, mut keep: impl FnMut(usize) -> bool) -> Option<ClipBox> {
        let (mut lo, mut hi) = (Vec3::splat(f32::INFINITY), Vec3::splat(f32::NEG_INFINITY));
        for (i, e) in scene.elements.iter().enumerate() {
            if !keep(i) {
                continue;
            }
            for p in &e.mesh.positions {
                lo = lo.min(Vec3::from(*p));
                hi = hi.max(Vec3::from(*p));
            }
        }
        // A small margin: faces lying exactly on a clip plane would flicker in and out.
        let m = Vec3::splat(1.0) + (hi - lo) * 1e-5;
        lo.cmple(hi).all().then_some(ClipBox {
            min: lo - m,
            max: hi + m,
        })
    }
}

// ---------------------------------------------------------------- camera

/// Bounding sphere of a scene, in scene coordinates (mm).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sphere {
    pub center: Vec3,
    pub radius: f32,
}

impl Sphere {
    /// The smallest sphere containing both.
    pub fn merge(self, o: Sphere) -> Sphere {
        let d = o.center - self.center;
        let dist = d.length();
        if dist + o.radius <= self.radius {
            return self;
        }
        if dist + self.radius <= o.radius {
            return o;
        }
        let radius = (dist + self.radius + o.radius) / 2.0;
        let center = self.center + d * ((radius - self.radius) / dist.max(1e-6));
        Sphere { center, radius }
    }

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
    /// Mirror the picture left-right: a plan looking up is drawn as reflected in a mirror on the
    /// floor, so north stays up and east stays right (ADR 0021).
    pub mirror_x: bool,
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
            mirror_x: false,
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
            self.fit_box(Vec3::from(lo), Vec3::from(hi), aspect);
        }
    }

    /// Frame an axis-aligned box (scene coordinates).
    pub fn fit_box(&mut self, lo: Vec3, hi: Vec3, aspect: f32) {
        self.target = (lo + hi) / 2.0;
        let r = ((hi - lo).length() / 2.0).max(1.0);
        // Half-angle of the narrower side of the view frustum.
        let half_v = self.fov_y / 2.0;
        let half_h = (half_v.tan() * aspect.max(1e-3)).atan();
        self.distance = r / half_v.min(half_h).sin() * 1.02;
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
        if self.mirror_x {
            Mat4::from_scale(Vec3::new(-1.0, 1.0, 1.0)) * proj * view
        } else {
            proj * view
        }
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

    /// Orbit about `pivot` (scene coordinates): the camera turns rigidly about it, so the
    /// pivot stays where it is on screen, like the wheel's zoom about the cursor.
    pub fn orbit_about(&mut self, dx: f32, dy: f32, pivot: Vec3) {
        let (yaw, pitch) = (self.yaw, self.pitch);
        self.orbit(dx, dy);
        // The turn actually made (the pitch is clamped): about Z, then about the horizontal
        // axis the pitch turns about (right of the view, before the yaw).
        let h = Vec3::new(-yaw.sin(), yaw.cos(), 0.0);
        let r = glam::Quat::from_rotation_z(self.yaw - yaw)
            * glam::Quat::from_axis_angle(h, -(self.pitch - pitch));
        self.target = pivot + r * (self.target - pivot);
    }

    /// Pan by a screen-space fraction of the view height.
    pub fn pan(&mut self, dx: f32, dy: f32) {
        let fwd = (self.target - self.eye()).normalize();
        let right = fwd.cross(Vec3::Z).normalize_or(Vec3::X);
        let up = right.cross(fwd);
        let h = 2.0 * self.distance * (self.fov_y / 2.0).tan();
        let dx = if self.mirror_x { -dx } else { dx };
        self.target += (-dx * right + dy * up) * h;
    }

    pub fn zoom(&mut self, factor: f32) {
        self.zoom_at(factor, self.target);
    }

    /// Zoom about `pivot` (scene coordinates): the camera is scaled about it, so the pivot stays
    /// at the same place on screen, in perspective and orthographic views alike.
    pub fn zoom_at(&mut self, factor: f32, pivot: Vec3) {
        let d = (self.distance * factor).clamp(10.0, 1.0e8);
        // The factor actually applied: at the clamp, the target must not keep sliding.
        let f = d / self.distance;
        self.target = pivot + (self.target - pivot) * f;
        self.distance = d;
    }

    /// Where a point in normalized device coords meets the plane through the target, facing
    /// the viewer: the zoom pivot when nothing is under the cursor, and in 2D views.
    pub fn on_target_plane(&self, ndc: [f32; 2], aspect: f32, scene: &Sphere) -> Vec3 {
        let (o, d) = self.ray(ndc, aspect, scene);
        let n = (self.target - self.eye()).normalize_or(Vec3::NEG_Z);
        let den = d.dot(n);
        if den.abs() < 1e-6 {
            return self.target;
        }
        o + d * ((self.target - o).dot(n) / den)
    }
}

// ---------------------------------------------------------------- GPU scene

/// How each element should be drawn this upload.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Style {
    Hidden,
    Normal,
    Selected,
    /// An element of a linked (reference) model.
    Reference,
    /// Comparing: our element, absent from the reference.
    OnlyHere,
    /// Comparing: a reference element, absent from ours.
    OnlyInReference,
}

/// Most scenes drawn together: the model and up to three linked models.
pub const MAX_LAYERS: usize = 4;
/// Depth push of linked models (NDC depth, times w).
const LINKED_DEPTH_PUSH: f32 = 2e-5;

/// One scene in a multi-scene drawing.
#[derive(Clone, Copy)]
pub struct Layer<'a> {
    pub scene: &'a GpuScene,
    /// Added to the layer's scene coordinates to get the model's (main layer's) coordinates.
    pub offset: Vec3,
    /// Draw faces; false draws edges only (whatever `DrawOptions::edges` says).
    pub faces: bool,
    /// Edge colour; None: the style's.
    pub edge_color: Option<Rgb>,
    /// A linked model: pushed back a hair in depth and drawn before the others, so the model
    /// wins where both have the same face or edge.
    pub linked: bool,
}

impl<'a> Layer<'a> {
    pub fn main(scene: &'a GpuScene) -> Layer<'a> {
        Layer {
            scene,
            offset: Vec3::ZERO,
            faces: true,
            edge_color: None,
            linked: false,
        }
    }

    /// Highlight: the edges of `scene` in `color`, drawn last (e.g. the element under the cursor).
    pub fn highlight(scene: &'a GpuScene, color: Rgb) -> Layer<'a> {
        Layer {
            scene,
            offset: Vec3::ZERO,
            faces: false,
            edge_color: Some(color),
            linked: false,
        }
    }
}

pub struct GpuScene {
    /// Vertex buffers with their vertex counts, each within the device's buffer size limit
    /// (256 MiB by default): a large model needs several.
    faces: Vec<(wgpu::Buffer, u32)>,
    lines: Vec<(wgpu::Buffer, u32)>,
    /// Whether edges carry face normals (needed to hide back edges in the section box).
    pub edge_normals: bool,
    /// Bounds of the whole scene (not just the visible part), for stable clip planes.
    pub bounds: Sphere,
}

impl GpuScene {
    pub fn triangles(&self) -> u32 {
        self.faces.iter().map(|(_, n)| n / 3).sum()
    }
}

/// Vertex buffers filled as vertices come: each is sent to the GPU when full, so the CPU never
/// holds more than one (a 9M-triangle model is ~750 MB of face vertices).
struct Chunks<'d, T> {
    device: &'d wgpu::Device,
    queue: &'d wgpu::Queue,
    label: &'static str,
    /// Vertices per buffer: within the device's limit, at most 64 MiB, and a multiple of 6 so
    /// buffers split only between whole triangles and whole segments. 64 MiB measured best on a
    /// 9M-triangle model: 16 MiB drew 25 % slower, 256 MiB drew the same but held 540 MB more
    /// after loading. Total GPU memory is not queryable (wgpu, WebGPU), and the sum is the same
    /// whatever the piece size.
    per: usize,
    cur: Vec<T>,
    done: Vec<(wgpu::Buffer, u32)>,
}

impl<'d, T: bytemuck::Pod> Chunks<'d, T> {
    fn new(device: &'d wgpu::Device, queue: &'d wgpu::Queue, label: &'static str) -> Self {
        let bytes = device.limits().max_buffer_size.min(64 << 20) as usize;
        let per = (bytes / std::mem::size_of::<T>() / 6 * 6).max(6);
        Chunks {
            device,
            queue,
            label,
            per,
            cur: Vec::new(),
            done: Vec::new(),
        }
    }

    fn push(&mut self, v: T) {
        self.cur.push(v);
        if self.cur.len() == self.per {
            self.flush();
        }
    }

    fn flush(&mut self) {
        if self.cur.is_empty() {
            return;
        }
        let buf = self
            .device
            .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some(self.label),
                contents: bytemuck::cast_slice(&self.cur),
                usage: wgpu::BufferUsages::VERTEX,
            });
        self.done.push((buf, self.cur.len() as u32));
        self.cur.clear();
        // The copy goes through a staging buffer, freed only once a submission has used it:
        // without this, every chunk's staging copy stays alive until the first frame.
        self.queue.submit([]);
        let _ = self.device.poll(wgpu::PollType::wait_indefinitely());
    }

    fn finish(mut self) -> Vec<(wgpu::Buffer, u32)> {
        self.flush();
        self.done
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
    face_front_pipeline: wgpu::RenderPipeline,
    face_back_pipeline: wgpu::RenderPipeline,
    face_front_mirrored: wgpu::RenderPipeline,
    face_back_mirrored: wgpu::RenderPipeline,
    line_clip_pipeline: wgpu::RenderPipeline,
    line_wide_pipeline: wgpu::RenderPipeline,
    line_clip_lean_pipeline: wgpu::RenderPipeline,
    line_pipeline: wgpu::RenderPipeline,
    /// One uniform buffer per layer (the model, then linked models).
    layer_uniforms: Vec<(wgpu::Buffer, wgpu::BindGroup)>,
    pub samples: u32,
}

#[derive(Clone, Debug)]
pub struct DrawOptions {
    /// Hatch spacing multiplier in pixels (use the display scale factor on HiDPI).
    pub hatch_scale: f32,
    /// Draw hatches (their intensity is `style.hatch_strength`).
    pub hatching: bool,
    pub edges: bool,
    /// Colours, hatches and light.
    pub style: GraphicStyle,
    /// Section box: draw only what is inside.
    pub clip: Option<ClipBox>,
}

impl Default for DrawOptions {
    fn default() -> Self {
        DrawOptions {
            hatch_scale: 1.0,
            hatching: true,
            edges: true,
            style: GraphicStyle::default(),
            clip: None,
        }
    }
}

impl Renderer {
    pub fn new(device: &wgpu::Device, samples: u32) -> Renderer {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("bimble shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("shader.wgsl").into()),
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
        let layer_uniforms = (0..MAX_LAYERS)
            .map(|_| {
                let buf = device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("bimble uniforms"),
                    size: std::mem::size_of::<Uniforms>() as u64,
                    usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                });
                let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("bimble bg"),
                    layout: &bgl,
                    entries: &[wgpu::BindGroupEntry {
                        binding: 0,
                        resource: buf.as_entire_binding(),
                    }],
                });
                (buf, bg)
            })
            .collect();
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

        let face_attrs = wgpu::vertex_attr_array![0 => Float32x3, 1 => Unorm8x4];
        let face_pipeline_with = |label: &str,
                                  (vs, fs): (&str, &str),
                                  cull_mode: Option<wgpu::Face>,
                                  front_face: wgpu::FrontFace,
                                  bias: (i32, f32)| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some(vs),
                    compilation_options: Default::default(),
                    buffers: &[Some(wgpu::VertexBufferLayout {
                        array_stride: std::mem::size_of::<FaceVertex>() as u64,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &face_attrs,
                    })],
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode,
                    front_face,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FORMAT,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(wgpu::CompareFunction::Less),
                    stencil: Default::default(),
                    // Push faces back so the edges drawn afterwards win the depth test.
                    bias: wgpu::DepthBiasState {
                        constant: bias.0,
                        slope_scale: bias.1,
                        clamp: 0.0,
                    },
                }),
                multisample,
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fs),
                    compilation_options: Default::default(),
                    targets: &target,
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        // Imported meshes don't always have consistent winding: draw both sides at once.
        let ccw = wgpu::FrontFace::Ccw;
        let face_pipeline =
            face_pipeline_with("bimble faces", ("vs_face", "fs_face"), None, ccw, (2, 1.5));
        // With a section box, inward faces show cut surfaces. Drawn in a second pass, pushed back
        // more where they slope away (so where an outward and an inward face meet, at
        // silhouette corners, the outward one wins instead of speckling the outline with cut
        // colour) but forward where they face the camera: a column's bottom, seen from inside,
        // then wins over the top of the slab it stands on (same depth), and a plan shows the
        // column's cut.
        // A mirrored camera turns every triangle's winding around: the same passes then need
        // the other winding as front.
        let clip_pipelines = |front: wgpu::FrontFace| {
            (
                face_pipeline_with(
                    "bimble faces, outward, section box",
                    ("vs_face_clip", "fs_face_clip"),
                    Some(wgpu::Face::Back),
                    front,
                    (2, 1.5),
                ),
                face_pipeline_with(
                    "bimble faces, inward (cut surfaces), section box",
                    ("vs_face_clip", "fs_face_cut"),
                    Some(wgpu::Face::Front),
                    front,
                    (-2, 4.0),
                ),
            )
        };
        let (face_front_pipeline, face_back_pipeline) = clip_pipelines(ccw);
        let (face_front_mirrored, face_back_mirrored) = clip_pipelines(wgpu::FrontFace::Cw);

        // Normal viewing reads only positions; the section box also reads the face normals.
        let line_attrs_lean = wgpu::vertex_attr_array![0 => Float32x3];
        let line_attrs_clip =
            wgpu::vertex_attr_array![0 => Float32x3, 1 => Snorm8x4, 2 => Snorm8x4];
        let line_pipeline_with =
            |label: &str,
             (vs, fs): (&str, &str),
             stride: usize,
             attrs: &[wgpu::VertexAttribute]| {
                device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                    label: Some(label),
                    layout: Some(&layout),
                    vertex: wgpu::VertexState {
                        module: &shader,
                        entry_point: Some(vs),
                        compilation_options: Default::default(),
                        buffers: &[Some(wgpu::VertexBufferLayout {
                            array_stride: stride as u64,
                            step_mode: wgpu::VertexStepMode::Vertex,
                            attributes: attrs,
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
                        entry_point: Some(fs),
                        compilation_options: Default::default(),
                        targets: &target,
                    }),
                    multiview_mask: None,
                    cache: None,
                })
            };
        let line_pipeline =
            line_pipeline_with("bimble edges", ("vs_line", "fs_line"), 12, &line_attrs_lean);
        let line_clip_lean_pipeline = line_pipeline_with(
            "bimble edges, section box, no normals",
            ("vs_line_clip_lean", "fs_line_clip"),
            12,
            &line_attrs_lean,
        );
        // Edges with normals, drawn without the box (the box was used, then switched off).
        let line_wide_pipeline = line_pipeline_with(
            "bimble edges (with normals)",
            ("vs_line", "fs_line"),
            std::mem::size_of::<LineVertex>(),
            &line_attrs_lean,
        );
        let line_clip_pipeline = line_pipeline_with(
            "bimble edges, section box",
            ("vs_line_clip", "fs_line_clip"),
            std::mem::size_of::<LineVertex>(),
            &line_attrs_clip,
        );

        Renderer {
            face_pipeline,
            face_front_pipeline,
            face_back_pipeline,
            face_front_mirrored,
            face_back_mirrored,
            line_pipeline,
            line_wide_pipeline,
            line_clip_pipeline,
            line_clip_lean_pipeline,
            layer_uniforms,
            samples,
        }
    }

    /// Upload the visible part of a scene. `style(i, element)` decides how each element is drawn.
    pub fn upload(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        scene: &Scene,
        style: impl Fn(usize, &SceneElement) -> Style,
    ) -> GpuScene {
        self.upload_with(device, queue, scene, style, false)
    }

    /// Like `upload`; with `edge_normals`, edges also carry the normals of their faces, which the
    /// section box needs to hide edges behind their own solid. They cost 8 bytes per edge
    /// vertex (12 → 20), so they are only uploaded when the box is used.
    pub fn upload_with(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        scene: &Scene,
        style: impl Fn(usize, &SceneElement) -> Style,
        edge_normals: bool,
    ) -> GpuScene {
        let mut faces = Chunks::<FaceVertex>::new(device, queue, "bimble face vb");
        let mut lines = Chunks::<LineVertex>::new(device, queue, "bimble line vb");
        let mut lines_lean = Chunks::<[f32; 3]>::new(device, queue, "bimble line vb");
        for (i, e) in scene.elements.iter().enumerate() {
            let s = style(i, e);
            if s == Style::Hidden {
                continue;
            }
            let slot = match s {
                Style::Selected => style_gpu::SELECTED_SLOT,
                Style::Reference => style_gpu::REFERENCE_SLOT,
                Style::OnlyHere => style_gpu::ONLY_HERE_SLOT,
                Style::OnlyInReference => style_gpu::ONLY_IN_REFERENCE_SLOT,
                Style::Normal | Style::Hidden => style_gpu::slot(e.category),
            };
            for t in e.mesh.indices.as_chunks::<3>().0 {
                let [a, b, c] = t.map(|k| e.mesh.positions[k as usize]);
                let n = (Vec3::from(b) - Vec3::from(a))
                    .cross(Vec3::from(c) - Vec3::from(a))
                    .normalize_or(Vec3::Z);
                let q = |x: f32| ((x * 0.5 + 0.5) * 255.0).round() as u8;
                let normal_slot = [q(n.x), q(n.y), q(n.z), slot];
                for pos in [a, b, c] {
                    faces.push(FaceVertex { pos, normal_slot });
                }
            }
            if edge_normals {
                for (k, seg) in e.mesh.edges.iter().enumerate() {
                    let [n1, n2] = e.mesh.edge_normals.get(k).copied().unwrap_or_default();
                    for &pos in seg {
                        lines.push(LineVertex { pos, n1, n2 });
                    }
                }
            } else {
                for &pos in e.mesh.edges.iter().flatten() {
                    lines_lean.push(pos);
                }
            }
        }
        GpuScene {
            faces: faces.finish(),
            lines: if edge_normals {
                lines.finish()
            } else {
                lines_lean.finish()
            },
            edge_normals,
            bounds: Sphere::of(scene),
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
        self.render_layers(
            device,
            queue,
            target,
            &[Layer::main(scene)],
            &scene.bounds,
            camera,
            opts,
        );
    }

    /// Draw several scenes in one image: the model first, then linked models. `bounds` must
    /// contain all of them (it sets the depth range). At most `MAX_LAYERS` layers are drawn.
    #[allow(clippy::too_many_arguments)]
    pub fn render_layers(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &Target,
        layers: &[Layer],
        bounds: &Sphere,
        camera: &Camera,
        opts: &DrawOptions,
    ) {
        let aspect = target.size.0 as f32 / target.size.1 as f32;
        let vp = camera.view_proj(aspect, bounds);
        // Light from over the viewer's left shoulder.
        let light = (camera.eye() - camera.target).normalize() + Vec3::new(-0.3, -0.2, 0.9);
        let st = &opts.style;
        let layers = &layers[..layers.len().min(MAX_LAYERS)];
        for (layer, (buf, _)) in layers.iter().zip(&self.layer_uniforms) {
            // The layer's coordinates + offset = the model's scene coordinates.
            let off = layer.offset;
            let edge = layer.edge_color.unwrap_or(st.edges).unit();
            let u = Uniforms {
                view_proj: (vp * Mat4::from_translation(off)).to_cols_array_2d(),
                light: [light.x, light.y, light.z, 0.0],
                edge: [edge[0], edge[1], edge[2], 1.0],
                params: [
                    opts.hatch_scale,
                    if opts.hatching {
                        st.hatch_strength
                    } else {
                        0.0
                    },
                    st.ambient,
                    st.cut_shade,
                ],
                clip_min: match opts.clip {
                    Some(c) => {
                        let m = c.min - off;
                        [m.x, m.y, m.z, 1.0]
                    }
                    None => [0.0; 4],
                },
                clip_max: match opts.clip {
                    Some(c) => {
                        let m = c.max - off;
                        [m.x, m.y, m.z, 0.0]
                    }
                    None => [0.0; 4],
                },
                eye: {
                    let e = camera.eye() - off;
                    [e.x, e.y, e.z, if camera.orthographic { 1.0 } else { 0.0 }]
                },
                view_dir: {
                    let d = (camera.target - camera.eye()).normalize_or(Vec3::NEG_Z);
                    [d.x, d.y, d.z, 0.0]
                },
                // The model draws at its depth; linked models a hair behind, so where both
                // have the same face the model wins instead of a patchwork of both.
                layer: [
                    if layer.linked { LINKED_DEPTH_PUSH } else { 0.0 },
                    0.0,
                    0.0,
                    0.0,
                ],
                table: style_gpu::gpu_table(st),
            };
            queue.write_buffer(buf, 0, bytemuck::bytes_of(&u));
        }

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
            // Linked models first, then the others in order: the model's edges are drawn over
            // theirs where they coincide (edges don't write depth), highlights last.
            let paired: Vec<_> = layers.iter().zip(&self.layer_uniforms).collect();
            let order = paired
                .iter()
                .filter(|(l, _)| l.linked)
                .chain(paired.iter().filter(|(l, _)| !l.linked));
            for (layer, (_, bind_group)) in order {
                let scene = layer.scene;
                pass.set_bind_group(0, bind_group, &[]);
                if layer.faces {
                    let pipelines: &[&wgpu::RenderPipeline] = match (opts.clip, camera.mirror_x) {
                        (None, _) => &[&self.face_pipeline],
                        (Some(_), false) => &[&self.face_front_pipeline, &self.face_back_pipeline],
                        (Some(_), true) => &[&self.face_front_mirrored, &self.face_back_mirrored],
                    };
                    for p in pipelines {
                        pass.set_pipeline(p);
                        for (buf, n) in &scene.faces {
                            pass.set_vertex_buffer(0, buf.slice(..));
                            pass.draw(0..*n, 0..1);
                        }
                    }
                }
                if (opts.edges || !layer.faces) && !scene.lines.is_empty() {
                    pass.set_pipeline(match (opts.clip.is_some(), scene.edge_normals) {
                        (true, true) => &self.line_clip_pipeline,
                        // Without normals, back edges can't be told apart: clipped, but drawn.
                        // The viewer and `render --clip` upload normals when the box is on.
                        (true, false) => &self.line_clip_lean_pipeline,
                        (false, false) => &self.line_pipeline,
                        (false, true) => &self.line_wide_pipeline,
                    });
                    for (buf, n) in &scene.lines {
                        pass.set_vertex_buffer(0, buf.slice(..));
                        pass.draw(0..*n, 0..1);
                    }
                }
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
    clip: Option<&ClipBox>,
) -> Option<(usize, f32)> {
    pick_hit(scene, visible, origin, dir, clip).map(|h| (h.element, h.distance))
}

/// Where a ray hits the scene.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Hit {
    pub element: usize,
    /// Along the ray, from its origin.
    pub distance: f32,
    /// Hit point, scene coordinates.
    pub point: Vec3,
    /// Unit normal of the hit triangle (as wound; may face away from the ray).
    pub normal: Vec3,
}

/// The nearest visible hit, ignoring what the clipping box removes.
///
/// Where the box cuts a solid, the ray reaches the solid's inside first: what it hits is the cut
/// surface (as drawn), on the box, so a cut column is picked before the slab it stands on.
pub fn pick_hit(
    scene: &Scene,
    visible: impl Fn(usize) -> bool,
    origin: Vec3,
    dir: Vec3,
    clip: Option<&ClipBox>,
) -> Option<Hit> {
    let entry = clip_entry(origin, dir, clip);
    let mut best: Option<Hit> = None;
    for (i, e) in scene.elements.iter().enumerate() {
        if !visible(i) {
            continue;
        }
        if let Some(hit) = element_hit(e, i, origin, dir, clip, entry)
            && best.is_none_or(|h| hit.distance < h.distance)
        {
            best = Some(hit);
        }
    }
    best
}

/// Where a ray enters the clipping box, and through which face (its normal, towards the ray).
fn clip_entry(origin: Vec3, dir: Vec3, clip: Option<&ClipBox>) -> Option<(f32, Vec3)> {
    clip.and_then(|c| {
        let mut t_in = 0.0f32;
        let mut normal = Vec3::ZERO;
        for k in 0..3 {
            if dir[k].abs() < 1e-12 {
                continue;
            }
            let (a, b) = (
                (c.min[k] - origin[k]) / dir[k],
                (c.max[k] - origin[k]) / dir[k],
            );
            let near = a.min(b);
            if near > t_in {
                t_in = near;
                normal = Vec3::ZERO;
                normal[k] = -dir[k].signum();
            }
        }
        (normal != Vec3::ZERO).then_some((t_in, normal))
    })
}

/// `pick_hit` through a tree of the scene's element bounds (`bvh::element_boxes`): only the
/// elements whose box the ray meets are tested, nearest first, and none past the best hit.
pub fn pick_hit_bvh(
    scene: &Scene,
    bvh: &bimble_geom::bvh::Bvh,
    visible: impl Fn(usize) -> bool,
    origin: Vec3,
    dir: Vec3,
    clip: Option<&ClipBox>,
) -> Option<Hit> {
    // Ties (two cut solids entered at the same point) go to the smaller element index, as in
    // `pick_hit`.
    struct ByElement(Hit);
    impl PartialEq for ByElement {
        fn eq(&self, o: &Self) -> bool {
            self.0.element == o.0.element
        }
    }
    impl PartialOrd for ByElement {
        fn partial_cmp(&self, o: &Self) -> Option<std::cmp::Ordering> {
            self.0.element.partial_cmp(&o.0.element)
        }
    }
    let entry = clip_entry(origin, dir, clip);
    bvh.ray(origin.into(), dir.into(), |i, _| {
        let i = i as usize;
        let e = scene.elements.get(i).filter(|_| visible(i))?;
        element_hit(e, i, origin, dir, clip, entry).map(|h| (h.distance, ByElement(h)))
    })
    .map(|(_, h)| h.0)
}

/// The nearest hit of a ray on one element (see `pick_hit`). With a clipping box, an element
/// counts as cut (hit where the ray enters the box) only if the nearest of its triangles the ray
/// meets inside the box faces away, i.e. the ray starts inside that solid; an element wholly
/// behind is hit on its front face as usual.
fn element_hit(
    e: &SceneElement,
    i: usize,
    origin: Vec3,
    dir: Vec3,
    clip: Option<&ClipBox>,
    entry: Option<(f32, Vec3)>,
) -> Option<Hit> {
    let mut nearest: Option<(f32, Vec3)> = None;
    for t in e.mesh.indices.as_chunks::<3>().0 {
        let [a, b, c] = t.map(|k| Vec3::from(e.mesh.positions[k as usize]));
        let Some(d) = ray_triangle(origin, dir, a, b, c) else {
            continue;
        };
        if clip.is_some_and(|c| !c.contains(origin + dir * d)) {
            continue;
        }
        if nearest.is_none_or(|(n, _)| d < n) {
            nearest = Some((d, (b - a).cross(c - a).normalize_or_zero()));
        }
    }
    let (d, normal) = nearest?;
    Some(match entry {
        // Seen from inside (meshes are wound outward): the solid is cut, its front removed.
        Some((t_in, n)) if normal.dot(dir) > 0.0 && t_in < d => Hit {
            element: i,
            distance: t_in,
            point: origin + dir * t_in,
            normal: n,
        },
        _ => Hit {
            element: i,
            distance: d,
            point: origin + dir * d,
            normal,
        },
    })
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
        Self::with_max_buffer_size(None)
    }

    /// `max_buffer_size`: a lower limit than the default 256 MiB, to test how large scenes split.
    fn with_max_buffer_size(max_buffer_size: Option<u64>) -> Result<Headless, HeadlessError> {
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
            required_limits: {
                let mut l = wgpu::Limits::downlevel_defaults().using_resolution(adapter.limits());
                if let Some(m) = max_buffer_size {
                    l.max_buffer_size = m;
                }
                l
            },
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
        assert!(pick(&scene, |_| true, o, d, None).is_some());
        // Looking away from the scene hits nothing.
        assert!(pick(&scene, |_| true, o, -d, None).is_none());
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

    /// Picking through the tree finds what testing every element finds, with and without a
    /// section box.
    #[test]
    fn picking_through_the_tree_matches_testing_everything() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let bvh = bimble_geom::bvh::Bvh::build(&bimble_geom::bvh::element_boxes(&scene));
        let bounds = Sphere::of(&scene);
        let mut cam = Camera::default();
        cam.fit(&scene, 1.5);
        let b = ClipBox::around(&scene, |_| true).unwrap();
        let half = ClipBox {
            min: b.min,
            max: (b.min + b.max) / 2.0 + Vec3::new(0.0, 0.0, 1000.0),
        };
        let mut hits = 0;
        for clip in [None, Some(&half)] {
            for k in 0..400 {
                let ndc = [(k % 20) as f32 / 10.0 - 0.95, (k / 20) as f32 / 10.0 - 0.95];
                let (o, d) = cam.ray(ndc, 1.5, &bounds);
                let brute = pick_hit(&scene, |_| true, o, d, clip);
                let tree = pick_hit_bvh(&scene, &bvh, |_| true, o, d, clip);
                assert_eq!(
                    brute.map(|h| (h.element, h.distance)),
                    tree.map(|h| (h.element, h.distance)),
                    "ray {k}, clip {}",
                    clip.is_some()
                );
                hits += brute.is_some() as usize;
            }
        }
        assert!(hits > 50, "{hits}");
    }

    #[test]
    fn orbiting_about_a_point_keeps_it_on_screen() {
        let bounds = Sphere {
            center: Vec3::ZERO,
            radius: 20_000.0,
        };
        let ndc = |c: &Camera, p: Vec3| c.view_proj(1.5, &bounds).project_point3(p).truncate();
        for mirror_x in [false, true] {
            let mut cam = Camera {
                mirror_x,
                ..Camera::default()
            };
            cam.pan(0.1, -0.05);
            let pivot = cam
                .eye()
                .lerp(cam.on_target_plane([0.5, -0.3], 1.5, &bounds), 0.6);
            let eye_to_pivot = (pivot - cam.eye()).length();
            let off_axis = |c: &Camera| (pivot - c.eye()).angle_between(c.target - c.eye());
            for (dx, dy) in [(0.05, 0.0), (0.0, 0.04), (-0.1, -0.07)] {
                let before = ndc(&cam, pivot);
                cam.orbit_about(dx, dy, pivot);
                let after = ndc(&cam, pivot);
                assert!(
                    (before - after).length() < 1e-3,
                    "mirror={mirror_x} ({dx}, {dy}): {before} -> {after}"
                );
                assert!(((pivot - cam.eye()).length() - eye_to_pivot).abs() < 1.0);
            }
            // Into the pitch clamp: only the turn actually made is applied. (Near vertical the
            // view's up switches to +Y, which rolls the picture; the pivot's angle from the
            // view axis and its distance don't change.)
            let a = off_axis(&cam);
            cam.orbit_about(0.02, 3.0, pivot);
            assert!((off_axis(&cam) - a).abs() < 1e-4);
            assert!(((pivot - cam.eye()).length() - eye_to_pivot).abs() < 1.0);
        }
    }

    #[test]
    fn zooming_keeps_the_pivot_under_the_cursor() {
        let bounds = Sphere {
            center: Vec3::ZERO,
            radius: 20_000.0,
        };
        let ndc = |c: &Camera, p: Vec3| c.view_proj(1.5, &bounds).project_point3(p).truncate();
        for (ortho, mirror_x) in [(false, false), (true, false), (false, true), (true, true)] {
            for factor in [0.5, 1.7, 1e-6, 1e6] {
                let mut cam = Camera {
                    orthographic: ortho,
                    mirror_x,
                    ..Camera::default()
                };
                cam.pan(0.1, -0.05);
                // A point on the target plane, and one in front of it (picked geometry).
                for pivot in [
                    cam.on_target_plane([0.6, -0.3], 1.5, &bounds),
                    cam.eye()
                        .lerp(cam.on_target_plane([-0.4, 0.5], 1.5, &bounds), 0.3),
                ] {
                    let mut c = cam;
                    let before = ndc(&c, pivot);
                    c.zoom_at(factor, pivot);
                    let after = ndc(&c, pivot);
                    assert!(
                        (before - after).length() < 1e-3,
                        "ortho={ortho} mirror={mirror_x} f={factor}: {before} -> {after}"
                    );
                    assert!((10.0..=1e8).contains(&c.distance));
                }
            }
        }
    }

    /// A scene larger than the device's buffer size limit is split, not a validation panic
    /// (a 200 MB IFC made a 742 MB face buffer against 256 MiB), and draws the same picture.
    #[test]
    fn scenes_larger_than_a_gpu_buffer_are_split() {
        let draw = |gpu: &Headless| {
            let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
            let r = Renderer::new(&gpu.device, 1);
            let gs = r.upload_with(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal, true);
            let target = Target::new(&gpu.device, (64, 40), 1);
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
            (
                gs.faces.len(),
                gs.lines.len(),
                gs.triangles(),
                gpu.read_rgba(&target).unwrap(),
            )
        };
        let (Ok(big), Ok(small)) = (
            Headless::new(),
            Headless::with_max_buffer_size(Some(16_384)),
        ) else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let (f1, l1, t1, px1) = draw(&big);
        let (f2, l2, t2, px2) = draw(&small);
        assert_eq!((f1, l1), (1, 1));
        assert!(
            f2 > 1 && l2 > 1,
            "split into {f2} face and {l2} line buffers"
        );
        assert_eq!(t1, t2);
        assert!(px1 == px2, "same picture whatever the split");
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
        let gs = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
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
        draw_with(gpu, scene, opts, |_| {})
    }

    fn draw_with(
        gpu: &Headless,
        scene: &Scene,
        opts: &DrawOptions,
        camera: impl Fn(&mut Camera),
    ) -> Vec<[u8; 4]> {
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload_with(
            &gpu.device,
            &gpu.queue,
            scene,
            |_, _| Style::Normal,
            opts.clip.is_some(),
        );
        let target = Target::new(&gpu.device, (400, 200), 1);
        let mut cam = Camera::default();
        cam.fit(scene, 2.0);
        camera(&mut cam);
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
            style: style.clone(),
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

        // Hatching changes faces only. A dense hatch (half the area inked) on every family, so the
        // share of changed face pixels does not depend on the GPU's rasterisation details.
        let mut dense = GraphicStyle {
            hatch_strength: 1.0,
            ..style
        };
        for c in MaterialCategory::ALL {
            let f = dense.families.get_mut(c);
            f.lines = [
                Some(Lines {
                    angle_deg: 45.0,
                    spacing_px: 4.0,
                    width: 0.5,
                    dash_px: None,
                    dash_on: 1.0,
                }),
                None,
            ];
            f.stipple = None;
            f.ink = Some(Rgb([0, 0, 0]));
        }
        let hatched = DrawOptions {
            style: dense,
            ..opts
        };
        let px2 = draw(&gpu, &scene, &hatched);
        let faces = px.len() - count(&px, style.background, 0);
        let changed = px.iter().zip(&px2).filter(|(a, b)| a != b).count();
        eprintln!("hatching changed {changed} of {faces} face pixels");
        assert!(
            changed * 4 > faces,
            "hatching changed only {changed} of {faces} face pixels"
        );
        assert_eq!(
            count(&px2, style.background, 0),
            count(&px, style.background, 0)
        );
    }

    fn srgb_to_linear(c: f32) -> f32 {
        if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    }
    fn linear_to_srgb(c: f32) -> f32 {
        if c <= 0.0031308 {
            c * 12.92
        } else {
            1.055 * c.powf(1.0 / 2.4) - 0.055
        }
    }

    /// The section box hides what is outside and shows the cut surface of solids it cuts.
    #[test]
    fn clip_box_cuts_solids_and_shows_the_cut() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = family_boxes();
        let all = ClipBox::around(&scene, |_| true).unwrap();
        // Keep the lower half of every box (boxes are 0..1000 mm high).
        let half = ClipBox {
            max: Vec3::new(all.max.x, all.max.y, all.min.z + 500.0),
            ..all
        };
        let fill = Rgb([200, 60, 60]);
        let mut style = GraphicStyle {
            ambient: 1.0,
            hatch_strength: 0.0,
            cut_shade: 0.5,
            ..GraphicStyle::default()
        };
        for c in MaterialCategory::ALL {
            style.families.get_mut(c).color = fill;
        }
        let mut opts = DrawOptions {
            style,
            edges: false,
            ..Default::default()
        };
        let whole = draw(&gpu, &scene, &opts);
        opts.clip = Some(half);
        let cut = draw(&gpu, &scene, &opts);

        // Cut surface: the fill at `cut_shade` brightness (in linear light), unlit.
        let cut_rgb = Rgb(fill.0.map(|c| {
            (linear_to_srgb(srgb_to_linear(c as f32 / 255.0) * 0.5) * 255.0).round() as u8
        }));
        assert_eq!(count(&whole, cut_rgb, 2), 0, "no cut colour without a box");
        let n_cut = count(&cut, cut_rgb, 2);
        assert!(n_cut > 300, "only {n_cut} cut-surface pixels");
        // Half of every box is gone: fewer object pixels, more background.
        let bg = GraphicStyle::default().background;
        assert!(
            count(&cut, bg, 0) > count(&whole, bg, 0),
            "nothing was removed"
        );

        // A box that contains everything changes nothing.
        opts.clip = Some(all);
        let same = draw(&gpu, &scene, &opts);
        let differ = whole.iter().zip(&same).filter(|(a, b)| a != b).count();
        assert!(
            differ < 50,
            "{differ} pixels differ with an all-enclosing box"
        );
    }

    /// Axis-aligned boxes (min, max in mm) as a scene, one element each.
    fn boxes(list: &[([f64; 3], [f64; 3], MaterialCategory)]) -> Scene {
        use bimble_geom::{ElementInfo, MeshBuilder, SceneBuilder};
        let mut sb = SceneBuilder::default();
        for (k, (lo, hi, c)) in list.iter().enumerate() {
            let p = |i: usize| {
                [
                    if i & 1 == 0 { lo[0] } else { hi[0] },
                    if i & 2 == 0 { lo[1] } else { hi[1] },
                    if i & 4 == 0 { lo[2] } else { hi[2] },
                ]
            };
            let mut mb = MeshBuilder::default();
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
                    name: String::new(),
                    storey: String::new(),
                    material: c.label().into(),
                    category: *c,
                },
                mb,
            );
        }
        sb.finish()
    }

    /// A plan cut through a column standing on a slab shows the column's cut surface, not the
    /// slab's top face under it (the column's bottom, seen from inside, is at the same depth).
    #[test]
    fn cut_surface_wins_over_the_slab_the_column_stands_on() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let c = MaterialCategory::Concrete;
        let column = ([-150.0, -150.0, 0.0], [150.0, 150.0, 3000.0], c);
        let slab = ([-2000.0, -1000.0, -200.0], [2000.0, 1000.0, 0.0], c);
        let plan = |scene: &Scene| {
            // Same frame and cut for both scenes (scene coordinates are relative to the
            // scene's origin, which differs: place the box from it).
            let o = Vec3::new(
                scene.origin_mm[0] as f32,
                scene.origin_mm[1] as f32,
                scene.origin_mm[2] as f32,
            );
            let clip = ClipBox {
                min: Vec3::new(-3000.0, -3000.0, -500.0) - o,
                max: Vec3::new(3000.0, 3000.0, 1200.0) - o,
            };
            let opts = DrawOptions {
                clip: Some(clip),
                edges: false,
                ..Default::default()
            };
            draw_with(&gpu, scene, &opts, |cam| {
                cam.orthographic = true;
                cam.yaw = -std::f32::consts::FRAC_PI_2;
                cam.pitch = std::f32::consts::FRAC_PI_2;
                cam.fit_box(
                    Vec3::new(-2000.0, -1000.0, -200.0) - o,
                    Vec3::new(2000.0, 1000.0, 3000.0) - o,
                    2.0,
                );
            })
        };
        let alone = plan(&boxes(&[column]));
        let on_slab = plan(&boxes(&[column, slab]));
        let w = 400usize;
        let differ = (95..105)
            .flat_map(|y| (195..205).map(move |x| (x, y)))
            .filter(|&(x, y)| {
                let (a, b) = (alone[y * w + x], on_slab[y * w + x]);
                (0..3).any(|k| (a[k] as i32 - b[k] as i32).abs() > 4)
            })
            .count();
        assert_eq!(
            differ, 0,
            "{differ} of 100 pixels of the column's cut differ"
        );
    }

    /// A mirrored camera (plans looking up) draws the same picture flipped left-right, cut
    /// surfaces included: the clip passes use the other winding as front.
    #[test]
    fn mirrored_camera_flips_the_picture_with_cut_surfaces() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = family_boxes();
        let all = ClipBox::around(&scene, |_| true).unwrap();
        let half = ClipBox {
            max: Vec3::new(all.max.x, all.max.y, all.min.z + 500.0),
            ..all
        };
        let opts = DrawOptions {
            clip: Some(half),
            edges: false,
            ..Default::default()
        };
        let plain = draw(&gpu, &scene, &opts);
        let mirrored = draw_with(&gpu, &scene, &opts, |c| c.mirror_x = true);
        let (w, h) = (400usize, 200usize);
        let differ = (0..h)
            .flat_map(|y| (0..w).map(move |x| (x, y)))
            .filter(|&(x, y)| {
                let (a, b) = (plain[y * w + x], mirrored[y * w + (w - 1 - x)]);
                (0..3).any(|k| (a[k] as i32 - b[k] as i32).abs() > 8)
            })
            .count();
        let bg = GraphicStyle::default().background;
        let objects = w * h - count(&plain, bg, 0);
        assert!(objects > 1000, "nothing drawn");
        // Hatch patterns are in screen space, so they don't mirror: compare with a tolerance.
        assert!(
            differ * 10 < objects,
            "{differ} of {objects} object pixels differ from the flipped picture"
        );
    }

    #[test]
    fn picking_ignores_what_the_clip_box_removes() {
        let scene = family_boxes();
        // Straight down through the first box (x 0..1000, y 0..1000, z 0..1000, scene coords).
        // The exact box of element 0 (`around` adds a margin).
        let ps = scene.elements[0]
            .mesh
            .positions
            .iter()
            .map(|p| Vec3::from(*p));
        let b0 = ClipBox {
            min: ps.clone().fold(Vec3::INFINITY, Vec3::min),
            max: ps.fold(Vec3::NEG_INFINITY, Vec3::max),
        };
        let mid = (b0.min + b0.max) / 2.0;
        let o = Vec3::new(mid.x, mid.y, b0.max.z + 5000.0);
        let (i, d) = pick(&scene, |_| true, o, Vec3::NEG_Z, None).unwrap();
        assert_eq!(i, 0);
        assert!((d - 5000.0).abs() < 1.0, "hits the top face first: {d}");
        // With the top half removed, the first hit is the bottom face, seen from inside.
        let lower = ClipBox {
            max: Vec3::new(b0.max.x, b0.max.y, b0.min.z + 500.0),
            ..b0
        };
        let (_, d2) = pick(&scene, |_| true, o, Vec3::NEG_Z, Some(&lower)).unwrap();
        // Seen from inside: the cut surface, on the box (500 mm above the bottom).
        assert!((d2 - 5500.0).abs() < 1.0, "hits the cut surface: {d2}");
        // Box beside the element: nothing to hit.
        let beside = ClipBox {
            min: b0.min + Vec3::new(0.0, 5000.0, 0.0),
            max: b0.max + Vec3::new(0.0, 5000.0, 0.0),
        };
        assert!(pick(&scene, |_| true, o, Vec3::NEG_Z, Some(&beside)).is_none());
    }

    /// Clicking a column cut by a plan selects the column, not the slab it stands on (whose top
    /// is level with the column's bottom).
    #[test]
    fn picking_a_cut_column_on_a_slab_gives_the_column() {
        let c = MaterialCategory::Concrete;
        // The slab first: on equal distances the first element would win.
        let scene = boxes(&[
            ([-2000.0, -1000.0, -200.0], [2000.0, 1000.0, 0.0], c),
            ([-150.0, -150.0, 0.0], [150.0, 150.0, 3000.0], c),
        ]);
        let o = Vec3::new(
            scene.origin_mm[0] as f32,
            scene.origin_mm[1] as f32,
            scene.origin_mm[2] as f32,
        );
        let plan = ClipBox {
            min: Vec3::new(-3000.0, -3000.0, 0.0) - o,
            max: Vec3::new(3000.0, 3000.0, 1200.0) - o,
        };
        let from = Vec3::new(0.0, 0.0, 10_000.0) - o;
        let hit = pick_hit(&scene, |_| true, from, Vec3::NEG_Z, Some(&plan)).unwrap();
        assert_eq!(scene.elements[hit.element].global_id, "box1", "the column");
        assert!(
            (hit.point.z + o.z - 1200.0).abs() < 1.0,
            "on the cut: {:?}",
            hit.point
        );
    }

    #[test]
    fn merged_spheres_contain_both() {
        let a = Sphere {
            center: Vec3::ZERO,
            radius: 1.0,
        };
        let b = Sphere {
            center: Vec3::new(10.0, 0.0, 0.0),
            radius: 2.0,
        };
        let m = a.merge(b);
        for (s, sign) in [(a, -1.0), (b, 1.0)] {
            let far = s.center + Vec3::X * sign * s.radius;
            assert!((far - m.center).length() <= m.radius + 1e-4);
        }
        assert!((m.radius - 6.5).abs() < 1e-5);
        // One inside the other.
        let big = Sphere {
            center: Vec3::ZERO,
            radius: 100.0,
        };
        assert_eq!(big.merge(a), big);
        assert_eq!(a.merge(big), big);
    }

    /// A linked model is drawn at its offset, in the reference colours; edges-only shows no fill.
    #[test]
    fn linked_layers_are_drawn_at_their_offset_in_their_colours() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        // Main: one box. Linked: the same box, 3 m along x in the main scene's coordinates.
        let main = family_boxes();
        let main_one = Scene {
            elements: main.elements[..1].to_vec(),
            ..main.clone()
        };
        let r = Renderer::new(&gpu.device, 1);
        let style = GraphicStyle {
            ambient: 1.0,
            hatch_strength: 0.0,
            ..GraphicStyle::default()
        };
        let opts = DrawOptions {
            style: style.clone(),
            ..Default::default()
        };
        let g_main = r.upload(&gpu.device, &gpu.queue, &main_one, |_, _| Style::Normal);
        let g_ref = r.upload(&gpu.device, &gpu.queue, &main_one, |_, _| Style::Reference);
        let offset = Vec3::new(3000.0, 0.0, 0.0);
        let bounds = g_main.bounds.merge(Sphere {
            center: g_ref.bounds.center + offset,
            radius: g_ref.bounds.radius,
        });
        let target = Target::new(&gpu.device, (400, 200), 1);
        let mut cam = Camera::default();
        let all = ClipBox::around(&main_one, |_| true).unwrap();
        cam.fit_box(all.min, all.max + offset, 2.0);
        let draw = |layers: &[Layer]| {
            r.render_layers(
                &gpu.device,
                &gpu.queue,
                &target,
                layers,
                &bounds,
                &cam,
                &opts,
            );
            gpu.read_rgba(&target).unwrap().as_chunks::<4>().0.to_vec()
        };
        let solid = Layer {
            scene: &g_ref,
            offset,
            faces: true,
            edge_color: Some(style.reference_edges),
            linked: true,
        };
        let px = draw(&[Layer::main(&g_main), solid]);
        let concrete = style.families.concrete.color;
        assert!(count(&px, concrete, 1) > 200, "the model is drawn");
        assert!(
            count(&px, style.reference, 1) > 200,
            "the linked model is drawn"
        );
        assert!(
            count(&px, style.reference_edges, 0) > 50,
            "with its edge colour"
        );
        // Same position (offset 0): the linked box hides behind/at the model's box; offset 3 m it
        // must be beside it: the reference colour sits right of the concrete.
        let mean_x = |c: Rgb| {
            let xs: Vec<usize> = px
                .iter()
                .enumerate()
                .filter(|(_, p)| (0..3).all(|k| (p[k] as i32 - c.0[k] as i32).abs() <= 1))
                .map(|(i, _)| i % 400)
                .collect();
            xs.iter().sum::<usize>() as f32 / xs.len().max(1) as f32
        };
        assert!(mean_x(style.reference) > mean_x(concrete) + 20.0);

        let wire = Layer {
            faces: false,
            ..solid
        };
        let px = draw(&[Layer::main(&g_main), wire]);
        assert_eq!(
            count(&px, style.reference, 1),
            0,
            "edges only: no reference fill"
        );
        assert!(count(&px, style.reference_edges, 0) > 50, "but its edges");

        // Comparing: our only-here colour.
        let g_diff = r.upload(&gpu.device, &gpu.queue, &main_one, |_, _| Style::OnlyHere);
        let px = draw(&[Layer::main(&g_diff)]);
        assert!(count(&px, style.only_here, 1) > 200);
    }
}
