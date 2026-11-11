//! A render-ready scene: one flat-shaded mesh per element, in mm, relative to `origin_mm`.
//! Built from a native `.bimble` model or from an IFC file (see `bimble-ifc::read`).

use crate::Mesh;
use bimble_core::{Element, MaterialCategory, Model};
use rayon::prelude::*;
use std::borrow::Cow;
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Debug, Clone)]
pub struct SceneElement {
    pub global_id: String,
    /// IFC class name, e.g. "IfcBeam"
    pub class: String,
    pub name: String,
    pub storey: String,
    pub material: String,
    pub category: MaterialCategory,
    pub geometry: Geometry,
}

/// An element's triangles: its own, or a shape shared with other elements and placed
/// (ADR 0034: a mapped IFC shape is meshed once, whatever the number of copies).
#[derive(Debug, Clone)]
pub enum Geometry {
    /// Scene coordinates (mm, relative to the scene origin).
    Own(Mesh),
    /// The shape in its own coordinates (mm), and where it goes in the scene.
    Instance {
        shape: Arc<Mesh>,
        to_scene: Placement32,
    },
}

/// Where a shape goes in the scene: columns of the linear part (rotation, maybe mirror or
/// scale) and a translation, mm, f32 relative to the scene origin.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Placement32 {
    pub x: [f32; 3],
    pub y: [f32; 3],
    pub z: [f32; 3],
    pub t: [f32; 3],
}

impl Placement32 {
    pub fn apply(&self, p: [f32; 3]) -> [f32; 3] {
        [0, 1, 2].map(|k| self.x[k] * p[0] + self.y[k] * p[1] + self.z[k] * p[2] + self.t[k])
    }

    pub fn rotate(&self, v: [f32; 3]) -> [f32; 3] {
        [0, 1, 2].map(|k| self.x[k] * v[0] + self.y[k] * v[1] + self.z[k] * v[2])
    }

    /// A mirror turns triangles inside out: their winding must be flipped.
    pub fn mirrors(&self) -> bool {
        let c = [
            self.y[1] * self.z[2] - self.y[2] * self.z[1],
            self.y[2] * self.z[0] - self.y[0] * self.z[2],
            self.y[0] * self.z[1] - self.y[1] * self.z[0],
        ];
        self.x[0] * c[0] + self.x[1] * c[1] + self.x[2] * c[2] < 0.0
    }

    /// The shape's mesh in the scene.
    pub fn mesh(&self, shape: &Mesh) -> Mesh {
        let mut indices = shape.indices.clone();
        if self.mirrors() {
            for t in indices.as_chunks_mut::<3>().0 {
                t.swap(1, 2);
            }
        }
        let unpack = |n: [i8; 4]| [n[0], n[1], n[2]].map(|c| c as f32 / 127.0);
        let turn = |n: [i8; 4]| {
            if n == [0; 4] {
                return n;
            }
            let v = self.rotate(unpack(n));
            let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-12);
            crate::pack_normal(v.map(|c| c / l))
        };
        Mesh {
            positions: shape.positions.iter().map(|p| self.apply(*p)).collect(),
            indices,
            edges: shape
                .edges
                .iter()
                .map(|s| [self.apply(s[0]), self.apply(s[1])])
                .collect(),
            edge_normals: shape
                .edge_normals
                .iter()
                .map(|[a, b]| [turn(*a), turn(*b)])
                .collect(),
        }
    }
}

/// A placement while building, f64 world mm (`Placement32` once the scene origin is known).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Transform {
    pub x: [f64; 3],
    pub y: [f64; 3],
    pub z: [f64; 3],
    pub t: [f64; 3],
}

impl Transform {
    pub fn apply(&self, p: [f64; 3]) -> [f64; 3] {
        [0, 1, 2].map(|k| self.x[k] * p[0] + self.y[k] * p[1] + self.z[k] * p[2] + self.t[k])
    }

    fn relative_to(&self, origin: [f64; 3]) -> Placement32 {
        let f = |v: [f64; 3]| v.map(|c| c as f32);
        Placement32 {
            x: f(self.x),
            y: f(self.y),
            z: f(self.z),
            t: [0, 1, 2].map(|k| (self.t[k] - origin[k]) as f32),
        }
    }
}

