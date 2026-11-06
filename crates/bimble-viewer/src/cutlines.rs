//! Cut outlines on screen (#35): where the faces of the clip box (a plan's cut, a section box)
//! cut the model, computed on the CPU from the meshes and drawn as lines over the picture.
//!
//! The GPU fills cut surfaces but their outline came from the mesh's own edges, which may lie at
//! the same depth as another face (a column standing on a slab: its bottom edges on the slab's
//! top) and lose the depth test. Lines on a front face of the clip box are never hidden: whatever
//! lies in front of that face is clipped away.

use bimble_geom::Scene;
use bimble_render::ClipBox;
use glam::Vec3;

/// At most this many segments (a huge model cut through: the rest is not outlined).
pub const MAX_SEGMENTS: usize = 2_000_000;

/// Segments where face `k` of `b` (axis k / 2, min side for even k) cuts the meshes of the
/// visible elements, clipped to the face's rectangle; for the faces in `faces` only.
pub fn cut_lines(
    scene: &Scene,
    b: &ClipBox,
    faces: [bool; 6],
    visible: impl Fn(usize) -> bool,
) -> Vec<[Vec3; 2]> {
    let mut out = Vec::new();
    for (i, e) in scene.elements.iter().enumerate() {
        if !visible(i) {
            continue;
        }
        let m = &e.mesh;
        let (mut lo, mut hi) = (Vec3::splat(f32::INFINITY), Vec3::splat(f32::NEG_INFINITY));
        for p in &m.positions {
            let p = Vec3::from_array(*p);
            lo = lo.min(p);
            hi = hi.max(p);
        }
        for (k, on) in faces.iter().enumerate() {
            let a = k / 2;
            let v = if k % 2 == 0 { b.min[a] } else { b.max[a] };
            if !on || !(lo[a] < v && hi[a] > v) {
                continue;
            }
            // The other two axes: the face's rectangle.
            let (u, w) = ((a + 1) % 3, (a + 2) % 3);
            if hi[u] < b.min[u] || lo[u] > b.max[u] || hi[w] < b.min[w] || lo[w] > b.max[w] {
                continue;
            }
            for t in m.indices.as_chunks::<3>().0 {
                let p = t.map(|i| Vec3::from_array(m.positions[i as usize]));
                let side = p.map(|q| q[a] >= v);
                let n = side.iter().filter(|&&s| s).count();
                if n == 0 || n == 3 {
                    continue;
                }
                let mut pts = [Vec3::ZERO; 2];
                let mut c = 0;
                for j in 0..3 {
                    let (s, e2) = (p[j], p[(j + 1) % 3]);
                    if side[j] != side[(j + 1) % 3] && c < 2 {
                        let (s, e2) = if s.to_array() <= e2.to_array() {
                            (s, e2)
                        } else {
                            (e2, s)
                        };
                        let f = (v - s[a]) / (e2[a] - s[a]);
                        let mut q = s + (e2 - s) * f;
                        q[a] = v;
                        pts[c] = q;
                        c += 1;
                    }
                }
                if c == 2
                    && let Some(seg) = clip_to_rect(pts, u, w, b)
                {
                    out.push(seg);
                    if out.len() >= MAX_SEGMENTS {
                        return out;
                    }
                }
            }
        }
    }
    out
}

/// A segment in a face of `b` clipped to the face's rectangle (axes u, w), Liang–Barsky.
fn clip_to_rect(s: [Vec3; 2], u: usize, w: usize, b: &ClipBox) -> Option<[Vec3; 2]> {
    let d = s[1] - s[0];
    let (mut t0, mut t1) = (0.0f32, 1.0f32);
    for ax in [u, w] {
        for (p, q) in [
            (-d[ax], s[0][ax] - b.min[ax]),
            (d[ax], b.max[ax] - s[0][ax]),
        ] {
            if p == 0.0 {
                if q < 0.0 {
                    return None;
                }
            } else {
                let r = q / p;
                if p < 0.0 {
                    t0 = t0.max(r);
                } else {
                    t1 = t1.min(r);
                }
            }
        }
    }
    (t0 < t1).then(|| [s[0] + d * t0, s[0] + d * t1])
}

/// Which faces of `b` turn towards a viewer at `eye` (scene coordinates).
pub fn front_faces(b: &ClipBox, eye: Vec3) -> [bool; 6] {
    std::array::from_fn(|k| {
        let a = k / 2;
        if k % 2 == 0 {
            eye[a] < b.min[a]
        } else {
            eye[a] > b.max[a]
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    /// A plan cut through the demo frame's columns at 1.2 m: each 300 × 300 column gives a
    /// closed square outline of 1.2 m on the top face, whatever stands under it.
    #[test]
    fn a_plan_cut_outlines_every_column() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut b = ClipBox::around(&scene, |_| true).unwrap();
        let z = (1200.0 - scene.origin_mm[2]) as f32;
        b.max.z = z;
        let faces = front_faces(&b, Vec3::new(0.0, 0.0, 1e6));
        assert_eq!(faces, [false, false, false, false, false, true]);
        let lines = cut_lines(&scene, &b, faces, |_| true);
        assert!(
            lines
                .iter()
                .all(|s| (s[0].z - z).abs() < 1e-3 && (s[1].z - z).abs() < 1e-3)
        );
        let total: f32 = lines.iter().map(|s| (s[1] - s[0]).length()).sum();
        // 12 columns × 4 × 300 mm.
        assert!((total - 12.0 * 1200.0).abs() < 1.0, "{total}");
        // Hidden: nothing.
        assert!(cut_lines(&scene, &b, faces, |_| false).is_empty());
    }

    #[test]
    fn segments_are_clipped_to_the_face() {
        let b = ClipBox {
            min: Vec3::ZERO,
            max: Vec3::splat(10.0),
        };
        let s = [Vec3::new(-5.0, 5.0, 10.0), Vec3::new(15.0, 5.0, 10.0)];
        let c = clip_to_rect(s, 0, 1, &b).unwrap();
        assert_eq!(c, [Vec3::new(0.0, 5.0, 10.0), Vec3::new(10.0, 5.0, 10.0)]);
        assert!(
            clip_to_rect(
                [Vec3::new(-5.0, 20.0, 10.0), Vec3::new(15.0, 20.0, 10.0)],
                0,
                1,
                &b
            )
            .is_none()
        );
    }
}
