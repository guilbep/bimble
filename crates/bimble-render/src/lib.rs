//! Drawing-style renderer on wgpu: flat-shaded faces, edges, hatch patterns per material family,
//! no textures. Every colour, hatch and light parameter comes from a `GraphicStyle`.
//!
//! The renderer draws into an offscreen `Target`. The viewer shows that texture in its window;
//! `bimble render` reads it back as a PNG. Both paths run the same code.

use bimble_core::MaterialCategory;
use bimble_geom::scene::{Geometry, Placement32};
use bimble_geom::{Scene, SceneElement};
use glam::camera::rh::{proj::directx, view::look_at_mat4};
use glam::{Mat4, Vec3, Vec4};
use std::collections::HashMap;
use std::sync::Arc;
use wgpu::util::DeviceExt;

pub use wgpu;

pub const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
/// Plain (non-sRGB) target holding gamma-encoded colours: what egui expects for textures, what
/// PNG stores, and it needs no sRGB view formats (missing on some old OpenGL drivers).
pub const COLOR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

/// The graphic style is data in `bimble_core::style`; re-exported here for convenience.
pub use bimble_core::style::{self, Fill, GraphicStyle, Lines, Rgb, Stipple};
mod occlusion;
mod style_gpu;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FaceVertex {
    pos: [f32; 3],
    /// a: style table slot (rgb unused). Colours come from the style uniform, so restyling never
    /// re-uploads the model. No normal: vertices are shared by the faces around them (indexed,
    /// ADR 0036 step 2), and the shader finds each face's normal from screen derivatives.
    slot: [u8; 4],
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

/// Where one copy of a drawn mesh goes (ADR 0034): columns of the placement and a translation
/// (scene coordinates), and the element's style slot in `slot[3]` (255: each vertex's own, for
/// merged geometry drawn as one identity instance). 52 bytes.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct InstanceData {
    x: [f32; 3],
    y: [f32; 3],
    z: [f32; 3],
    t: [f32; 3],
    slot: [u8; 4],
}

impl InstanceData {
    const IDENTITY: InstanceData = InstanceData {
        x: [1.0, 0.0, 0.0],
        y: [0.0, 1.0, 0.0],
        z: [0.0, 0.0, 1.0],
        t: [0.0; 3],
        slot: [0, 0, 0, 255],
    };
}

const INSTANCE_ATTRS: [wgpu::VertexAttribute; 5] = wgpu::vertex_attr_array![
    3 => Float32x3, 4 => Float32x3, 5 => Float32x3, 6 => Float32x3, 7 => Unorm8x4
];

/// A shape with at least this many visible copies is drawn instanced; fewer are merged into
/// the shared buffers (a draw call per rarely used shape costs more than its copies: xeokit's
/// measured pitfall, ADR 0034).
const MIN_INSTANCES: usize = 4;

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
            let b = e.bounds();
            if !b.is_empty() {
                lo = lo.min(Vec3::from(b.min));
                hi = hi.max(Vec3::from(b.max));
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

    /// Millimetres one pixel covers at the orbit target, for a view `height_px` high.
    pub fn mm_per_pixel(&self, height_px: u32) -> f32 {
        2.0 * self.distance * (self.fov_y / 2.0).tan() / height_px.max(1) as f32
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
    /// Merged geometry: vertex buffers with their vertex counts, each within the device's
    /// buffer size limit (256 MiB by default): a large model needs several.
    faces: Vec<Faces>,
    lines: Vec<(wgpu::Buffer, u32)>,
    /// Merged elements in spatial clusters (ADR 0036): a cluster off screen or outside the
    /// section box is not drawn. Inside a cluster, elements come largest first, so "everything
    /// at least this big" is a prefix of its range.
    clusters: Vec<Cluster>,
    /// Elements too large for one buffer: always drawn, whole (not indexed: a triangle's
    /// vertices may fall in two buffers).
    huge_faces: Vec<(wgpu::Buffer, u32)>,
    huge_lines: Vec<(wgpu::Buffer, u32)>,
    /// Shared shapes (ADR 0034): their vertices, packed in buffers like the merged ones, and
    /// one draw per shape (and per mirroring) over its copies in `instances`.
    shape_faces: Vec<Faces>,
    shape_lines: Vec<(wgpu::Buffer, u32)>,
    shapes: Vec<ShapeDraw>,
    instances: Option<wgpu::Buffer>,
    /// The instances again on the CPU, with each one's bounding sphere (centre, radius): the
    /// visible ones are copied into `visible` before a frame that leaves some out.
    instance_data: Vec<InstanceData>,
    instance_spheres: Vec<[f32; 4]>,
    /// Their boxes (min, max of the placed shape box's corners), for occlusion.
    instance_boxes: Vec<[Vec3; 2]>,
    visible: Option<wgpu::Buffer>,
    /// The one identity instance merged geometry is drawn with.
    identity: wgpu::Buffer,
    /// Triangles of the largest simple elements, drawn on the CPU to find what they hide
    /// (ADR 0036 #5).
    occluders: Vec<[Vec3; 3]>,
    /// The occluders' triangles per element.
    occluder_elements: Vec<std::ops::Range<u32>>,
    /// Whether edges carry face normals (needed to hide back edges in the section box).
    pub edge_normals: bool,
    /// Bounds of the whole scene (not just the visible part), for stable clip planes.
    pub bounds: Sphere,
}

/// Welded vertices and the indices of their triangles (into this buffer's vertices).
struct Faces {
    vertices: wgpu::Buffer,
    indices: wgpu::Buffer,
    /// Indices: three a triangle.
    n: u32,
}

/// Merged elements near each other, in one run of each vertex buffer.
#[derive(Clone, Debug)]
struct Cluster {
    faces: (usize, std::ops::Range<u32>),
    lines: (usize, std::ops::Range<u32>),
    /// Bounding sphere of its elements: centre, radius.
    sphere: [f32; 4],
    /// Its elements largest first: (bounding radius, face indices and line vertices before it).
    steps: Vec<(f32, u32, u32)>,
    /// Their boxes (min, max), in the same order, and the box of them all (occlusion).
    boxes: Vec<[Vec3; 2]>,
    bbox: [Vec3; 2],
}

impl Cluster {
    /// Face indices and line vertices of its elements at least `min_radius` big (a prefix).
    fn prefix(&self, min_radius: f32) -> (u32, u32) {
        let all = (self.faces.1.len() as u32, self.lines.1.len() as u32);
        if min_radius <= 0.0 {
            return all;
        }
        let k = self.steps.partition_point(|s| s.0 >= min_radius);
        self.steps.get(k).map_or(all, |s| (s.1, s.2))
    }
}

/// Merged elements per cluster: about this many face vertices (32 768 triangles). Fewer
/// clusters, fewer draws; smaller ones, finer culling.
/// (Tests use small ones, so a small model has several.)
const CLUSTER_VERTICES: usize = if cfg!(test) { 3 * 512 } else { 3 * 32_768 };

/// What of a scene one frame draws: a frustum (planes as `n·p + d ≥ 0`, layer coordinates),
/// the section box, and the smallest size drawn.
struct Cull {
    planes: [Vec4; 6],
    clip: Option<(Vec3, Vec3)>,
    min_radius: f32,
    /// What the scene's largest elements hide (ADR 0036 #5).
    occ: Option<occlusion::DepthBuf>,
}

impl Cull {
    /// Planes of a clip-space matrix with depth 0..1 (Gribb–Hartmann), normalized.
    fn new(m: Mat4, clip: Option<(Vec3, Vec3)>, opts: &DrawOptions) -> Cull {
        let min_radius = opts.min_radius;
        if !opts.cull {
            return Cull {
                planes: [Vec4::W; 6],
                clip: None,
                min_radius,
                occ: None,
            };
        }
        let r = [m.row(0), m.row(1), m.row(2), m.row(3)];
        let planes = [
            r[3] + r[0],
            r[3] - r[0],
            r[3] + r[1],
            r[3] - r[1],
            r[2],
            r[3] - r[2],
        ]
        .map(|p| p / p.truncate().length().max(1e-20));
        Cull {
            planes,
            clip,
            min_radius,
            occ: None,
        }
    }

    /// With occlusion culling against the scene's occluders, where it applies: not with a
    /// section box (it cuts occluders away).
    fn occluding(mut self, m: Mat4, aspect: f32, scene: &GpuScene, opts: &DrawOptions) -> Cull {
        if opts.cull && opts.occlusion && opts.clip.is_none() && !scene.occluders.is_empty() {
            self.occ = Some(occlusion::DepthBuf::new(
                m,
                aspect,
                &scene.occluders,
                &scene.occluder_elements,
            ));
        }
        self
    }

    /// Ranges (relative to each cluster's own) of merged elements to draw: per kept cluster, its
    /// elements at least the size threshold, and with occlusion only those it may show, in runs.
    fn runs<'s>(
        &self,
        scene: &'s GpuScene,
    ) -> Vec<(&'s Cluster, std::ops::Range<u32>, std::ops::Range<u32>)> {
        let mut out = Vec::new();
        for c in scene
            .clusters
            .iter()
            .filter(|c| self.keeps(c.sphere) && self.sees(&c.bbox))
        {
            let (nf, nl) = c.prefix(self.min_radius);
            let (Some(occ), false) = (&self.occ, c.steps.is_empty()) else {
                out.push((c, 0..nf, 0..nl));
                continue;
            };
            let end = |k: usize| c.steps.get(k + 1).map_or((nf, nl), |s| (s.1, s.2));
            let mut run: Option<(u32, u32, u32, u32)> = None;
            for (k, s) in c.steps.iter().enumerate() {
                if s.1 >= nf && s.2 >= nl {
                    break;
                }
                let (fe, le) = end(k);
                let (fe, le) = (fe.min(nf), le.min(nl));
                if occ.may_see(&c.boxes[k]) {
                    run = Some(run.map_or((s.1, fe, s.2, le), |r| (r.0, fe, r.2, le)));
                } else if let Some(r) = run.take() {
                    out.push((c, r.0..r.1, r.2..r.3));
                }
            }
            if let Some(r) = run {
                out.push((c, r.0..r.1, r.2..r.3));
            }
        }
        out
    }

    /// Whether anything of a sphere (centre, radius) may be drawn.
    fn keeps(&self, s: [f32; 4]) -> bool {
        let c = Vec3::new(s[0], s[1], s[2]);
        let r = s[3];
        if r < self.min_radius {
            return false;
        }
        if let Some((lo, hi)) = self.clip
            && (c + r).cmplt(lo).any() | (c - r).cmpgt(hi).any()
        {
            return false;
        }
        self.planes.iter().all(|p| p.truncate().dot(c) + p.w >= -r)
    }

    /// Whether a box may be seen past the occluders (true without occlusion).
    fn sees(&self, b: &[Vec3; 2]) -> bool {
        self.occ.as_ref().is_none_or(|o| o.may_see(b))
    }
}