impl SceneElement {
    /// Its triangles in scene coordinates: its own, or its shape placed (computed).
    pub fn world_mesh(&self) -> Cow<'_, Mesh> {
        match &self.geometry {
            Geometry::Own(m) => Cow::Borrowed(m),
            Geometry::Instance { shape, to_scene } => Cow::Owned(to_scene.mesh(shape)),
        }
    }

    pub fn triangle_count(&self) -> usize {
        self.local_mesh().triangle_count()
    }

    pub fn edge_count(&self) -> usize {
        self.local_mesh().edges.len()
    }

    /// Its own mesh, or its shape in the shape's coordinates.
    pub fn local_mesh(&self) -> &Mesh {
        match &self.geometry {
            Geometry::Own(m) => m,
            Geometry::Instance { shape, .. } => shape,
        }
    }

    /// Bounds in scene coordinates.
    pub fn bounds(&self) -> crate::bvh::Aabb {
        use crate::bvh::Aabb;
        match &self.geometry {
            Geometry::Own(m) => Aabb::of_points(&m.positions),
            Geometry::Instance { shape, to_scene } => {
                let b = Aabb::of_points(&shape.positions);
                if b.is_empty() {
                    return b;
                }
                let corners: Vec<[f32; 3]> = (0..8)
                    .map(|i| {
                        let c =
                            [0, 1, 2].map(|k| if i >> k & 1 == 0 { b.min[k] } else { b.max[k] });
                        to_scene.apply(c)
                    })
                    .collect();
                Aabb::of_points(&corners)
            }
        }
    }
}

/// One axis of a structural grid (IfcGridAxis): its tag ("A", "1") and its end points in plan,
/// world mm.
#[derive(Debug, Clone, PartialEq)]
pub struct GridAxis {
    pub tag: String,
    pub start_mm: [f64; 2],
    pub end_mm: [f64; 2],
}

/// A building storey: its name (as on elements) and its level, world mm.
#[derive(Debug, Clone, PartialEq)]
pub struct StoreyLevel {
    pub name: String,
    pub elevation_mm: f64,
    /// The storey's GlobalId when it has one (views refer to it), else empty.
    pub id: String,
}

impl Scene {
    /// The storeys as levels for views: keyed by GlobalId, or by name when there is none.
    pub fn levels(&self) -> Vec<bimble_core::view::Level> {
        self.storeys
            .iter()
            .map(|s| bimble_core::view::Level {
                key: if s.id.is_empty() {
                    s.name.clone()
                } else {
                    s.id.clone()
                },
                name: s.name.clone(),
                elevation_mm: s.elevation_mm,
            })
            .collect()
    }
}

#[derive(Debug, Clone, Default)]
pub struct Scene {
    /// World position (mm) of the mesh coordinate origin. Keeps f32 precise for site coordinates.
    pub origin_mm: [f64; 3],
    pub elements: Vec<SceneElement>,
    /// Storeys, lowest first.
    pub storeys: Vec<StoreyLevel>,
    /// Structural grid axes (plan, world mm), in file order.
    pub grids: Vec<GridAxis>,
    /// Things the loader could not represent exactly (unsupported geometry, etc.).
    pub warnings: Vec<String>,
    /// Density (kg/m³) of the materials that state one, by material name: a `.bimble` model's
    /// own values (ADR 0031). Empty for an IFC.
    pub densities: std::collections::BTreeMap<String, f64>,
    /// Where the model is and its project north (ADR 0045).
    pub georef: bimble_core::georef::Georef,
    /// The IFC schema of the file it was read from ("IFC2X3", "IFC4"…); empty otherwise.
    pub schema: String,
}

impl Scene {
    pub fn triangle_count(&self) -> usize {
        self.elements.iter().map(|e| e.triangle_count()).sum()
    }

