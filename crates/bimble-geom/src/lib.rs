//! Mesh generation. Meshes are in element-local coordinates (f32, mm) so precision stays good
//! far from the origin; the renderer applies the placement as an instance transform.
//!
//! Faces get their own vertices (flat shading, crisp drawing look). Feature edges are listed
//! separately so the renderer can draw outlines like a technical drawing.

pub mod boolean;
pub mod compare;
pub mod scene;

pub use scene::{ElementInfo, MeshBuilder, Scene, SceneBuilder, SceneElement, StoreyLevel};

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

/// Extrude a counter-clockwise profile along local +Z by `depth`.
pub fn extrude(profile: &Profile, depth: f64) -> Result<Mesh, GeomError> {
    let pts = profile_points(profile);
    let n = pts.len();
    let flat: Vec<f64> = pts.iter().flatten().copied().collect();
    let tris =
        earcutr::earcut(&flat, &[], 2).map_err(|e| GeomError::Triangulation(format!("{e:?}")))?;
    if tris.len() / 3 != n - 2 {
        return Err(GeomError::Triangulation(format!(
            "expected {} triangles, got {}",
            n - 2,
            tris.len() / 3
        )));
    }
    let d = depth as f32;
    let p = |i: usize, z: f32| [pts[i][0] as f32, pts[i][1] as f32, z];
    let mut m = Mesh::default();

    // Bottom cap (normal -Z) gets reversed winding; top cap (normal +Z) keeps the profile winding.
    for (z, nz, rev) in [(0.0, -1.0, true), (d, 1.0, false)] {
        let base = m.positions.len() as u32;
        for i in 0..n {
            m.positions.push(p(i, z));
            m.normals.push([0.0, 0.0, nz]);
        }
        for t in tris.as_chunks::<3>().0 {
            let t = [t[0] as u32, t[1] as u32, t[2] as u32];
            let t = if rev { [t[0], t[2], t[1]] } else { t };
            m.indices.extend(t.map(|k| base + k));
        }
    }

    // Side quads, one per profile edge, with an outward normal.
    for i in 0..n {
        let j = (i + 1) % n;
        let (a, b) = (pts[i], pts[j]);
        let (ex, ey) = (b[0] - a[0], b[1] - a[1]);
        let len = (ex * ex + ey * ey).sqrt();
        let nrm = [(ey / len) as f32, (-ex / len) as f32, 0.0];
        let base = m.positions.len() as u32;
        m.positions.extend([p(i, 0.0), p(j, 0.0), p(j, d), p(i, d)]);
        m.normals.extend([nrm; 4]);
        m.indices
            .extend([base, base + 1, base + 2, base, base + 2, base + 3]);
        m.edges.push([p(i, 0.0), p(j, 0.0)]);
        m.edges.push([p(i, d), p(j, d)]);
        m.edges.push([p(i, 0.0), p(i, d)]);
    }
    Ok(m)
}

pub fn element_mesh(e: &Element) -> Result<Mesh, GeomError> {
    extrude(&e.generator.profile(), e.generator.depth())
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
        let mut b = extrude(&g.profile(), g.depth()).unwrap();
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
        let m = extrude(&g.profile(), g.depth()).unwrap();
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
        let m = extrude(&g.profile(), g.depth()).unwrap();
        approx(m.volume(), 27.0e6 * 200.0);
    }

    #[test]
    fn wall_starts_at_origin() {
        let g = Generator::Wall {
            length: 5000.0,
            thickness: 200.0,
            height: 2800.0,
        };
        let m = extrude(&g.profile(), g.depth()).unwrap();
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
