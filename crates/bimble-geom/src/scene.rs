//! A render-ready scene: one flat-shaded mesh per element, in mm, relative to `origin_mm`.
//! Built from a native `.bimble` model or from an IFC file (see `bimble-ifc::read`).

use crate::Mesh;
use bimble_core::{MaterialCategory, Model};
use rayon::prelude::*;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct SceneElement {
    pub global_id: String,
    /// IFC class name, e.g. "IfcBeam"
    pub class: String,
    pub name: String,
    pub storey: String,
    pub material: String,
    pub category: MaterialCategory,
    pub mesh: Mesh,
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
}

impl Scene {
    pub fn triangle_count(&self) -> usize {
        self.elements.iter().map(|e| e.mesh.triangle_count()).sum()
    }

    /// Bounding box in scene coordinates (relative to origin), or None if empty.
    pub fn bounds(&self) -> Option<([f32; 3], [f32; 3])> {
        let mut it = self.elements.iter().flat_map(|e| e.mesh.positions.iter());
        let first = *it.next()?;
        Some(it.fold((first, first), |(mut lo, mut hi), p| {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k]);
                hi[k] = hi[k].max(p[k]);
            }
            (lo, hi)
        }))
    }

    pub fn from_model(m: &Model) -> Scene {
        // Elements are meshed on all cores; results keep the model's (sorted) order.
        let elements: Vec<_> = m.elements.values().collect();
        let meshed: Vec<Result<(ElementInfo, MeshBuilder), String>> = elements
            .par_iter()
            .map(|e| {
                let Ok(local) = crate::element_mesh(e) else {
                    return Err(format!("{} {}: could not mesh", e.class.name(), e.name));
                };
                let storey = &m.storeys[&e.storey];
                let mat = &m.materials[&e.material];
                let t = crate::placement_matrix(&e.placement, storey.elevation);
                let mut mb = MeshBuilder::default();
                for tri in local.indices.as_chunks::<3>().0 {
                    let p = tri.map(|i| transform(&t, local.positions[i as usize].map(f64::from)));
                    mb.add_triangle(p);
                }
                let info = ElementInfo {
                    global_id: e.id.to_string(),
                    class: e.class.name().to_owned(),
                    name: e.name.clone(),
                    storey: storey.name.clone(),
                    material: mat.name.clone(),
                    category: mat.category,
                };
                Ok((info, mb))
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
                Ok((info, mb)) => b.push(info, mb),
                Err(w) => b.warnings.push(w),
            }
        }
        let mut scene = b.finish();
        scene.densities = m
            .materials
            .values()
            .filter_map(|x| Some((x.name.clone(), x.props.density_kg_m3?)))
            .collect();
        scene
    }
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
    items: Vec<(ElementInfo, MeshBuilder)>,
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
            self.items.push((info, mesh));
        }
    }

    pub fn finish(self) -> Scene {
        let (mut lo, mut hi) = ([f64::INFINITY; 3], [f64::NEG_INFINITY; 3]);
        for (_, mb) in &self.items {
            for p in mb.tris.iter().flatten() {
                for k in 0..3 {
                    lo[k] = lo[k].min(p[k]);
                    hi[k] = hi[k].max(p[k]);
                }
            }
        }
        let origin = if lo[0].is_finite() {
            [0, 1, 2].map(|k| ((lo[k] + hi[k]) / 2.0).round())
        } else {
            [0.0; 3]
        };
        Scene {
            densities: Default::default(),
            origin_mm: origin,
            elements: self
                .items
                .into_par_iter()
                .map(|(info, mb)| SceneElement {
                    global_id: info.global_id,
                    class: info.class,
                    name: info.name,
                    storey: info.storey,
                    material: info.material,
                    category: info.category,
                    mesh: mb.build(origin),
                })
                // Degenerate triangles are dropped in build(): an element can end up empty.
                .filter(|e| !e.mesh.indices.is_empty())
                .collect(),
            warnings: self.warnings,
            grids: self.grids,
            storeys: {
                let mut v = self.storeys;
                v.sort_by(|a, b| a.elevation_mm.total_cmp(&b.elevation_mm));
                v
            },
        }
    }
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

    /// Build the GPU mesh relative to `origin`, with crease edges for outlines.
    pub fn build(&self, origin: [f64; 3]) -> Mesh {
        let mut m = Mesh::default();
        let rel = |p: [f64; 3]| {
            [
                (p[0] - origin[0]) as f32,
                (p[1] - origin[1]) as f32,
                (p[2] - origin[2]) as f32,
            ]
        };
        for t in &self.tris {
            let n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
            let l = norm(n);
            if l.is_nan() || l <= 0.0 {
                continue;
            }
            let n = n.map(|c| (c / l) as f32);
            let base = m.positions.len() as u32;
            for p in t {
                m.positions.push(rel(*p));
                m.normals.push(n);
            }
            m.indices.extend([base, base + 1, base + 2]);
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
        let n = m.normals[t[0] as usize];
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
        let v: f64 = s.elements.iter().map(|e| e.mesh.volume()).sum();
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
}