    /// What the scene holds in memory, in triangles: own triangles, each shared shape once,
    /// and one per copy (its placement). See ADR 0034.
    pub fn stored_triangles(&self) -> usize {
        let mut shapes = std::collections::HashSet::new();
        self.elements
            .iter()
            .map(|e| match &e.geometry {
                Geometry::Own(m) => m.triangle_count(),
                Geometry::Instance { shape, .. } if shapes.insert(Arc::as_ptr(shape)) => {
                    1 + shape.triangle_count()
                }
                Geometry::Instance { .. } => 1,
            })
            .sum()
    }

    /// Elements that are a copy of a shared shape.
    pub fn instance_count(&self) -> usize {
        self.elements
            .iter()
            .filter(|e| matches!(e.geometry, Geometry::Instance { .. }))
            .count()
    }

    /// Bounding box in scene coordinates (relative to origin), or None if empty.
    pub fn bounds(&self) -> Option<([f32; 3], [f32; 3])> {
        let b = self
            .elements
            .iter()
            .fold(crate::bvh::Aabb::EMPTY, |b, e| b.union(&e.bounds()));
        (!b.is_empty()).then_some((b.min, b.max))
    }

    pub fn from_model(m: &Model) -> Scene {
        // Elements are meshed on all cores; results keep the model's (sorted) order.
        let elements: Vec<_> = m.elements.values().collect();
        // A mesh shared by several uncut elements (copies of one shape, ADR 0042) is meshed once
        // and placed, as the IFC reader does (ADR 0034).
        let shared_key = |e: &Element| match &e.generator {
            bimble_core::Generator::Mesh { positions, .. } if e.openings.is_empty() => {
                Some(Arc::as_ptr(positions) as usize)
            }
            _ => None,
        };
        let mut first: HashMap<usize, (&Element, usize)> = HashMap::new();
        for e in &elements {
            if let Some(k) = shared_key(e) {
                first.entry(k).or_insert((e, 0)).1 += 1;
            }
        }
        let shapes: HashMap<usize, Arc<Mesh>> = first
            .into_iter()
            .filter(|(_, (_, n))| *n > 1)
            .collect::<Vec<_>>()
            .into_par_iter()
            .filter_map(|(k, (e, _))| Some((k, Arc::new(crate::element_mesh(e).ok()?))))
            .collect();
        let meshed: Vec<Result<(ElementInfo, Built), String>> = elements
            .par_iter()
            .map(|e| {
                let storey = &m.storeys[&e.storey];
                let mat = &m.materials[&e.material];
                let info = ElementInfo {
                    global_id: e.id.to_string(),
                    class: e.class_name().to_owned(),
                    name: e.name.clone(),
                    storey: storey.name.clone(),
                    material: mat.name.clone(),
                    category: mat.category,
                };
                if let Some(shape) = shared_key(e).and_then(|k| shapes.get(&k)) {
                    let p = &e.placement;
                    let o = p.origin;
                    let to_world = Transform {
                        x: p.x_axis,
                        y: p.y_axis(),
                        z: p.z_axis,
                        t: [o[0], o[1], o[2] + storey.elevation],
                    };
                    return Ok((info, Built::Instance(shape.clone(), to_world)));
                }
                let local = crate::element_mesh(e).map_err(|err| {
                    format!("{} {}: could not mesh ({err:?})", e.class_name(), e.name)
                })?;
                let t = crate::placement_matrix(&e.placement, storey.elevation);
                let mut mb = MeshBuilder::default();
                for tri in local.indices.as_chunks::<3>().0 {
                    let p = tri.map(|i| transform(&t, local.positions[i as usize].map(f64::from)));
                    mb.add_triangle(p);
                }
                Ok((info, Built::Own(mb)))
            })
            .collect();
        let mut b = SceneBuilder {
            storeys: m
                .storeys
                .values()
                .map(|s| StoreyLevel {
                    name: s.name.clone(),
                    elevation_mm: s.elevation,
                    id: s.id.to_string(),
                })
                .collect(),
            ..Default::default()
        };
        for r in meshed {
            match r {
                Ok((info, Built::Own(mb))) => b.push(info, mb),
                Ok((info, Built::Instance(shape, t))) => b.push_instance(info, shape, t),
                Err(w) => b.warnings.push(w),
            }
        }
        let mut scene = b.finish();
        scene.densities = m
            .materials
            .values()
            .filter_map(|x| Some((x.name.clone(), x.props.density_kg_m3?)))
            .collect();
        scene.georef = m.georef.clone();
        scene
    }
}

