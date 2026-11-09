//! Mesh generation. Meshes are in element-local coordinates (f32, mm) so precision stays good
//! far from the origin; the renderer applies the placement as an instance transform.
//!
//! Faces get their own vertices (flat shading, crisp drawing look). Feature edges are listed
//! separately so the renderer can draw outlines like a technical drawing.

pub mod boolean;
pub mod bvh;
pub mod codec;
pub mod compare;
pub mod dedup;
pub mod hlod;
pub mod scene;

pub use scene::{
    ElementInfo, GridAxis, MeshBuilder, Scene, SceneBuilder, SceneElement, StoreyLevel,
};

use bimble_core::Generator;
use std::collections::HashMap;

use bimble_core::{Element, Placement, Profile};

#[derive(Debug, Default, Clone)]
pub struct Mesh {
    /// Welded per element: faces are flat, so a face's normal comes from its triangle, not
    /// from its vertices (renderers compute it).
    pub positions: Vec<[f32; 3]>,
    pub indices: Vec<u32>,
    /// Feature edges as pairs of positions (profile outline at both ends + vertical edges).
    pub edges: Vec<Segment>,
    /// For each edge, the normals of its two faces (snorm8, w unused). An edge whose faces both
    /// face away from the viewer is hidden behind its own solid, and only seen through a section
    /// cut, where it must not be drawn. Boundary edges store opposite normals (never both away);
    /// zero normals mean "unknown, always draw". May be shorter than `edges` (missing = unknown).
    pub edge_normals: Vec<EdgeNormals>,
}

/// An edge: two positions.
pub type Segment = [[f32; 3]; 2];
/// The packed normals of the two faces of an edge (see `Mesh::edge_normals`).
pub type EdgeNormals = [[i8; 4]; 2];

/// A unit normal packed as snorm8.
pub fn pack_normal(n: [f32; 3]) -> [i8; 4] {
    let q = |c: f32| (c.clamp(-1.0, 1.0) * 127.0).round() as i8;
    [q(n[0]), q(n[1]), q(n[2]), 0]
}

impl Mesh {
    pub fn triangle_count(&self) -> usize {
        self.indices.len() / 3
    }

    /// Signed volume (divergence theorem). Positive when the triangles face outward.
    ///
    /// Computed relative to the mesh's first vertex. For a closed mesh the reference point doesn't
    /// matter; for an open or flat surface it does (the result grows with area × distance). Using
    /// a vertex of the element makes a flat sheet read ~0 instead of depending on where the scene
    /// origin happens to be (a 2400 mm² plate 1.3 m from the origin read 1e6 mm³).
    pub fn volume(&self) -> f64 {
        let Some(o) = self.positions.first().map(|p| p.map(f64::from)) else {
            return 0.0;
        };
        self.indices
            .as_chunks::<3>()
            .0
            .iter()
            .map(|t| {
                let [a, b, c] = [0, 1, 2].map(|k| {
                    let p = self.positions[t[k] as usize];
                    [p[0] as f64 - o[0], p[1] as f64 - o[1], p[2] as f64 - o[2]]
                });
                a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0])
            })
            .sum::<f64>()
            / 6.0
    }
}

#[derive(Debug, PartialEq)]
pub enum GeomError {
    Triangulation(String),
}

fn profile_points(p: &Profile) -> Vec<[f64; 2]> {
    match p {
        Profile::Rectangle { x_dim, y_dim } => {
            let (x, y) = (x_dim / 2.0, y_dim / 2.0);
            vec![[-x, -y], [x, -y], [x, y], [-x, y]]
        }
        Profile::Polygon(pts) => pts.clone(),
    }
}