/// One shared shape's draw: where its vertices are, and which instances.
#[derive(Clone, Debug)]
struct ShapeDraw {
    faces: (usize, std::ops::Range<u32>),
    lines: (usize, std::ops::Range<u32>),
    instances: std::ops::Range<u32>,
    /// Its placements mirror: triangles wound the other way.
    mirrored: bool,
    /// The largest bounding radius of its copies (mm): a draw skipped when too small.
    radius: f32,
}

impl GpuScene {
    /// Triangles drawn (each copy of a shape counted).
    pub fn triangles(&self) -> u32 {
        let merged: u32 = self.faces.iter().map(|f| f.n / 3).sum::<u32>()
            + self.huge_faces.iter().map(|(_, n)| n / 3).sum::<u32>();
        let shared: u32 = self
            .shapes
            .iter()
            .map(|d| d.faces.1.len() as u32 / 3 * d.instances.len() as u32)
            .sum();
        merged + shared
    }

    /// Triangles a frame would draw from this camera (culled, as `render` does).
    pub fn visible_triangles(&self, camera: &Camera, aspect: f32, opts: &DrawOptions) -> u64 {
        let m = camera.view_proj(aspect, &self.bounds);
        let cull =
            Cull::new(m, opts.clip.map(|c| (c.min, c.max)), opts).occluding(m, aspect, self, opts);
        let merged: u64 = cull
            .runs(self)
            .iter()
            .map(|(_, f, _)| f.len() as u64 / 3)
            .sum();
        let huge: u64 = self.huge_faces.iter().map(|(_, n)| *n as u64 / 3).sum();
        let shared: u64 = self
            .shapes
            .iter()
            .map(|d| {
                let n = d
                    .instances
                    .clone()
                    .filter(|&i| {
                        cull.keeps(self.instance_spheres[i as usize])
                            && cull.sees(&self.instance_boxes[i as usize])
                    })
                    .count();
                d.faces.1.len() as u64 / 3 * n as u64
            })
            .sum();
        merged + huge + shared
    }

    /// Bytes of geometry on the GPU: vertex, index and instance buffers.
    pub fn gpu_bytes(&self) -> u64 {
        let soup = |v: &Vec<(wgpu::Buffer, u32)>| v.iter().map(|(b, _)| b.size()).sum::<u64>();
        let indexed = |v: &Vec<Faces>| {
            v.iter()
                .map(|f| f.vertices.size() + f.indices.size())
                .sum::<u64>()
        };
        indexed(&self.faces)
            + soup(&self.lines)
            + soup(&self.huge_faces)
            + soup(&self.huge_lines)
            + indexed(&self.shape_faces)
            + soup(&self.shape_lines)
            + self.instances.as_ref().map_or(0, |b| 2 * b.size())
    }

    /// Draw calls a frame makes for faces (merged buffers, then one per shared shape).
    pub fn face_draws(&self) -> usize {
        self.clusters.len() + self.huge_faces.len() + self.shapes.len()
    }
}

/// Per shape draw, the instances a frame draws: `(false, range)` in the scene's instance buffer
/// when all of them are kept (nothing to copy), else `(true, range)` in its visible buffer,
/// where the kept ones are written packed. Without `queue`, nothing is packed: all are drawn.
fn visible_instances(
    scene: &GpuScene,
    cull: &Cull,
    queue: Option<&wgpu::Queue>,
) -> Vec<(bool, std::ops::Range<u32>)> {
    let mut packed: Vec<InstanceData> = Vec::new();
    let out = scene
        .shapes
        .iter()
        .map(|d| {
            if d.radius < cull.min_radius {
                return (false, 0..0);
            }
            if queue.is_none() {
                return (false, d.instances.clone());
            }
            let start = packed.len();
            for i in d.instances.clone() {
                if cull.keeps(scene.instance_spheres[i as usize])
                    && cull.sees(&scene.instance_boxes[i as usize])
                {
                    packed.push(scene.instance_data[i as usize]);
                }
            }
            if packed.len() - start == d.instances.len() {
                packed.truncate(start);
                (false, d.instances.clone())
            } else {
                (true, start as u32..packed.len() as u32)
            }
        })
        .collect();
    if let (Some(b), Some(q), false) = (&scene.visible, queue, packed.is_empty()) {
        q.write_buffer(b, 0, bytemuck::cast_slice(&packed));
    }
    out
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
        if self.cur.len() >= self.per {
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

    /// Push a run that must stay in one buffer (one shape): where it went, (buffer, range).
    /// None if it is larger than a buffer.
    fn push_run(&mut self, run: &[T]) -> Option<(usize, std::ops::Range<u32>)> {
        if run.len() > self.per {
            return None;
        }
        if self.cur.len() + run.len() > self.per {
            self.flush();
        }
        let start = self.cur.len() as u32;
        self.cur.extend_from_slice(run);
        Some((self.done.len(), start..self.cur.len() as u32))
    }

    fn finish(mut self) -> Vec<(wgpu::Buffer, u32)> {
        self.flush();
        self.done
    }
}

/// Indexed faces filled as meshes come, like `Chunks`: each buffer pair is sent to the GPU when
/// full. Indices are u32 and point into their own buffer's vertices, so draws need no base
/// vertex (WebGL2 has none).
struct IndexedChunks<'d> {
    device: &'d wgpu::Device,
    queue: &'d wgpu::Queue,
    label: &'static str,
    per_vertices: usize,
    per_indices: usize,
    vertices: Vec<FaceVertex>,
    indices: Vec<u32>,
    done: Vec<Faces>,
}

impl<'d> IndexedChunks<'d> {
    fn new(device: &'d wgpu::Device, queue: &'d wgpu::Queue, label: &'static str) -> Self {
        let bytes = device.limits().max_buffer_size.min(64 << 20) as usize;
        IndexedChunks {
            device,
            queue,
            label,
            per_vertices: (bytes / std::mem::size_of::<FaceVertex>()).max(3),
            per_indices: (bytes / 4 / 3 * 3).max(3),
            vertices: Vec::new(),
            indices: Vec::new(),
            done: Vec::new(),
        }
    }