/// An element of a model, meshed: its own triangles, or a shared shape placed.
enum Built {
    Own(MeshBuilder),
    Instance(Arc<Mesh>, Transform),
}

fn transform(t: &[[f64; 4]; 4], p: [f64; 3]) -> [f64; 3] {
    [0, 1, 2].map(|r| t[0][r] * p[0] + t[1][r] * p[1] + t[2][r] * p[2] + t[3][r])
}

/// Metadata for an element added to a `SceneBuilder`.
#[derive(Debug, Clone)]
pub struct ElementInfo {
    pub global_id: String,
    pub class: String,
    pub name: String,
    pub storey: String,
    pub material: String,
    pub category: MaterialCategory,
}

/// Collects world-space (f64, mm) triangles per element, then rebases them to a common origin.
#[derive(Default)]
pub struct SceneBuilder {
    items: Vec<(ElementInfo, Body)>,
    pub warnings: Vec<String>,
    /// Storeys (any order; sorted by elevation in `finish`).
    pub storeys: Vec<StoreyLevel>,
    pub grids: Vec<GridAxis>,
}

impl SceneBuilder {
    pub fn with_storeys(storeys: Vec<StoreyLevel>) -> SceneBuilder {
        SceneBuilder {
            storeys,
            ..Default::default()
        }
    }

    pub fn push(&mut self, info: ElementInfo, mesh: MeshBuilder) {
        if !mesh.tris.is_empty() {
            self.items.push((info, Body::Tris(mesh)));
        }
    }

    /// An element that is a shared shape (mm, its own coordinates, already built) placed in the
    /// world (ADR 0034).
    pub fn push_instance(&mut self, info: ElementInfo, shape: Arc<Mesh>, to_world: Transform) {
        if !shape.indices.is_empty() {
            self.items.push((info, Body::Instance(shape, to_world)));
        }
    }

    pub fn finish(self) -> Scene {
        let (mut lo, mut hi) = ([f64::INFINITY; 3], [f64::NEG_INFINITY; 3]);
        let mut add = |p: [f64; 3]| {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k]);
                hi[k] = hi[k].max(p[k]);
            }
        };
        for (_, body) in &self.items {
            match body {
                Body::Tris(mb) => mb.tris.iter().flatten().for_each(|p| add(*p)),
                Body::Instance(shape, tf) => {
                    let b = crate::bvh::Aabb::of_points(&shape.positions);
                    for i in 0..8 {
                        let c = [0, 1, 2]
                            .map(|k| f64::from(if i >> k & 1 == 0 { b.min[k] } else { b.max[k] }));
                        add(tf.apply(c));
                    }
                }
            }
        }
        let origin = if lo[0].is_finite() {
            [0, 1, 2].map(|k| ((lo[k] + hi[k]) / 2.0).round())
        } else {
            [0.0; 3]
        };
        let mut scene = Scene {
            densities: Default::default(),
            georef: Default::default(),
            schema: String::new(),
            origin_mm: origin,
            elements: self
                .items
                .into_par_iter()
                .map(|(info, body)| SceneElement {
                    global_id: info.global_id,
                    class: info.class,
                    name: info.name,
                    storey: info.storey,
                    material: info.material,
                    category: info.category,
                    geometry: match body {
                        Body::Tris(mb) => Geometry::Own(mb.build(origin)),
                        Body::Instance(shape, tf) => Geometry::Instance {
                            shape,
                            to_scene: tf.relative_to(origin),
                        },
                    },
                })
                // Degenerate triangles are dropped in build(): an element can end up empty.
                .filter(|e| e.triangle_count() > 0)
                .collect(),
            warnings: self.warnings,
            grids: self.grids,
            storeys: {
                let mut v = self.storeys;
                v.sort_by(|a, b| a.elevation_mm.total_cmp(&b.elevation_mm));
                v
            },
        };
        // Copies the file didn't mark as copies become shared shapes (ADR 0036 step 3).
        crate::dedup::share_copies(&mut scene.elements);
        scene
    }
}