/// Triangulate a counter-clockwise loop with clockwise holes (earcut wants the start index of
/// each hole, holes stored contiguously after the outer loop).
fn earcut_indices(outer: &[[f64; 2]], holes: &[Vec<[f64; 2]>]) -> Result<Vec<u32>, GeomError> {
    let mut flat: Vec<f64> = outer.iter().flatten().copied().collect();
    let mut hole_indices: Vec<usize> = Vec::new();
    for h in holes {
        hole_indices.push(flat.len() / 2);
        flat.extend(h.iter().flatten().copied());
    }
    let tris = earcutr::earcut(&flat, &hole_indices, 2)
        .map_err(|e| GeomError::Triangulation(format!("{e:?}")))?;
    Ok(tris.iter().map(|i| *i as u32).collect())
}

/// Extrude a counter-clockwise profile along local +Z by `depth`, cutting out clockwise voids,
/// as f64 triangles with outward winding (closed solid, ready for booleans).
pub fn extrude_tris(
    profile: &Profile,
    voids: &[Vec<[f64; 2]>],
    depth: f64,
) -> Result<Vec<crate::boolean::Tri>, GeomError> {
    let pts = profile_points(profile);
    let n = pts.len();
    let tris = earcut_indices(&pts, voids)?;
    let total = n + voids.iter().map(Vec::len).sum::<usize>();
    // Euler: a polygon with h holes triangulates to nv - 2 + 2h triangles.
    let expected = total - 2 + 2 * voids.len();
    if tris.len() / 3 != expected {
        return Err(GeomError::Triangulation(format!(
            "expected {expected} triangles, got {}",
            tris.len() / 3
        )));
    }
    let all: Vec<&[[f64; 2]]> = std::iter::once(&pts[..])
        .chain(voids.iter().map(|v| v.as_slice()))
        .collect();
    // 3D points of the caps, per loop.
    let at = |loop_idx: usize, i: usize, z: f64| {
        let q = all[loop_idx][i];
        [q[0], q[1], z]
    };
    let mut out: Vec<crate::boolean::Tri> = Vec::with_capacity(tris.len() + all.len() * 6);

    // Caps: bottom (normal -Z) reversed, top (normal +Z) as-is.
    for (z, rev) in [(0.0, true), (depth, false)] {
        for t in tris.as_chunks::<3>().0 {
            let mut t: [usize; 3] = [t[0] as usize, t[1] as usize, t[2] as usize];
            if rev {
                t.swap(1, 2);
            }
            // Map a welded vertex index back to (loop, point-in-loop).
            let pt = |k: usize| {
                let mut k = k;
                for (li, l) in all.iter().enumerate() {
                    if k < l.len() {
                        return at(li, k, z);
                    }
                    k -= l.len();
                }
                unreachable!("earcut index out of range");
            };
            out.push(t.map(pt));
        }
    }

    // Sides: one quad per profile edge (void edges included), outward.
    for (li, l) in all.iter().enumerate() {
        for i in 0..l.len() {
            let j = (i + 1) % l.len();
            let (a, b) = (at(li, i, 0.0), at(li, j, 0.0));
            let (a2, b2) = (at(li, i, depth), at(li, j, depth));
            out.push([a, b, b2]);
            out.push([a, b2, a2]);
        }
    }
    Ok(out)
}

/// Extrude a profile into a render-ready mesh (flat normals, profile edges listed).
pub fn extrude(profile: &Profile, voids: &[Vec<[f64; 2]>], depth: f64) -> Result<Mesh, GeomError> {
    let tris = extrude_tris(profile, voids, depth)?;
    Ok(mesh_from_tris(&tris))
}

/// A triangle mesh as stored in a `Mesh` generator, with flat per-face normals and edges.
pub fn mesh_from(positions: &[[f64; 3]], indices: &[u32]) -> Result<Mesh, GeomError> {
    if !indices.len().is_multiple_of(3) {
        return Err(GeomError::Triangulation(
            "index count not a multiple of 3".into(),
        ));
    }
    let tris: Vec<crate::boolean::Tri> = indices
        .as_chunks::<3>()
        .0
        .iter()
        .map(|t| t.map(|i| positions[i as usize]))
        .collect();
    Ok(mesh_from_tris(&tris))
}