    fn flush(&mut self) {
        if self.indices.is_empty() {
            return;
        }
        let init = |contents: &[u8], usage| {
            self.device
                .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some(self.label),
                    contents,
                    usage,
                })
        };
        self.done.push(Faces {
            vertices: init(
                bytemuck::cast_slice(&self.vertices),
                wgpu::BufferUsages::VERTEX,
            ),
            indices: init(
                bytemuck::cast_slice(&self.indices),
                wgpu::BufferUsages::INDEX,
            ),
            n: self.indices.len() as u32,
        });
        self.vertices.clear();
        self.indices.clear();
        // As in `Chunks::flush`: free the staging copies now.
        self.queue.submit([]);
        let _ = self.device.poll(wgpu::PollType::wait_indefinitely());
    }

    /// Push a run (one shape, or one cluster) that must stay in one buffer pair, its indices
    /// relative to its vertices: where its indices went, (buffer, range). None if too large.
    fn push_run(
        &mut self,
        vertices: &[FaceVertex],
        indices: &[u32],
    ) -> Option<(usize, std::ops::Range<u32>)> {
        if vertices.len() > self.per_vertices || indices.len() > self.per_indices {
            return None;
        }
        if self.vertices.len() + vertices.len() > self.per_vertices
            || self.indices.len() + indices.len() > self.per_indices
        {
            self.flush();
        }
        let base = self.vertices.len() as u32;
        let start = self.indices.len() as u32;
        self.vertices.extend_from_slice(vertices);
        self.indices.extend(indices.iter().map(|k| k + base));
        Some((self.done.len(), start..self.indices.len() as u32))
    }

    fn finish(mut self) -> Vec<Faces> {
        self.flush();
        self.done
    }
}

/// A sphere around a mesh's box, placed: the placed box's centre, and its longest placed
/// half-diagonal (exact for turned copies, and for stretched ones such as a longer pipe). Its
/// radius is how big the element is (ADR 0035).
fn sphere(m: &bimble_geom::Mesh, p: Option<&Placement32>) -> [f32; 4] {
    let b = bimble_geom::bvh::Aabb::of_points(&m.positions);
    if b.is_empty() {
        return [0.0; 4];
    }
    let (lo, hi) = (Vec3::from(b.min), Vec3::from(b.max));
    let (c, h) = ((lo + hi) / 2.0, (hi - lo) / 2.0);
    let Some(p) = p else {
        return [c.x, c.y, c.z, h.length()];
    };
    let pc = Vec3::from(p.apply(c.to_array()));
    let r = [(1.0, 1.0), (1.0, -1.0), (-1.0, 1.0), (-1.0, -1.0)]
        .map(|(sy, sz)| Vec3::from(p.rotate([h.x, sy * h.y, sz * h.z])).length())
        .into_iter()
        .fold(0.0, f32::max);
    [pc.x, pc.y, pc.z, r]
}

/// A mesh's welded vertices with a style slot, and its triangles' indices after those already
/// in `vertices`.
fn push_faces(
    m: &bimble_geom::Mesh,
    slot: u8,
    vertices: &mut Vec<FaceVertex>,
    indices: &mut Vec<u32>,
) {
    let base = vertices.len() as u32;
    let slot = [0, 0, 0, slot];
    vertices.extend(m.positions.iter().map(|&pos| FaceVertex { pos, slot }));
    indices.extend(m.indices.iter().map(|k| k + base));
}

/// A mesh's edges as line vertices, with the normals of their faces or without.
fn push_lines(
    m: &bimble_geom::Mesh,
    with_normals: bool,
    mut push: impl FnMut(LineVertex),
    mut push_lean: impl FnMut([f32; 3]),
) {
    if with_normals {
        for (k, seg) in m.edges.iter().enumerate() {
            let [n1, n2] = m.edge_normals.get(k).copied().unwrap_or_default();
            for &pos in seg {
                push(LineVertex { pos, n1, n2 });
            }
        }
    } else {
        for &pos in m.edges.iter().flatten() {
            push_lean(pos);
        }
    }
}

// ---------------------------------------------------------------- target

/// Where a pass draws: colour (and its MSAA resolve), depth, and how they start.
struct Out<'a> {
    size: (u32, u32),
    view: &'a wgpu::TextureView,
    resolve: Option<&'a wgpu::TextureView>,
    depth: &'a wgpu::TextureView,
    clear: wgpu::Color,
    load_depth: bool,
}

/// A transparent picture laid over a `Target`'s (`Renderer::render_overlay`): highlights.
/// Its colour is premultiplied by its alpha, as egui blends.
pub struct Overlay {
    pub color: wgpu::Texture,
    pub color_view: wgpu::TextureView,
    msaa_view: Option<wgpu::TextureView>,
}

impl Overlay {
    /// For a target of this size and sample count.
    pub fn new(device: &wgpu::Device, size: (u32, u32), samples: u32) -> Overlay {
        let size = (size.0.max(1), size.1.max(1));
        let tex = |label, samples, usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: size.0,
                    height: size.1,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: samples,
                dimension: wgpu::TextureDimension::D2,
                format: COLOR_FORMAT,
                view_formats: &[],
                usage,
            })
        };
        let color = tex(
            "bimble overlay",
            1,
            wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC,
        );
        let msaa_view = (samples > 1).then(|| {
            tex(
                "bimble overlay msaa",
                samples,
                wgpu::TextureUsages::RENDER_ATTACHMENT,
            )
            .create_view(&Default::default())
        });
        Overlay {
            color_view: color.create_view(&Default::default()),
            color,
            msaa_view,
        }
    }
}

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
    probe_pipeline: wgpu::RenderPipeline,
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
    /// Skip elements smaller than this (bounding radius, mm): while navigating, small things
    /// are left out to keep the frame rate (0: draw everything).
    pub min_radius: f32,
    /// Leave out what is off screen or outside the section box (ADR 0036). Off only to compare.
    pub cull: bool,
    /// Leave out what the largest elements hide (ADR 0036 #5; off with a section box). It costs
    /// CPU time (1–3 ms a frame on an M4 Pro) and saves GPU time: worth it on slow GPUs only.
    pub occlusion: bool,
}

