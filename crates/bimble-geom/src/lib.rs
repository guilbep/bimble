//! Mesh generation. Meshes are in element-local coordinates (f32, mm) so precision stays good
//! far from the origin; the renderer applies the placement as an instance transform.
//!
//! Faces get their own vertices (flat shading, crisp drawing look). Feature edges are listed
//! separately so the renderer can draw outlines like a technical drawing.

pub mod boolean;
pub mod compare;
pub mod scene;

pub use scene::{
    ElementInfo, GridAxis, MeshBuilder, Scene, SceneBuilder, SceneElement, StoreyLevel,
};
use scene::{cross, norm, sub};

use bimble_core::Generator;

use bimble_core::{Element, Placement, Profile};

#[derive(Debug, Default, Clone)]
pub struct Mesh {
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
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

/// Extrude a counter-clockwise profile along local +Z by `depth`, cutting out clockwise voids.
pub fn extrude(profile: &Profile, voids: &[Vec<[f64; 2]>], depth: f64) -> Result<Mesh, GeomError> {
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
    let d = depth as f32;
    let all: Vec<&[[f64; 2]]> = std::iter::once(&pts[..])
        .chain(voids.iter().map(|v| v.as_slice()))
        .collect();
    let p = |loop_idx: usize, i: usize, z: f32| {
        let q = all[loop_idx][i];
        [q[0] as f32, q[1] as f32, z]
    };
    let mut m = Mesh::default();

    // Bottom cap (normal -Z) gets reversed winding; top cap (normal +Z) keeps the profile winding.
    for (z, nz, rev) in [(0.0, -1.0, true), (d, 1.0, false)] {
        let base = m.positions.len() as u32;
        for (li, l) in all.iter().enumerate() {
            for i in 0..l.len() {
                m.positions.push(p(li, i, z));
                m.normals.push([0.0, 0.0, nz]);
            }
        }
        for t in tris.as_chunks::<3>().0 {
            let t = [t[0], t[1], t[2]];
            let t = if rev { [t[0], t[2], t[1]] } else { t };
            m.indices.extend(t.map(|k| base + k));
        }
    }

    // Side quads, one per profile edge (void edges included), with an outward normal.
    for (li, l) in all.iter().enumerate() {
        for i in 0..l.len() {
            let j = (i + 1) % l.len();
            let (a, b) = (l[i], l[j]);
            let (ex, ey) = (b[0] - a[0], b[1] - a[1]);
            let len = (ex * ex + ey * ey).sqrt();
            let nrm = [(ey / len) as f32, (-ex / len) as f32, 0.0];
            let base = m.positions.len() as u32;
            m.positions
                .extend([p(li, i, 0.0), p(li, j, 0.0), p(li, j, d), p(li, i, d)]);
            m.normals.extend([nrm; 4]);
            m.indices
                .extend([base, base + 1, base + 2, base, base + 2, base + 3]);
            m.edges.push([p(li, i, 0.0), p(li, j, 0.0)]);
            m.edges.push([p(li, i, d), p(li, j, d)]);
            m.edges.push([p(li, i, 0.0), p(li, i, d)]);
        }
    }
    Ok(m)
}

/// A triangle mesh as stored in a `Mesh` generator, with flat per-face normals and edges.
pub fn mesh_from(positions: &[[f64; 3]], indices: &[u32]) -> Result<Mesh, GeomError> {
    if !indices.len().is_multiple_of(3) {
        return Err(GeomError::Triangulation(
            "index count not a multiple of 3".into(),
        ));
    }
    let mut m = Mesh {
        positions: positions.iter().map(|p| p.map(|c| c as f32)).collect(),
        indices: indices.to_vec(),
        ..Default::default()
    };
    for t in indices.as_chunks::<3>().0 {
        let [a, b, c] = t.map(|i| positions[i as usize]);
        let n = norm(cross(sub(b, a), sub(c, a)));
        let n = if n > 1e-12 {
            cross(sub(b, a), sub(c, a)).map(|v| (v / n) as f32)
        } else {
            [0.0, 0.0, 1.0]
        };
        m.normals.extend([n; 3]);
        let seg = [a, b, c].map(|p| p.map(|v| v as f32));
        m.edges.push([seg[0], seg[1]]);
        m.edges.push([seg[1], seg[2]]);
        m.edges.push([seg[2], seg[0]]);
    }
    Ok(m)
}

pub fn element_mesh(e: &Element) -> Result<Mesh, GeomError> {
    match &e.generator {
        Generator::Mesh { positions, indices } => mesh_from(positions, indices),
        g => {
            let x = g
                .extrusion()
                .ok_or_else(|| GeomError::Triangulation("no extrusion".into()))?;
            extrude(&x.profile, &x.voids, x.depth)
        }
    }
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
            normals: vec![[0.0, 0.0, 1.0]; 4],
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