/// What an element is made of while building: world triangles, or a placed shape.
enum Body {
    Tris(MeshBuilder),
    Instance(Arc<Mesh>, Transform),
}

/// World-space triangle soup for one element. Each triangle gets its own flat normal.
#[derive(Default, Clone)]
pub struct MeshBuilder {
    pub tris: Vec<[[f64; 3]; 3]>,
}

impl MeshBuilder {
    pub fn add_triangle(&mut self, t: [[f64; 3]; 3]) {
        if t.iter().flatten().all(|v| v.is_finite()) {
            self.tris.push(t);
        }
    }

    /// Add a planar polygon (outer loop + holes), triangulated in its own plane.
    /// The winding of the output follows the outer loop (Newell normal).
    pub fn add_polygon(&mut self, outer: &[[f64; 3]], holes: &[Vec<[f64; 3]>]) {
        if outer.len() < 3 {
            return;
        }
        let n = newell(outer);
        let len = norm(n);
        if len.is_nan() || len <= 1e-12 {
            return;
        }
        let n = n.map(|c| c / len);
        // Drop the dominant axis to project to 2D.
        let ax = (0..3)
            .max_by(|&a, &b| n[a].abs().total_cmp(&n[b].abs()))
            .unwrap();
        let (u, v) = match ax {
            0 => (1, 2),
            1 => (2, 0),
            _ => (0, 1),
        };
        let mut pts: Vec<[f64; 3]> = outer.to_vec();
        let mut flat: Vec<f64> = outer.iter().flat_map(|p| [p[u], p[v]]).collect();
        let mut hole_idx = Vec::new();
        for h in holes.iter().filter(|h| h.len() >= 3) {
            hole_idx.push(pts.len());
            pts.extend_from_slice(h);
            flat.extend(h.iter().flat_map(|p| [p[u], p[v]]));
        }
        let Ok(idx) = earcutr::earcut(&flat, &hole_idx, 2) else {
            return;
        };
        for t in idx.as_chunks::<3>().0 {
            let mut tri = [pts[t[0]], pts[t[1]], pts[t[2]]];
            if dot(cross(sub(tri[1], tri[0]), sub(tri[2], tri[0])), n) < 0.0 {
                tri.swap(1, 2);
            }
            self.add_triangle(tri);
        }
    }

    /// Build the mesh relative to `origin`, positions welded, with crease edges for outlines.
    pub fn build(&self, origin: [f64; 3]) -> Mesh {
        let mut m = Mesh::default();
        let mut index: HashMap<[u32; 3], u32> = HashMap::new();
        let rel = |p: [f64; 3]| {
            [
                (p[0] - origin[0]) as f32,
                (p[1] - origin[1]) as f32,
                (p[2] - origin[2]) as f32,
            ]
        };
        for t in &self.tris {
            let l = norm(cross(sub(t[1], t[0]), sub(t[2], t[0])));
            if l.is_nan() || l <= 0.0 {
                continue;
            }
            for p in t {
                let p = rel(*p);
                let k = *index.entry(p.map(f32::to_bits)).or_insert_with(|| {
                    m.positions.push(p);
                    m.positions.len() as u32 - 1
                });
                m.indices.push(k);
            }
        }
        let (edges, normals) = crease_edges_with_normals(&m, 20.0);
        m.edges = edges;
        m.edge_normals = normals;
        m
    }
}

struct EdgeUse {
    count: usize,
    /// Normal of the first face using this edge.
    normal: [f32; 3],
    /// Normal of the last other face using it.
    other: [f32; 3],
    crease: bool,
    seg: [[f32; 3]; 2],
}

/// Edges to outline: boundary edges, and edges where adjacent faces meet at more than `angle_deg`.
/// Coplanar triangulation diagonals and smooth curved surfaces (arc segments) are not outlined.
pub fn crease_edges(m: &Mesh, angle_deg: f32) -> Vec<[[f32; 3]; 2]> {
    crease_edges_with_normals(m, angle_deg).0
}