/// Weld a triangle soup (same bits) into a mesh with flat per-face normals.
pub fn mesh_from_tris(tris: &[crate::boolean::Tri]) -> Mesh {
    let mut positions: Vec<[f64; 3]> = Vec::new();
    let mut index: HashMap<[u64; 3], u32> = HashMap::new();
    let mut indices: Vec<u32> = Vec::with_capacity(tris.len() * 3);
    for t in tris {
        for p in t {
            let p = p.map(|c| c + 0.0); // -0.0 → 0.0 so equal coordinates weld
            let next = positions.len() as u32;
            let i = *index.entry(p.map(f64::to_bits)).or_insert(next);
            if i == next {
                positions.push(p);
            }
            indices.push(i);
        }
    }
    let mut m = Mesh {
        positions: positions.iter().map(|p| p.map(|c| c as f32)).collect(),
        indices,
        ..Default::default()
    };
    // Feature edges (profile outline, corners), like the scene build draws them.
    m.edges = crate::scene::crease_edges(&m, 20.0);
    m
}

/// The element's triangles in element-local coordinates (mm, f64), openings subtracted.
/// An opening that can't be cut (not closed solids) is skipped: drawn uncut, like the viewer.
pub fn element_tris(e: &Element) -> Result<Vec<crate::boolean::Tri>, GeomError> {
    let mut tris: Vec<crate::boolean::Tri> = match &e.generator {
        Generator::Mesh { positions, indices } => {
            if !indices.len().is_multiple_of(3) {
                return Err(GeomError::Triangulation(
                    "index count not a multiple of 3".into(),
                ));
            }
            indices
                .as_chunks::<3>()
                .0
                .iter()
                .map(|t| t.map(|i| positions[i as usize]))
                .collect()
        }
        g => {
            let x = g
                .extrusion()
                .ok_or_else(|| GeomError::Triangulation("no extrusion".into()))?;
            extrude_tris(&x.profile, &x.voids, x.depth)?
        }
    };
    if e.openings.is_empty() {
        return Ok(tris);
    }
    let cutters: Vec<Vec<crate::boolean::Tri>> = e
        .openings
        .iter()
        .filter_map(|o| opening_tris(o).ok())
        .collect();
    if cutters.is_empty() {
        return Ok(tris);
    }
    let refs: Vec<&[crate::boolean::Tri]> = cutters.iter().map(Vec::as_slice).collect();
    // A boolean that fails (not closed solids) leaves the element drawn uncut.
    if let Ok(r) = crate::boolean::apply(crate::boolean::Op::Difference, &tris, &refs) {
        tris = r;
    }
    Ok(tris)
}

/// An opening's solid, transformed from its own placement into element coordinates.
fn opening_tris(o: &bimble_core::Opening) -> Result<Vec<crate::boolean::Tri>, GeomError> {
    let mut tris = match &o.generator {
        Generator::Mesh { positions, indices } => {
            if !indices.len().is_multiple_of(3) {
                return Err(GeomError::Triangulation(
                    "index count not a multiple of 3".into(),
                ));
            }
            indices
                .as_chunks::<3>()
                .0
                .iter()
                .map(|t| t.map(|i| positions[i as usize]))
                .collect::<Vec<_>>()
        }
        g => {
            let x = g
                .extrusion()
                .ok_or_else(|| GeomError::Triangulation("no extrusion".into()))?;
            extrude_tris(&x.profile, &x.voids, x.depth)?
        }
    };
    let t = placement_matrix(&o.placement, 0.0);
    for tri in &mut tris {
        for p in tri {
            *p = transform(&t, *p);
        }
    }
    Ok(tris)
}

fn transform(t: &[[f64; 4]; 4], p: [f64; 3]) -> [f64; 3] {
    [0, 1, 2].map(|r| t[0][r] * p[0] + t[1][r] * p[1] + t[2][r] * p[2] + t[3][r])
}

pub fn element_mesh(e: &Element) -> Result<Mesh, GeomError> {
    Ok(mesh_from_tris(&element_tris(e)?))
}