impl Default for DrawOptions {
    fn default() -> Self {
        DrawOptions {
            hatch_scale: 1.0,
            min_radius: 0.0,
            cull: true,
            occlusion: false,
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
        let instance_layout = wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<InstanceData>() as u64,
            step_mode: wgpu::VertexStepMode::Instance,
            attributes: &INSTANCE_ATTRS,
        };
        // Probing what a finished picture shows (occlusion queries): same depth, nothing written.
        let probe_target = [Some(wgpu::ColorTargetState {
            format: COLOR_FORMAT,
            blend: None,
            write_mask: wgpu::ColorWrites::empty(),
        })];
        let face_pipeline_with = |label: &str,
                                  (vs, fs): (&str, &str),
                                  cull_mode: Option<wgpu::Face>,
                                  front_face: wgpu::FrontFace,
                                  bias: (i32, f32),
                                  probe: bool| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some(vs),
                    compilation_options: Default::default(),
                    buffers: &[
                        Some(wgpu::VertexBufferLayout {
                            array_stride: std::mem::size_of::<FaceVertex>() as u64,
                            step_mode: wgpu::VertexStepMode::Vertex,
                            attributes: &face_attrs,
                        }),
                        Some(instance_layout.clone()),
                    ],
                },
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode,
                    front_face,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FORMAT,
                    depth_write_enabled: Some(!probe),
                    depth_compare: Some(if probe {
                        wgpu::CompareFunction::LessEqual
                    } else {
                        wgpu::CompareFunction::Less
                    }),
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
                    targets: if probe { &probe_target } else { &target },
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        // Imported meshes don't always have consistent winding: draw both sides at once.
        let ccw = wgpu::FrontFace::Ccw;
        let face_pipeline = face_pipeline_with(
            "bimble faces",
            ("vs_face", "fs_face"),
            None,
            ccw,
            (2, 1.5),
            false,
        );
        let probe_pipeline = face_pipeline_with(
            "bimble faces, probe",
            ("vs_face", "fs_face"),
            None,
            ccw,
            (2, 1.5),
            true,
        );
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
                    false,
                ),
                face_pipeline_with(
                    "bimble faces, inward (cut surfaces), section box",
                    ("vs_face_clip", "fs_face_cut"),
                    Some(wgpu::Face::Front),
                    front,
                    (-2, 4.0),
                    false,
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
                        buffers: &[
                            Some(wgpu::VertexBufferLayout {
                                array_stride: stride as u64,
                                step_mode: wgpu::VertexStepMode::Vertex,
                                attributes: attrs,
                            }),
                            Some(instance_layout.clone()),
                        ],
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
            probe_pipeline,
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
        let mut faces = IndexedChunks::new(device, queue, "bimble face buffers");
        let mut lines = Chunks::<LineVertex>::new(device, queue, "bimble line vb");
        let mut lines_lean = Chunks::<[f32; 3]>::new(device, queue, "bimble line vb");
        // Merged elements are pushed largest first, below.
        let mut merged: Vec<([f32; 4], std::borrow::Cow<bimble_geom::Mesh>, u8)> = Vec::new();
        // Copies of shared shapes, by shape, in first-seen order.
        type Copies<'a> = (&'a Arc<bimble_geom::Mesh>, Vec<(Placement32, u8)>);
        let mut groups: Vec<Copies> = Vec::new();
        let mut group_of: HashMap<*const bimble_geom::Mesh, usize> = HashMap::new();
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
            match &e.geometry {
                Geometry::Own(m) => {
                    merged.push((sphere(m, None), std::borrow::Cow::Borrowed(m), slot))
                }
                Geometry::Instance { shape, to_scene } => {
                    let g = *group_of.entry(Arc::as_ptr(shape)).or_insert_with(|| {
                        groups.push((shape, Vec::new()));
                        groups.len() - 1
                    });
                    groups[g].1.push((*to_scene, slot));
                }
            }
        }
        let mut shape_faces = IndexedChunks::new(device, queue, "bimble shape face buffers");
        let mut shape_lines = Chunks::<LineVertex>::new(device, queue, "bimble shape line vb");
        let mut shape_lines_lean = Chunks::<[f32; 3]>::new(device, queue, "bimble shape line vb");
        let mut instances: Vec<InstanceData> = Vec::new();
        let mut instance_spheres: Vec<[f32; 4]> = Vec::new();
        let mut instance_boxes: Vec<[Vec3; 2]> = Vec::new();
        let mut shapes = Vec::new();
        // Copies of small shapes that may occlude, placed (merged ones are in `merged`).
        let mut occluder_copies: Vec<(Arc<bimble_geom::Mesh>, Vec<Placement32>)> = Vec::new();
        for (shape, copies) in groups {
            let (mut fv, mut fi) = (Vec::new(), Vec::new());
            push_faces(shape, 0, &mut fv, &mut fi);
            let (mut lv, mut ll) = (Vec::new(), Vec::new());
            push_lines(shape, edge_normals, |v| lv.push(v), |p| ll.push(p));
            let placed = if copies.len() < MIN_INSTANCES {
                None
            } else {
                let f = shape_faces.push_run(&fv, &fi);
                let l = if edge_normals {
                    shape_lines.push_run(&lv)
                } else {
                    shape_lines_lean.push_run(&ll)
                };
                f.zip(l)
            };
            if placed.is_some() && shape.indices.len() <= 3 * occlusion::MAX_TRIANGLES {
                occluder_copies.push((shape.clone(), copies.iter().map(|c| c.0).collect()));
            }
            let Some((f, l)) = placed else {
                // Few copies (or a shape larger than a buffer): merged, each placed.
                for (p, slot) in copies {
                    let m = p.mesh(shape);
                    merged.push((sphere(&m, None), std::borrow::Cow::Owned(m), slot));
                }
                continue;
            };
            let mut r = 0.0f32;
            for mirrored in [false, true] {
                let start = instances.len() as u32;
                for (p, slot) in copies.iter().filter(|(p, _)| p.mirrors() == mirrored) {
                    let sp = sphere(shape, Some(p));
                    r = r.max(sp[3]);
                    instance_spheres.push(sp);
                    let sb = bimble_geom::bvh::Aabb::of_points(&shape.positions);
                    instance_boxes.push((0..8).fold(
                        [Vec3::INFINITY, Vec3::NEG_INFINITY],
                        |a, i| {
                            let c = Vec3::from(p.apply([0, 1, 2].map(|k| {
                                if i >> k & 1 == 0 {
                                    sb.min[k]
                                } else {
                                    sb.max[k]
                                }
                            })));
                            [a[0].min(c), a[1].max(c)]
                        },
                    ));
                    instances.push(InstanceData {
                        x: p.x,
                        y: p.y,
                        z: p.z,
                        t: p.t,
                        slot: [0, 0, 0, *slot],
                    });
                }
                if instances.len() as u32 > start {
                    shapes.push(ShapeDraw {
                        faces: f.clone(),
                        lines: l.clone(),
                        instances: start..instances.len() as u32,
                        mirrored,
                        radius: r,
                    });
                }
            }
        }
        // Occluders: the largest elements of few triangles, own or copies (ADR 0036 #5).
        let occluders = {
            let mut cand: Vec<(f32, std::borrow::Cow<bimble_geom::Mesh>)> = merged
                .iter()
                .filter(|(_, m, _)| m.indices.len() <= 3 * occlusion::MAX_TRIANGLES)
                .map(|(sp, m, _)| (sp[3], std::borrow::Cow::Borrowed(m.as_ref())))
                .collect();
            for (shape, placements) in &occluder_copies {
                for p in placements {
                    cand.push((
                        sphere(shape, Some(p))[3],
                        std::borrow::Cow::Owned(p.mesh(shape)),
                    ));
                }
            }
            cand.sort_by(|a, b| b.0.total_cmp(&a.0));
            let (mut tris, mut ranges) = (Vec::new(), Vec::new());
            for (_, m) in &cand {
                if tris.len() + m.indices.len() / 3 > occlusion::BUDGET {
                    break;
                }
                let start = tris.len() as u32;
                for t in m.indices.as_chunks::<3>().0 {
                    tris.push(t.map(|k| Vec3::from(m.positions[k as usize])));
                }
                ranges.push(start..tris.len() as u32);
            }
            (tris, ranges)
        };
        // Merged elements in spatial clusters: Morton order of their centres over the scene's
        // bounds, cut every `CLUSTER_VERTICES` face vertices; largest first inside each.
        let bounds = Sphere::of(scene);
        let lo = bounds.center - bounds.radius;
        let cell = (2.0 * bounds.radius).max(1.0) / 1023.0;
        let morton = |c: [f32; 4]| -> u32 {
            let q = |x: f32, o: f32| (((x - o) / cell).clamp(0.0, 1023.0) as u32) & 1023;
            let spread = |mut v: u32| {
                v = (v | (v << 16)) & 0x0300_00ff;
                v = (v | (v << 8)) & 0x0300_f00f;
                v = (v | (v << 4)) & 0x030c_30c3;
                (v | (v << 2)) & 0x0924_9249
            };
            spread(q(c[0], lo.x)) | (spread(q(c[1], lo.y)) << 1) | (spread(q(c[2], lo.z)) << 2)
        };
        merged.sort_by_cached_key(|e| morton(e.0));
        let mut huge_faces = Chunks::<FaceVertex>::new(device, queue, "bimble huge face vb");
        let mut huge_lines = Chunks::<LineVertex>::new(device, queue, "bimble huge line vb");
        let mut huge_lines_lean = Chunks::<[f32; 3]>::new(device, queue, "bimble huge line vb");
        let mut clusters = Vec::new();
        let mut rest = &mut merged[..];
        // A cluster fits in a buffer (small on some devices): faces and edges both.
        let face_cap = CLUSTER_VERTICES.min(faces.per_indices);
        let line_cap = if edge_normals {
            lines.per
        } else {
            lines_lean.per
        };
        while !rest.is_empty() {
            // The next cluster: elements until the vertex budget (at least one).
            let (mut n, mut fv_n, mut lv_n) = (0, 0, 0);
            while n < rest.len() {
                let (f, l) = (rest[n].1.indices.len(), 2 * rest[n].1.edges.len());
                if n > 0 && (fv_n + f > face_cap || lv_n + l > line_cap) {
                    break;
                }
                (fv_n, lv_n) = (fv_n + f, lv_n + l);
                n += 1;
            }
            let (group, tail) = rest.split_at_mut(n);
            rest = tail;
            group.sort_by(|a, b| b.0[3].total_cmp(&a.0[3]));
            let (mut fv, mut fi, mut lv, mut ll) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
            let mut steps = Vec::with_capacity(group.len());
            let mut boxes = Vec::with_capacity(group.len());
            let mut ball: Option<Sphere> = None;
            for (sp, m, slot) in group.iter() {
                steps.push((sp[3], fi.len() as u32, (lv.len() + ll.len()) as u32));
                let b = bimble_geom::bvh::Aabb::of_points(&m.positions);
                boxes.push([Vec3::from(b.min), Vec3::from(b.max)]);
                push_faces(m, *slot, &mut fv, &mut fi);
                push_lines(m, edge_normals, |v| lv.push(v), |p| ll.push(p));
                let b = Sphere {
                    center: Vec3::new(sp[0], sp[1], sp[2]),
                    radius: sp[3],
                };
                ball = Some(ball.map_or(b, |a| a.merge(b)));
            }
            let f = faces.push_run(&fv, &fi);
            let l = if edge_normals {
                lines.push_run(&lv)
            } else {
                lines_lean.push_run(&ll)
            };
            match (f, l) {
                (Some(f), Some(l)) => {
                    let b = ball.unwrap_or(Sphere {
                        center: Vec3::ZERO,
                        radius: 0.0,
                    });
                    clusters.push(Cluster {
                        faces: f,
                        lines: l,
                        sphere: [b.center.x, b.center.y, b.center.z, b.radius],
                        steps,
                        bbox: boxes
                            .iter()
                            .fold([Vec3::INFINITY, Vec3::NEG_INFINITY], |a, b| {
                                [a[0].min(b[0]), a[1].max(b[1])]
                            }),
                        boxes,
                    });
                }
                // One element larger than a buffer (a cluster of more is cut before): what
                // doesn't fit streams across buffers of its own; drawn always, whole.
                (f, l) => {
                    if f.is_none() {
                        fi.iter().for_each(|&k| huge_faces.push(fv[k as usize]));
                    }
                    if l.is_none() {
                        lv.into_iter().for_each(|v| huge_lines.push(v));
                        ll.into_iter().for_each(|v| huge_lines_lean.push(v));
                    }
                    clusters.push(Cluster {
                        faces: f.unwrap_or((0, 0..0)),
                        lines: l.unwrap_or((0, 0..0)),
                        sphere: [0.0, 0.0, 0.0, f32::INFINITY],
                        steps: Vec::new(),
                        boxes: Vec::new(),
                        bbox: [Vec3::NEG_INFINITY, Vec3::INFINITY],
                    });
                }
            }
        }
        drop(merged);
        let init = |label, contents: &[u8]| {
            device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some(label),
                contents,
                usage: wgpu::BufferUsages::VERTEX,
            })
        };
        GpuScene {
            faces: faces.finish(),
            lines: if edge_normals {
                lines.finish()
            } else {
                lines_lean.finish()
            },
            shape_faces: shape_faces.finish(),
            shape_lines: if edge_normals {
                shape_lines.finish()
            } else {
                shape_lines_lean.finish()
            },
            clusters,
            huge_faces: huge_faces.finish(),
            huge_lines: if edge_normals {
                huge_lines.finish()
            } else {
                huge_lines_lean.finish()
            },
            shapes,
            instances: (!instances.is_empty())
                .then(|| init("bimble instances", bytemuck::cast_slice(&instances))),
            visible: (!instances.is_empty()).then(|| {
                device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("bimble visible instances"),
                    size: std::mem::size_of_val(&instances[..]) as u64,
                    usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                })
            }),
            instance_data: instances,
            instance_spheres,
            instance_boxes,
            identity: init(
                "bimble identity instance",
                bytemuck::bytes_of(&InstanceData::IDENTITY),
            ),
            occluders: occluders.0,
            occluder_elements: occluders.1,
            edge_normals,
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
    /// The depth is kept, for `render_overlay`.
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
        // The target stores gamma-encoded values, so the sRGB colour is written as is.
        let bg = opts.style.background.0.map(|c| c as f64 / 255.0);
        let (view, resolve) = match &target.msaa_view {
            Some(m) => (m, Some(&target.color_view)),
            None => (&target.color_view, None),
        };
        let out = Out {
            size: target.size,
            view,
            resolve,
            depth: &target.depth_view,
            clear: wgpu::Color {
                r: bg[0],
                g: bg[1],
                b: bg[2],
                a: 1.0,
            },
            load_depth: false,
        };
        self.draw(device, queue, out, layers, bounds, camera, opts);
    }

    /// Draw layers (highlights: the element under the cursor, a selected linked element) into
    /// `overlay`, transparent elsewhere, hidden where the target's last picture is in front:
    /// shown over it, they change without drawing the model again (ADR 0036 #3). The camera,
    /// bounds and options must be those of that picture.
    #[allow(clippy::too_many_arguments)]
    pub fn render_overlay(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &Target,
        overlay: &Overlay,
        layers: &[Layer],
        bounds: &Sphere,
        camera: &Camera,
        opts: &DrawOptions,
    ) {
        let (view, resolve) = match &overlay.msaa_view {
            Some(m) => (m, Some(&overlay.color_view)),
            None => (&overlay.color_view, None),
        };
        let out = Out {
            size: target.size,
            view,
            resolve,
            depth: &target.depth_view,
            clear: wgpu::Color::TRANSPARENT,
            load_depth: true,
        };
        self.draw(device, queue, out, layers, bounds, camera, opts);
    }

    /// Share of the drawn triangles a perfect occlusion culling would leave out (ADR 0036 #1),
    /// per merged element and per shape copy: draws the picture, then each of them again
    /// against its depth with an occlusion query; those with no sample left were hidden. A
    /// measurement (one query and one draw call per element), not for every frame.
    pub fn hidden_share(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &Target,
        scene: &GpuScene,
        camera: &Camera,
        opts: &DrawOptions,
    ) -> f64 {
        self.render(device, queue, target, scene, camera, opts);
        let cull = Cull::new(
            camera.view_proj(target.size.0 as f32 / target.size.1 as f32, &scene.bounds),
            opts.clip.map(|c| (c.min, c.max)),
            opts,
        );
        // (triangles, draw): what each query draws.
        enum Probe {
            Merged(usize, std::ops::Range<u32>),
            Copy(usize, u32),
        }
        let mut probes: Vec<(u64, Probe)> = Vec::new();
        for c in scene.clusters.iter().filter(|c| cull.keeps(c.sphere)) {
            let n = c.prefix(cull.min_radius).0;
            let mut cuts: Vec<u32> = c.steps.iter().map(|s| s.1).filter(|&o| o < n).collect();
            if cuts.is_empty() {
                cuts.push(0);
            }
            cuts.push(n);
            for w in cuts.windows(2).filter(|w| w[1] > w[0]) {
                let r = c.faces.1.start + w[0]..c.faces.1.start + w[1];
                probes.push((r.len() as u64 / 3, Probe::Merged(c.faces.0, r)));
            }
        }
        for (k, d) in scene.shapes.iter().enumerate() {
            for i in d.instances.clone() {
                if cull.keeps(scene.instance_spheres[i as usize]) {
                    probes.push((d.faces.1.len() as u64 / 3, Probe::Copy(k, i)));
                }
            }
        }
        let total: u64 = probes.iter().map(|p| p.0).sum();
        let (view, resolve) = match &target.msaa_view {
            Some(m) => (m, Some(&target.color_view)),
            None => (&target.color_view, None),
        };
        let stride = std::mem::size_of::<InstanceData>() as u64;
        let mut hidden = 0u64;
        // A query set holds at most 4096 queries.
        for chunk in probes.chunks(4096) {
            let set = device.create_query_set(&wgpu::QuerySetDescriptor {
                label: Some("bimble probe"),
                ty: wgpu::QueryType::Occlusion,
                count: chunk.len() as u32,
            });
            let bytes = chunk.len() as u64 * 8;
            let resolved = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("bimble probe resolve"),
                size: bytes,
                usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            });
            let read = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("bimble probe read"),
                size: bytes,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            let mut enc = device.create_command_encoder(&Default::default());
            {
                let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("bimble probe pass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view,
                        depth_slice: None,
                        resolve_target: resolve,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view: &target.depth_view,
                        depth_ops: Some(wgpu::Operations {
                            load: wgpu::LoadOp::Load,
                            store: wgpu::StoreOp::Store,
                        }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: Some(&set),
                    multiview_mask: None,
                });
                pass.set_pipeline(&self.probe_pipeline);
                pass.set_bind_group(0, &self.layer_uniforms[0].1, &[]);
                for (q, (_, p)) in chunk.iter().enumerate() {
                    pass.begin_occlusion_query(q as u32);
                    match p {
                        Probe::Merged(b, r) => {
                            let f = &scene.faces[*b];
                            pass.set_vertex_buffer(0, f.vertices.slice(..));
                            pass.set_vertex_buffer(1, scene.identity.slice(..));
                            pass.set_index_buffer(f.indices.slice(..), wgpu::IndexFormat::Uint32);
                            pass.draw_indexed(r.clone(), 0, 0..1);
                        }
                        Probe::Copy(k, i) => {
                            let d = &scene.shapes[*k];
                            let f = &scene.shape_faces[d.faces.0];
                            let inst = scene.instances.as_ref().expect("shapes have instances");
                            pass.set_vertex_buffer(0, f.vertices.slice(..));
                            pass.set_vertex_buffer(
                                1,
                                inst.slice(*i as u64 * stride..(*i as u64 + 1) * stride),
                            );
                            pass.set_index_buffer(f.indices.slice(..), wgpu::IndexFormat::Uint32);
                            pass.draw_indexed(d.faces.1.clone(), 0, 0..1);
                        }
                    }
                    pass.end_occlusion_query();
                }
            }
            enc.resolve_query_set(&set, 0..chunk.len() as u32, &resolved, 0);
            enc.copy_buffer_to_buffer(&resolved, 0, &read, 0, bytes);
            queue.submit([enc.finish()]);
            let slice = read.slice(..);
            slice.map_async(wgpu::MapMode::Read, |_| {});
            let _ = device.poll(wgpu::PollType::wait_indefinitely());
            let Ok(data) = slice.get_mapped_range() else {
                continue;
            };
            let counts: &[u64] = bytemuck::cast_slice(&data);
            hidden += chunk
                .iter()
                .zip(counts)
                .filter(|(_, n)| **n == 0)
                .map(|(p, _)| p.0)
                .sum::<u64>();
        }
        hidden as f64 / total.max(1) as f64
    }

    #[allow(clippy::too_many_arguments)]
    fn draw(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        out: Out,
        layers: &[Layer],
        bounds: &Sphere,
        camera: &Camera,
        opts: &DrawOptions,
    ) {
        let aspect = out.size.0 as f32 / out.size.1 as f32;
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

        // What each layer draws (ADR 0036): clusters and instances off screen, outside the box
        // or too small are left out; shapes with some left out draw from a packed copy.
        let culls: Vec<Cull> = layers
            .iter()
            .map(|l| {
                let m = vp * Mat4::from_translation(l.offset);
                Cull::new(
                    m,
                    opts.clip.map(|c| (c.min - l.offset, c.max - l.offset)),
                    opts,
                )
                .occluding(m, aspect, l.scene, opts)
            })
            .collect();
        let draws: Vec<Vec<(bool, std::ops::Range<u32>)>> = layers
            .iter()
            .zip(&culls)
            .enumerate()
            .map(|(i, (l, cull))| {
                // Writes land before the frame's one submit: a scene drawn in two layers can
                // hold one packed list, so only its first layer packs; the others draw all.
                let first = !layers[..i].iter().any(|o| std::ptr::eq(o.scene, l.scene));
                visible_instances(l.scene, cull, first.then_some(queue))
            })
            .collect();

        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("bimble"),
        });
        {
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("bimble pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: out.view,
                    depth_slice: None,
                    resolve_target: out.resolve,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(out.clear),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: out.depth,
                    depth_ops: Some(wgpu::Operations {
                        load: if out.load_depth {
                            wgpu::LoadOp::Load
                        } else {
                            wgpu::LoadOp::Clear(1.0)
                        },
                        // Kept for an overlay drawn later.
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            // Linked models first, then the others in order: the model's edges are drawn over
            // theirs where they coincide (edges don't write depth), highlights last.
            let paired: Vec<_> = layers
                .iter()
                .zip(&self.layer_uniforms)
                .zip(culls.iter().zip(&draws))
                .map(|((l, u), cd)| (l, (u, cd)))
                .collect();
            let order = paired
                .iter()
                .filter(|(l, _)| l.linked)
                .chain(paired.iter().filter(|(l, _)| !l.linked));
            let stride = std::mem::size_of::<InstanceData>() as u64;
            for (layer, ((_, bind_group), (cull, shape_draws))) in order {
                let scene = layer.scene;
                // A shape's instances: all from its buffer, or the visible ones packed.
                let inst_slice = |k: usize| {
                    let (packed, r) = &shape_draws[k];
                    let buf = if *packed {
                        scene.visible.as_ref()
                    } else {
                        scene.instances.as_ref()
                    };
                    buf.map(|b| {
                        (
                            b.slice(r.start as u64 * stride..r.end as u64 * stride),
                            r.len() as u32,
                        )
                    })
                };
                pass.set_bind_group(0, bind_group, &[]);
                let runs = cull.runs(scene);
                if layer.faces {
                    // A mirrored camera, or a mirrored shape, turns the winding around.
                    let pipelines = |mirror: bool| -> [&wgpu::RenderPipeline; 2] {
                        match (opts.clip, mirror) {
                            (None, _) => [&self.face_pipeline, &self.face_pipeline],
                            (Some(_), false) => {
                                [&self.face_front_pipeline, &self.face_back_pipeline]
                            }
                            (Some(_), true) => {
                                [&self.face_front_mirrored, &self.face_back_mirrored]
                            }
                        }
                    };
                    let passes = if opts.clip.is_some() { 2 } else { 1 };
                    for k in 0..passes {
                        pass.set_pipeline(pipelines(camera.mirror_x)[k]);
                        pass.set_vertex_buffer(1, scene.identity.slice(..));
                        let mut bound = usize::MAX;
                        for (c, r, _) in &runs {
                            if r.is_empty() {
                                continue;
                            }
                            if bound != c.faces.0 {
                                let f = &scene.faces[c.faces.0];
                                pass.set_vertex_buffer(0, f.vertices.slice(..));
                                pass.set_index_buffer(
                                    f.indices.slice(..),
                                    wgpu::IndexFormat::Uint32,
                                );
                                bound = c.faces.0;
                            }
                            let s = c.faces.1.start;
                            pass.draw_indexed(s + r.start..s + r.end, 0, 0..1);
                        }
                        for (buf, n) in &scene.huge_faces {
                            pass.set_vertex_buffer(0, buf.slice(..));
                            pass.draw(0..*n, 0..1);
                        }
                        for (i, d) in scene.shapes.iter().enumerate() {
                            let Some((inst, n)) = inst_slice(i) else {
                                continue;
                            };
                            if n == 0 {
                                continue;
                            }
                            pass.set_pipeline(pipelines(camera.mirror_x != d.mirrored)[k]);
                            let f = &scene.shape_faces[d.faces.0];
                            pass.set_vertex_buffer(0, f.vertices.slice(..));
                            pass.set_index_buffer(f.indices.slice(..), wgpu::IndexFormat::Uint32);
                            pass.set_vertex_buffer(1, inst);
                            pass.draw_indexed(d.faces.1.clone(), 0, 0..n);
                        }
                    }
                }
                if (opts.edges || !layer.faces)
                    && (!scene.lines.is_empty()
                        || !scene.huge_lines.is_empty()
                        || !scene.shapes.is_empty())
                {
                    pass.set_pipeline(match (opts.clip.is_some(), scene.edge_normals) {
                        (true, true) => &self.line_clip_pipeline,
                        // Without normals, back edges can't be told apart: clipped, but drawn.
                        // The viewer and `render --clip` upload normals when the box is on.
                        (true, false) => &self.line_clip_lean_pipeline,
                        (false, false) => &self.line_pipeline,
                        (false, true) => &self.line_wide_pipeline,
                    });
                    pass.set_vertex_buffer(1, scene.identity.slice(..));
                    for (c, _, r) in &runs {
                        if !r.is_empty() {
                            pass.set_vertex_buffer(0, scene.lines[c.lines.0].0.slice(..));
                            let s = c.lines.1.start;
                            pass.draw(s + r.start..s + r.end, 0..1);
                        }
                    }
                    for (buf, n) in &scene.huge_lines {
                        pass.set_vertex_buffer(0, buf.slice(..));
                        pass.draw(0..*n, 0..1);
                    }
                    for (i, d) in scene.shapes.iter().enumerate() {
                        if d.lines.1.is_empty() {
                            continue;
                        }
                        let Some((inst, n)) = inst_slice(i) else {
                            continue;
                        };
                        if n == 0 {
                            continue;
                        }
                        pass.set_vertex_buffer(0, scene.shape_lines[d.lines.0].0.slice(..));
                        pass.set_vertex_buffer(1, inst);
                        pass.draw(d.lines.1.clone(), 0..n);
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
    let m = e.world_mesh();
    for t in m.indices.as_chunks::<3>().0 {
        let [a, b, c] = t.map(|k| Vec3::from(m.positions[k as usize]));
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
        self.read_texture(&target.color)
    }

    /// RGBA8 rows of a single-sample colour texture (a target's, an overlay's).
    pub fn read_texture(&self, texture: &wgpu::Texture) -> Result<Vec<u8>, HeadlessError> {
        let (w, h) = (texture.width(), texture.height());
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
                texture,
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

    /// The scene with every copy of a shared shape as its own mesh: the generated frame is all
    /// copies once built (ADR 0036 step 3), and these tests are about merged geometry.
    fn own(mut scene: Scene) -> Scene {
        for e in &mut scene.elements {
            e.geometry = Geometry::Own(e.world_mesh().into_owned());
        }
        scene
    }

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
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
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
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
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
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
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
            let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
            let r = Renderer::new(&gpu.device, 1);
            let gs = r.upload_with(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal, true);
            let target = Target::new(&gpu.device, (32, 20), 1);
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
                gs.faces.len() + gs.huge_faces.len(),
                gs.lines.len() + gs.huge_lines.len(),
                gs.triangles(),
                gpu.read_rgba(&target).unwrap(),
            )
        };
        let (Ok(big), Ok(small)) = (
            Headless::new(),
            // Indexed faces of this frame take ~20 KB (were 41 KB unindexed).
            Headless::with_max_buffer_size(Some(8_192)),
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

    /// A shape drawn instanced (ADR 0034) looks as its copies placed on the CPU do: rotated,
    /// one mirrored, with and without a section box (whose passes cull by winding).
    #[test]
    fn instanced_shapes_draw_like_placed_copies() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let demo = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
        let column = demo
            .elements
            .iter()
            .find(|e| e.class == "IfcColumn")
            .unwrap()
            .clone();
        let shape = Arc::new(column.world_mesh().into_owned());
        let (c, s) = (0.6f32.cos(), 0.6f32.sin());
        let placements: Vec<Placement32> = (0..4)
            .map(|k| {
                let mut p = Placement32 {
                    x: [c, s, 0.0],
                    y: [-s, c, 0.0],
                    z: [0.0, 0.0, 1.0],
                    t: [k as f32 * 900.0, 0.0, 0.0],
                };
                if k == 1 {
                    p.x = [-c, -s, 0.0];
                }
                p
            })
            .collect();
        let scene_with = |instanced: bool| Scene {
            elements: placements
                .iter()
                .map(|p| SceneElement {
                    geometry: if instanced {
                        Geometry::Instance {
                            shape: shape.clone(),
                            to_scene: *p,
                        }
                    } else {
                        Geometry::Own(p.mesh(&shape))
                    },
                    ..column.clone()
                })
                .collect(),
            ..demo.clone()
        };
        let r = Renderer::new(&gpu.device, 1);
        let draw = |scene: &Scene, clip: Option<ClipBox>| {
            let gs = r.upload_with(&gpu.device, &gpu.queue, scene, |_, _| Style::Normal, true);
            let target = Target::new(&gpu.device, (480, 320), 1);
            let mut cam = Camera::default();
            cam.fit(scene, 1.5);
            let opts = DrawOptions {
                clip,
                ..DrawOptions::default()
            };
            r.render(&gpu.device, &gpu.queue, &target, &gs, &cam, &opts);
            (
                gs.face_draws(),
                gs.triangles(),
                gpu.read_rgba(&target).unwrap(),
            )
        };
        let (inst, placed) = (scene_with(true), scene_with(false));
        let b = ClipBox::around(&placed, |_| true).unwrap();
        let half = ClipBox {
            min: b.min,
            max: Vec3::new(b.max.x, b.max.y, (b.min.z + b.max.z) / 2.0),
        };
        for clip in [None, Some(half)] {
            let (d1, t1, a) = draw(&inst, clip);
            let (d2, t2, b) = draw(&placed, clip);
            assert_eq!(t1, t2);
            assert_eq!(
                (d1, d2),
                (2, 1),
                "one draw per shape and mirroring, one merged"
            );
            let differ = a
                .as_chunks::<4>()
                .0
                .iter()
                .zip(b.as_chunks::<4>().0)
                .filter(|(p, q)| (0..3).any(|k| p[k].abs_diff(q[k]) > 8))
                .count();
            assert!(
                differ < 30,
                "clip {}: {differ} pixels differ",
                clip.is_some()
            );
        }
    }

    /// Merged geometry is in clusters, largest first inside each: "at least this big" is a
    /// prefix of each cluster that shrinks as the size grows, all of it at 0, none past the
    /// largest element.
    #[test]
    fn small_elements_are_a_suffix_of_each_cluster() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
        let all: u32 = gs.faces.iter().map(|f| f.n).sum();
        let in_clusters: u32 = gs.clusters.iter().map(|c| c.faces.1.len() as u32).sum();
        assert_eq!(in_clusters, all, "every merged vertex is in one cluster");
        let at = |r: f32| -> u32 { gs.clusters.iter().map(|c| c.prefix(r).0).sum() };
        assert_eq!(at(0.0), all);
        assert_eq!(at(1e9), 0);
        for c in &gs.clusters {
            let sizes: Vec<f32> = c.steps.iter().map(|s| s.0).collect();
            assert!(sizes.windows(2).all(|w| w[0] >= w[1]), "largest first");
        }
        let mut last = u32::MAX;
        for r in [100.0, 1000.0, 3000.0, 10_000.0] {
            let f = at(r);
            assert!(f <= last.min(all), "{r}: {f}");
            last = f;
        }
        assert!(at(1000.0) < all, "columns and beams are under 1 m");
    }

    /// Culling (ADR 0036) leaves out only what isn't seen: close-up, turned, orthographic,
    /// mirrored, with the section box and with an offset linked model, frames are the same with
    /// and without it; and from close up it does leave things out (so a cull that never fires
    /// can't pass).
    #[test]
    fn culling_changes_nothing_on_screen() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let spec = FrameSpec {
            bays_x: 12,
            bays_y: 12,
            storeys: 3,
            ..Default::default()
        };
        let mut scene = own(Scene::from_model(&concrete_frame(&spec)));
        // Shared shapes too: a column placed on a grid, one copy in four mirrored.
        let column = scene
            .elements
            .iter()
            .find(|e| e.class == "IfcColumn")
            .unwrap()
            .clone();
        let shape = Arc::new(column.world_mesh().into_owned());
        let (lo, hi) = scene.bounds().unwrap();
        for k in 0..64 {
            let (i, j) = ((k % 8) as f32, (k / 8) as f32);
            let mut p = Placement32 {
                x: [1.0, 0.0, 0.0],
                y: [0.0, 1.0, 0.0],
                z: [0.0, 0.0, 1.0],
                t: [
                    (hi[0] - lo[0]) * (i / 8.0 - 0.4),
                    (hi[1] - lo[1]) * (j / 8.0 - 0.4),
                    0.0,
                ],
            };
            if k % 4 == 1 {
                p.x = [-1.0, 0.0, 0.0];
            }
            scene.elements.push(SceneElement {
                geometry: Geometry::Instance {
                    shape: shape.clone(),
                    to_scene: p,
                },
                ..column.clone()
            });
        }
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload_with(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal, true);
        assert!(gs.clusters.len() > 1, "{} clusters", gs.clusters.len());
        assert!(!gs.shapes.is_empty());
        let target = Target::new(&gpu.device, (160, 100), 1);
        let aspect = 1.6;
        let mut fit = Camera::default();
        fit.fit(&scene, aspect);
        let b = ClipBox::around(&scene, |_| true).unwrap();
        let half = ClipBox {
            min: b.min,
            max: Vec3::new((b.min.x + b.max.x) / 2.0, b.max.y, b.max.z),
        };
        // Half a model aside: close up, both copies are partly on screen and both cull.
        let link_off = Vec3::new((b.max.x - b.min.x) / 2.0, 0.0, 0.0);
        let (mut cut_some, mut occluded_some) = (false, false);
        for k in 0..12 {
            let mut cam = fit;
            cam.distance /= [1.0, 4.0, 20.0][k % 3];
            cam.orbit(k as f32 * 0.9, -0.2 * (k % 2) as f32);
            cam.orthographic = k % 4 == 3;
            cam.mirror_x = k % 5 == 4;
            let clip = (k % 3 == 1).then_some(half);
            let shot = |cull: bool| {
                let opts = DrawOptions {
                    clip,
                    cull,
                    occlusion: true,
                    ..DrawOptions::default()
                };
                let mut link = Layer::main(&gs);
                link.offset = link_off;
                link.linked = true;
                let bounds = Sphere {
                    center: gs.bounds.center + link_off / 2.0,
                    radius: gs.bounds.radius + link_off.length() / 2.0,
                };
                r.render_layers(
                    &gpu.device,
                    &gpu.queue,
                    &target,
                    &[Layer::main(&gs), link],
                    &bounds,
                    &cam,
                    &opts,
                );
                gpu.read_rgba(&target).unwrap()
            };
            let (on, off) = (shot(true), shot(false));
            let differ = on
                .as_chunks::<4>()
                .0
                .iter()
                .zip(off.as_chunks::<4>().0)
                // Shading from screen derivatives (ADR 0036 step 2) can move a pixel on a
                // triangle's border by a few levels when draws are split differently; a missing
                // element or edge changes it far more.
                .filter(|(p, q)| (0..3).any(|c| p[c].abs_diff(q[c]) > 40))
                .count();
            assert_eq!(differ, 0, "camera {k}: {differ} pixels differ");
            let opts = DrawOptions {
                clip,
                ..DrawOptions::default()
            };
            // The frame's GlobalIds are random, so its element order and clusters change from
            // run to run: close up, 14–36 % are left out.
            if gs.visible_triangles(&cam, aspect, &opts) * 10 < gs.triangles() as u64 * 8 {
                cut_some = true;
            }
            let opts = DrawOptions {
                occlusion: true,
                ..opts
            };
            let open = DrawOptions {
                occlusion: false,
                ..opts.clone()
            };
            if gs.visible_triangles(&cam, aspect, &opts) < gs.visible_triangles(&cam, aspect, &open)
            {
                occluded_some = true;
            }
        }
        assert!(cut_some, "close up, culling leaves things out");
        assert!(
            occluded_some,
            "occlusion culling leaves things out somewhere"
        );
    }

    /// Vertices are shared by the faces around them (ADR 0036 step 2): the shader still shades
    /// each face flat, from its own plane. A column seen from a corner: its three sides and its
    /// edges, four colours; interpolating anything per vertex would show a gradient. (Without
    /// edges, the one-pixel line where two faces meet takes in-between shades: the edges cover
    /// it.)
    #[test]
    fn welded_faces_are_shaded_flat() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let mut scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
        scene.elements.retain(|e| e.class == "IfcColumn");
        scene.elements.truncate(1);
        let Geometry::Own(m) = &scene.elements[0].geometry else {
            panic!("a column has its own mesh");
        };
        assert!(m.positions.len() * 2 < m.indices.len(), "welded");
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
        let target = Target::new(&gpu.device, (256, 256), 1);
        let mut cam = Camera::default();
        cam.fit(&scene, 1.0);
        let opts = DrawOptions {
            hatching: false,
            ..DrawOptions::default()
        };
        r.render(&gpu.device, &gpu.queue, &target, &gs, &cam, &opts);
        let px = gpu.read_rgba(&target).unwrap();
        let bg = opts.style.background.0;
        let mut counts: HashMap<[u8; 4], usize> = HashMap::new();
        for p in px.as_chunks::<4>().0 {
            if p[..3] != bg[..] {
                *counts.entry(*p).or_default() += 1;
            }
        }
        let mut n: Vec<usize> = counts.into_values().collect();
        n.sort_unstable_by(|a, b| b.cmp(a));
        let all: usize = n.iter().sum();
        // Three sides and the edges: 98 % of the pixels in four colours.
        let top: usize = n.iter().take(4).sum();
        assert!(all > 2000, "{all} pixels drawn");
        assert!(
            top * 100 >= all * 98,
            "{top} of {all} pixels in 4 colours: {n:?}"
        );
    }

    /// The occlusion measurement (ADR 0036 #1): an element alone is never hidden; a column
    /// behind a wall of others mostly is.
    #[test]
    fn hidden_share_counts_what_is_behind() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        for samples in [1, 4] {
            let r = Renderer::new(&gpu.device, samples);
            let target = Target::new(&gpu.device, (160, 100), samples);
            let mut cam = Camera::default();
            cam.fit(&scene, 1.6);
            let one = r.upload(&gpu.device, &gpu.queue, &scene, |j, _| {
                if j == 0 { Style::Normal } else { Style::Hidden }
            });
            let opts = DrawOptions::default();
            let h1 = r.hidden_share(&gpu.device, &gpu.queue, &target, &one, &cam, &opts);
            let all = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
            let ha = r.hidden_share(&gpu.device, &gpu.queue, &target, &all, &cam, &opts);
            let o = own(scene.clone());
            let allo = r.upload(&gpu.device, &gpu.queue, &o, |_, _| Style::Normal);
            let hao = r.hidden_share(&gpu.device, &gpu.queue, &target, &allo, &cam, &opts);
            assert_eq!(h1, 0.0, "one element alone");
            assert!(ha < 0.8 && hao < 0.8, "{ha} {hao}");
        }
    }

    /// A highlight drawn as an overlay over the kept picture (ADR 0036 #3) looks as when drawn
    /// with the model: where the element is hidden, the kept depth hides its edges too.
    #[test]
    fn highlights_drawn_over_the_kept_picture_match() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
        let r = Renderer::new(&gpu.device, 1);
        let gs = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
        let mut cam = Camera::default();
        cam.fit(&scene, 1.6);
        // The column nearest the camera (the frame's order is random: its GlobalIds are).
        let centre = |k: usize| {
            let b = ClipBox::around(&scene, |i| i == k).unwrap();
            (b.min + b.max) / 2.0
        };
        let col = (0..scene.elements.len())
            .filter(|&k| scene.elements[k].class == "IfcColumn")
            .min_by(|&a, &b| {
                let d = |k| centre(k).distance(cam.eye());
                d(a).total_cmp(&d(b))
            })
            .unwrap();
        let hi = r.upload(&gpu.device, &gpu.queue, &scene, |j, _| {
            if j == col {
                Style::Normal
            } else {
                Style::Hidden
            }
        });
        // One sample: every pixel is wholly in the overlay or not.
        let target = Target::new(&gpu.device, (200, 125), 1);
        let overlay = Overlay::new(&gpu.device, target.size, 1);
        let opts = DrawOptions::default();
        let red = Rgb([230, 20, 20]);
        let draw = |layers: &[Layer]| {
            r.render_layers(
                &gpu.device,
                &gpu.queue,
                &target,
                layers,
                &gs.bounds,
                &cam,
                &opts,
            );
        };
        draw(&[Layer::main(&gs), Layer::highlight(&hi, red)]);
        let together = gpu.read_rgba(&target).unwrap();
        draw(&[Layer::main(&gs)]);
        let base = gpu.read_rgba(&target).unwrap();
        r.render_overlay(
            &gpu.device,
            &gpu.queue,
            &target,
            &overlay,
            &[Layer::highlight(&hi, red)],
            &gs.bounds,
            &cam,
            &opts,
        );
        let over = gpu.read_texture(&overlay.color).unwrap();
        let px = |v: &[u8]| v.as_chunks::<4>().0.to_vec();
        let (together, base, over) = (px(&together), px(&base), px(&over));
        // Pixels the overlay covers wholly look the same; where it is empty, so do the pictures
        // (nothing hidden shows, nothing visible is missed). Partly covered pixels blend with
        // the resolved picture under them, not per sample: a slightly darker fringe.
        let near = |a: &[u8; 4], b: &[u8; 4]| (0..3).all(|k| a[k].abs_diff(b[k]) <= 2);
        let (mut whole, mut off) = (0, 0);
        for ((t, b), o) in together.iter().zip(&base).zip(&over) {
            match o[3] {
                255 => {
                    whole += 1;
                    off += usize::from(!near(t, &[o[0], o[1], o[2], 255]));
                }
                0 => off += usize::from(!near(t, b)),
                _ => {}
            }
        }
        assert!(whole > 20, "{whole} pixels wholly in the overlay");
        assert_eq!(off, 0, "{off} pixels differ");
    }

    /// Renders on whatever adapter exists (a software one in CI containers). Skipped if none.
    #[test]
    fn headless_render_draws_the_frame() {
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let scene = own(Scene::from_model(&concrete_frame(&FrameSpec::default())));
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
        let m0 = scene.elements[0].world_mesh();
        let ps = m0.positions.iter().map(|p| Vec3::from(*p));
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