/// Crease and boundary edges, with the normals of the faces on both sides (see `Mesh`).
pub fn crease_edges_with_normals(
    m: &Mesh,
    angle_deg: f32,
) -> (Vec<crate::Segment>, Vec<crate::EdgeNormals>) {
    let cos_limit = angle_deg.to_radians().cos();
    // Quantize to 0.01 mm to weld duplicated vertices.
    let key = |p: [f32; 3]| p.map(|c| (c * 100.0).round() as i64);
    let mut map: HashMap<([i64; 3], [i64; 3]), EdgeUse> = HashMap::new();
    for t in m.indices.as_chunks::<3>().0 {
        // The face's own normal, from its geometry: welded meshes don't keep a normal per
        // vertex, and flat-shaded faces are exactly what creases are computed from.
        let (a0, b0, c0) = (
            m.positions[t[0] as usize],
            m.positions[t[1] as usize],
            m.positions[t[2] as usize],
        );
        let u = [b0[0] - a0[0], b0[1] - a0[1], b0[2] - a0[2]];
        let v = [c0[0] - a0[0], c0[1] - a0[1], c0[2] - a0[2]];
        let cr = [
            u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0],
        ];
        let l = (cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]).sqrt();
        let n: [f32; 3] = if l > 1e-12 {
            cr.map(|c| c / l)
        } else {
            [0.0, 0.0, 1.0]
        };
        for k in 0..3 {
            let (a, b) = (
                m.positions[t[k] as usize],
                m.positions[t[(k + 1) % 3] as usize],
            );
            let (ka, kb) = (key(a), key(b));
            if ka == kb {
                continue;
            }
            let k2 = if ka < kb { (ka, kb) } else { (kb, ka) };
            let e = map.entry(k2).or_insert(EdgeUse {
                count: 0,
                normal: n,
                other: n,
                crease: false,
                seg: [a, b],
            });
            if e.count > 0 {
                let c = e.normal[0] * n[0] + e.normal[1] * n[1] + e.normal[2] * n[2];
                if c < cos_limit {
                    e.crease = true;
                }
                e.other = n;
            }
            e.count += 1;
        }
    }
    let mut out: Vec<_> = map
        .into_values()
        .filter(|e| e.count == 1 || e.crease)
        .map(|e| {
            let n1 = crate::pack_normal(e.normal);
            let n2 = match e.count {
                // Boundary: opposite normals, so the edge is never "behind its own solid".
                1 => crate::pack_normal(e.normal.map(|c| -c)),
                // Two faces: the second one. More than two (non-manifold): unknown, always draw.
                2 => crate::pack_normal(e.other),
                _ => return (e.seg, [[0; 4]; 2]),
            };
            (e.seg, [n1, n2])
        })
        .collect();
    // Deterministic order (HashMap iteration is random).
    out.sort_by(|a, b| {
        a.0.as_flattened()
            .partial_cmp(b.0.as_flattened())
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    out.into_iter().unzip()
}

pub fn newell(poly: &[[f64; 3]]) -> [f64; 3] {
    let mut n = [0.0; 3];
    for i in 0..poly.len() {
        let (a, b) = (poly[i], poly[(i + 1) % poly.len()]);
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    n
}

pub fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

pub fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

pub fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

pub fn norm(a: [f64; 3]) -> f64 {
    dot(a, a).sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    /// Review 2026-11-05: an element whose triangles are all degenerate must not survive as an
    /// empty mesh (its bounding box would be ±f64::MAX).
    #[test]
    fn degenerate_element_is_dropped() {
        let info = |name: &str| ElementInfo {
            global_id: name.into(),
            class: "IfcWall".into(),
            name: name.into(),
            storey: String::new(),
            material: String::new(),
            category: MaterialCategory::Other,
        };
        let mut flat = MeshBuilder::default();
        flat.add_triangle([[0.0; 3], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0]]); // collinear
        let mut ok = MeshBuilder::default();
        ok.add_triangle([[0.0; 3], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]);
        let mut b = SceneBuilder::default();
        b.push(info("flat"), flat);
        b.push(info("ok"), ok);
        let s = b.finish();
        assert_eq!(s.elements.len(), 1);
        assert_eq!(s.elements[0].name, "ok");
    }

    #[test]
    fn polygon_with_hole_area() {
        let mut mb = MeshBuilder::default();
        let sq = |s: f64| vec![[0.0, 0.0, 0.0], [s, 0.0, 0.0], [s, s, 0.0], [0.0, s, 0.0]];
        let hole: Vec<[f64; 3]> = vec![
            [1.0, 1.0, 0.0],
            [1.0, 2.0, 0.0],
            [2.0, 2.0, 0.0],
            [2.0, 1.0, 0.0],
        ];
        mb.add_polygon(&sq(4.0), &[hole]);
        let area: f64 = mb
            .tris
            .iter()
            .map(|t| norm(cross(sub(t[1], t[0]), sub(t[2], t[0]))) / 2.0)
            .sum();
        assert!((area - 15.0).abs() < 1e-9, "{area}");
        assert!(
            mb.tris
                .iter()
                .all(|t| cross(sub(t[1], t[0]), sub(t[2], t[0]))[2] > 0.0)
        );
    }

    #[test]
    fn box_has_12_crease_edges() {
        let g = bimble_core::Generator::RectColumn {
            width: 300.0,
            depth: 300.0,
            height: 3000.0,
        };
        let x = g.extrusion().unwrap();
        let m = crate::extrude(&x.profile, &x.voids, x.depth).unwrap();
        assert_eq!(crease_edges(&m, 20.0).len(), 12);
    }

    #[test]
    fn scene_from_model_keeps_volume_and_rebases() {
        let model = concrete_frame(&FrameSpec::default());
        let s = Scene::from_model(&model);
        assert_eq!(s.elements.len(), model.elements.len());
        let v: f64 = s.elements.iter().map(|e| e.world_mesh().volume()).sum();
        let want: f64 = model
            .elements
            .values()
            .map(|e| crate::element_mesh(e).unwrap().volume())
            .sum();
        assert!((v - want).abs() / want < 1e-5, "{v} vs {want}");
        let (lo, hi) = s.bounds().unwrap();
        for k in 0..3 {
            assert!(
                (lo[k] + hi[k]).abs() < 2.0,
                "not centred on axis {k}: {lo:?} {hi:?}"
            );
        }
    }

    /// Copies sharing one mesh (ADR 0042) are placed instances of one shape, each where its
    /// placement puts it, turned ones included.
    #[test]
    fn shared_meshes_become_placed_instances() {
        let mut model = concrete_frame(&FrameSpec::default());
        let template = model.elements.values().next().unwrap().clone();
        let mesh = bimble_core::Generator::Mesh {
            positions: vec![[0.0, 0.0, 0.0], [1000.0, 0.0, 0.0], [0.0, 500.0, 0.0]].into(),
            indices: vec![0, 1, 2].into(),
        };
        let mut want = Vec::new();
        for (k, (x_axis, origin)) in [([1.0, 0.0, 0.0], 0.0), ([0.0, 1.0, 0.0], 10_000.0)]
            .into_iter()
            .enumerate()
        {
            let mut e = template.clone();
            e.id = bimble_core::GlobalId::derived(format!("copy/{k}").as_bytes());
            e.generator = mesh.clone();
            e.openings.clear();
            e.placement = bimble_core::Placement {
                origin: [origin, 0.0, 0.0],
                z_axis: [0.0, 0.0, 1.0],
                x_axis,
            };
            want.push((e.id.to_string(), e.placement.clone()));
            model.elements.insert(e.id, e);
        }
        let s = Scene::from_model(&model);
        let z = model.storeys[&template.storey].elevation;
        for (id, p) in want {
            let e = s.elements.iter().find(|e| e.global_id == id).unwrap();
            assert!(matches!(e.geometry, Geometry::Instance { .. }));
            // The far corner (1000, 0) of the shape, in the world, against the scene's.
            let w = [
                p.origin[0] + 1000.0 * p.x_axis[0],
                p.origin[1] + 1000.0 * p.x_axis[1],
                z,
            ];
            let world = e.world_mesh();
            let got = world.positions.iter().map(|q| {
                (0..3)
                    .map(|k| (f64::from(q[k]) + s.origin_mm[k] - w[k]).abs())
                    .fold(0.0, f64::max)
            });
            assert!(got.fold(f64::MAX, f64::min) < 0.01, "{id}");
        }
    }
}