/// Column-major 4×4 transform from an element placement plus its storey elevation (mm, f64).
pub fn placement_matrix(p: &Placement, storey_elevation: f64) -> [[f64; 4]; 4] {
    let y = p.y_axis();
    let o = p.origin;
    [
        [p.x_axis[0], p.x_axis[1], p.x_axis[2], 0.0],
        [y[0], y[1], y[2], 0.0],
        [p.z_axis[0], p.z_axis[1], p.z_axis[2], 0.0],
        [o[0], o[1], o[2] + storey_elevation, 1.0],
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::Generator;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn approx(a: f64, b: f64) {
        assert!((a - b).abs() <= 1e-6 * b.abs().max(1.0), "{a} != {b}");
    }

    /// An open, flat sheet away from the scene origin reads ~0, not area × distance / 3.
    #[test]
    fn flat_open_mesh_away_from_origin_has_zero_volume() {
        let (x, y, z) = (600.0f32, -450.0f32, 1200.0f32);
        let q = [
            [x, y, z],
            [x + 1000.0, y, z],
            [x + 1000.0, y + 1000.0, z],
            [x, y + 1000.0, z],
        ];
        let m = Mesh {
            positions: q.to_vec(),
            // One side only: an open surface. Relative to the origin this would be 1e6×1200/3.
            indices: vec![0, 1, 2, 0, 2, 3],
            ..Default::default()
        };
        assert!(m.volume().abs() < 1.0, "{}", m.volume());
        // A closed box far away keeps its exact volume.
        let g = bimble_core::Generator::RectColumn {
            width: 300.0,
            depth: 400.0,
            height: 3000.0,
        };
        let xg = g.extrusion().unwrap();
        let mut b = extrude(&xg.profile, &xg.voids, xg.depth).unwrap();
        for p in &mut b.positions {
            p[0] += x;
            p[1] += y;
            p[2] += z;
        }
        approx(b.volume(), 300.0 * 400.0 * 3000.0);
    }

    #[test]
    fn column_box() {
        let g = Generator::RectColumn {
            width: 300.0,
            depth: 400.0,
            height: 3000.0,
        };
        let x = g.extrusion().unwrap();
        let m = extrude(&x.profile, &x.voids, x.depth).unwrap();
        assert_eq!(m.triangle_count(), 12);
        approx(m.volume(), 300.0 * 400.0 * 3000.0);
        assert_eq!(m.edges.len(), 12);
    }

    #[test]
    fn concave_slab_volume_and_outward() {
        // L-shaped slab: 6×6 m minus a 3×3 m corner = 27 m², 200 mm thick.
        let g = Generator::Slab {
            outline: vec![
                [0.0, 0.0],
                [6000.0, 0.0],
                [6000.0, 3000.0],
                [3000.0, 3000.0],
                [3000.0, 6000.0],
                [0.0, 6000.0],
            ],
            thickness: 200.0,
        };
        let x = g.extrusion().unwrap();
        let m = extrude(&x.profile, &x.voids, x.depth).unwrap();
        approx(m.volume(), 27.0e6 * 200.0);
    }

    #[test]
    fn wall_starts_at_origin() {
        let g = Generator::Wall {
            length: 5000.0,
            thickness: 200.0,
            height: 2800.0,
        };
        let x = g.extrusion().unwrap();
        let m = extrude(&x.profile, &x.voids, x.depth).unwrap();
        approx(m.volume(), 5000.0 * 200.0 * 2800.0);
        let xmin = m.positions.iter().map(|p| p[0]).fold(f32::MAX, f32::min);
        assert_eq!(xmin, 0.0);
    }

    #[test]
    fn whole_demo_frame_meshes_outward() {
        let model = concrete_frame(&FrameSpec::default());
        for e in model.elements.values() {
            let m = element_mesh(e).unwrap();
            assert!(m.volume() > 0.0, "{} has inward faces", e.name);
        }
    }
}
