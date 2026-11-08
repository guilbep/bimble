//! Geometry for drawings: horizontal sections of meshes, and segments clipped to a height range.
//!
//! Points are scene coordinates (mm). Sections are computed in f64.

use bimble_geom::Mesh;
use std::collections::HashMap;

pub type P2 = [f64; 2];

/// What a horizontal plane cuts out of one mesh.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct Section {
    /// Closed loops (outer outlines and holes; draw with the even-odd rule).
    pub loops: Vec<Vec<P2>>,
    /// Open chains, where the mesh is not closed: drawn as lines, never filled.
    pub open: Vec<Vec<P2>>,
}

/// Points closer than this are the same point when chaining segments (mm).
const WELD_MM: f64 = 1e-4;

fn key(p: P2) -> (i64, i64) {
    (
        (p[0] / WELD_MM).round() as i64,
        (p[1] / WELD_MM).round() as i64,
    )
}

/// Intersection of edge a–b with the plane z = h, computed from the endpoints in a fixed order
/// so the two triangles sharing the edge get bit-identical points.
fn edge_point(a: [f64; 3], b: [f64; 3], h: f64) -> P2 {
    let (a, b) = if a <= b { (a, b) } else { (b, a) };
    let t = (h - a[2]) / (b[2] - a[2]);
    [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]
}

/// The section of a mesh by the plane z = h. Vertices exactly on the plane count as above it,
/// so faces lying in the plane don't produce degenerate segments.
pub fn section(mesh: &Mesh, h: f64) -> Section {
    let mut segs: Vec<[P2; 2]> = Vec::new();
    for t in mesh.indices.as_chunks::<3>().0 {
        let p = t.map(|i| mesh.positions[i as usize].map(f64::from));
        let above = p.map(|q| q[2] >= h);
        let n_above = above.iter().filter(|&&x| x).count();
        if n_above == 0 || n_above == 3 {
            continue;
        }
        // The two edges whose ends are on different sides, in triangle order.
        let mut pts = Vec::with_capacity(2);
        for k in 0..3 {
            let (i, j) = (k, (k + 1) % 3);
            if above[i] != above[j] {
                pts.push(edge_point(p[i], p[j], h));
            }
        }
        if pts.len() == 2 && key(pts[0]) != key(pts[1]) {
            segs.push([pts[0], pts[1]]);
        }
    }
    chain(segs)
}

/// Joins segments that share end points into loops (closed) and chains (open).
pub fn chain(segs: Vec<[P2; 2]>) -> Section {
    let mut at: HashMap<(i64, i64), Vec<usize>> = HashMap::new();
    for (i, s) in segs.iter().enumerate() {
        at.entry(key(s[0])).or_default().push(i);
        at.entry(key(s[1])).or_default().push(i);
    }
    let mut used = vec![false; segs.len()];
    let mut out = Section::default();
    // Start at segment `start` (reversed if asked) and extend from its last point until the
    // chain closes or stops.
    let walk = |start: usize, reverse: bool, used: &mut Vec<bool>| -> (Vec<P2>, bool) {
        used[start] = true;
        let [a, b] = segs[start];
        let mut pts = if reverse { vec![b, a] } else { vec![a, b] };
        loop {
            let end = key(*pts.last().unwrap());
            let next = at
                .get(&end)
                .and_then(|v| v.iter().copied().find(|&j| !used[j]));
            let Some(j) = next else { break };
            used[j] = true;
            let s = segs[j];
            let p = if key(s[0]) == end { s[1] } else { s[0] };
            if key(p) == key(pts[0]) {
                return (pts, true);
            }
            pts.push(p);
        }
        (pts, false)
    };
    let free = |p: P2| at.get(&key(p)).is_some_and(|v| v.len() == 1);
    // Open chains first, each from one of its free ends, so none is split in two.
    for (i, s) in segs.iter().enumerate() {
        if used[i] {
            continue;
        }
        let reverse = if free(s[0]) {
            false
        } else if free(s[1]) {
            true
        } else {
            continue;
        };
        let (pts, closed) = walk(i, reverse, &mut used);
        if closed {
            out.loops.push(pts);
        } else {
            out.open.push(pts);
        }
    }
    for i in 0..segs.len() {
        if !used[i] {
            let (pts, closed) = walk(i, false, &mut used);
            if closed {
                out.loops.push(pts);
            } else {
                out.open.push(pts);
            }
        }
    }
    out
}

/// The part of segment a–b with z in [z0, z1], projected to the plan.
pub fn clip_z(a: [f64; 3], b: [f64; 3], z0: f64, z1: f64) -> Option<[P2; 2]> {
    let (mut t0, mut t1) = (0.0f64, 1.0f64);
    let dz = b[2] - a[2];
    if dz.abs() < 1e-12 {
        if a[2] < z0 || a[2] > z1 {
            return None;
        }
    } else {
        let (ta, tb) = ((z0 - a[2]) / dz, (z1 - a[2]) / dz);
        let (lo, hi) = if ta < tb { (ta, tb) } else { (tb, ta) };
        t0 = t0.max(lo);
        t1 = t1.min(hi);
        if t0 > t1 {
            return None;
        }
    }
    let at = |t: f64| [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
    Some([at(t0), at(t1)])
}

/// Signed area (counter-clockwise positive).
pub fn area(l: &[P2]) -> f64 {
    let n = l.len();
    (0..n)
        .map(|i| {
            let (a, b) = (l[i], l[(i + 1) % n]);
            a[0] * b[1] - b[0] * a[1]
        })
        .sum::<f64>()
        / 2.0
}

type Key3 = (i64, i64, i64);

/// Upward faces steeper than this (|n.z| of a unit normal) count as seen from above.
const FACING_MIN_NZ: f64 = 0.02;

/// What of a mesh is seen from above: the outline (loops, even-odd) of all its faces turned
/// upwards, projected on the plan. A flat top gives its outline; a sloped slab, a ramp, a stair
/// (its treads) or a stepped footing give the whole area they cover, not only a slice near their
/// highest point.
pub fn facing_up_region(mesh: &Mesh) -> Vec<Vec<P2>> {
    // Edges of the upward triangles, keyed by their 3D end points: an edge shared by two of
    // them is inside the region; one used once is on its outline.
    let key3 = |p: [f64; 3]| {
        (
            (p[0] / WELD_MM).round() as i64,
            (p[1] / WELD_MM).round() as i64,
            (p[2] / WELD_MM).round() as i64,
        )
    };
    let mut count: HashMap<(Key3, Key3), (usize, [P2; 2])> = HashMap::new();
    for t in mesh.indices.as_chunks::<3>().0 {
        let p = t.map(|i| mesh.positions[i as usize].map(f64::from));
        let (u, v) = (
            [p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]],
            [p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]],
        );
        let n = [
            u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0],
        ];
        let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
        if len < 1e-12 || n[2] / len <= FACING_MIN_NZ {
            continue;
        }
        for k in 0..3 {
            let (a, b) = (p[k], p[(k + 1) % 3]);
            let (ka, kb) = (key3(a), key3(b));
            let id = if ka <= kb { (ka, kb) } else { (kb, ka) };
            count
                .entry(id)
                .or_insert((0, [[a[0], a[1]], [b[0], b[1]]]))
                .0 += 1;
        }
    }
    let mut segs: Vec<[P2; 2]> = count
        .into_values()
        .filter(|(n, s)| *n == 1 && key(s[0]) != key(s[1]))
        .map(|(_, s)| s)
        .collect();
    // HashMap order is random: sort so the same mesh always gives the same loops.
    segs.sort_by(|a, b| {
        a.iter()
            .flatten()
            .zip(b.iter().flatten())
            .map(|(x, y)| x.total_cmp(y))
            .find(|o| o.is_ne())
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    chain(segs).loops
}

/// Whether an edge can be seen from above: one of its two faces turns upwards. Edges whose faces
/// both turn down or sideways (a stair's underside, a beam's bottom) are hidden by their own
/// solid. Unknown normals (zero) count as seen.
pub fn edge_seen_from_above(n: &bimble_geom::EdgeNormals) -> bool {
    let unknown = |v: &[i8; 4]| v[0] == 0 && v[1] == 0 && v[2] == 0;
    if unknown(&n[0]) || unknown(&n[1]) {
        return true;
    }
    // snorm8: 127 = 1. A face turned up more than ~1°.
    n[0][2] > 2 || n[1][2] > 2
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_geom::MeshBuilder;

    /// A closed box from (x0, y0, z0) to (x1, y1, z1), outward winding.
    fn cuboid(lo: [f64; 3], hi: [f64; 3]) -> Mesh {
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
        mb.build([0.0; 3])
    }

    #[test]
    fn a_wall_cut_halfway_gives_its_rectangle() {
        let m = cuboid([0.0, 0.0, 0.0], [5000.0, 200.0, 2800.0]);
        let s = section(&m, 1200.0);
        assert_eq!(s.loops.len(), 1, "{s:?}");
        assert!(s.open.is_empty());
        assert!((area(&s.loops[0]).abs() - 5000.0 * 200.0).abs() < 1e-6);
    }

    #[test]
    fn cut_at_a_face_or_outside_gives_nothing_degenerate() {
        let m = cuboid([0.0, 0.0, 0.0], [1000.0, 1000.0, 1000.0]);
        assert_eq!(section(&m, 2000.0), Section::default());
        assert_eq!(section(&m, -1.0), Section::default());
        // Exactly at the top or bottom face: vertices on the plane count as above, so the top
        // face plane gives nothing and the bottom face plane gives the full square... or nothing,
        // but never a broken loop.
        for h in [0.0, 1000.0] {
            let s = section(&m, h);
            assert!(s.open.is_empty(), "h = {h}: {s:?}");
        }
    }

    #[test]
    fn a_hollow_section_has_two_loops() {
        // A square tube: outer box minus inner box, as two closed shells (outer outward, inner
        // inward) like a mesh boolean would give.
        let outer = cuboid([0.0, 0.0, 0.0], [1000.0, 1000.0, 3000.0]);
        let inner = cuboid([200.0, 200.0, -10.0], [800.0, 800.0, 3010.0]);
        let mut m = outer.clone();
        let base = m.positions.len() as u32;
        m.positions.extend(inner.positions.iter());
        // Inner shell reversed (faces point into the hole).
        for t in inner.indices.as_chunks::<3>().0 {
            m.indices.extend([t[0] + base, t[2] + base, t[1] + base]);
        }
        let s = section(&m, 1500.0);
        assert_eq!(s.loops.len(), 2);
        let mut areas: Vec<f64> = s.loops.iter().map(|l| area(l).abs()).collect();
        areas.sort_by(f64::total_cmp);
        assert!((areas[0] - 600.0 * 600.0).abs() < 1e-6);
        assert!((areas[1] - 1000.0 * 1000.0).abs() < 1e-6);
    }

    #[test]
    fn an_open_surface_gives_an_open_chain() {
        // A single vertical sheet: no volume, so its section is a line, not a loop.
        let mut mb = MeshBuilder::default();
        let (a, b, c, d) = (
            [0.0, 0.0, 0.0],
            [1000.0, 0.0, 0.0],
            [1000.0, 0.0, 1000.0],
            [0.0, 0.0, 1000.0],
        );
        mb.add_triangle([a, b, c]);
        mb.add_triangle([a, c, d]);
        let s = section(&mb.build([0.0; 3]), 500.0);
        assert!(s.loops.is_empty());
        assert_eq!(s.open.len(), 1);
        let l = &s.open[0];
        let len =
            ((l[l.len() - 1][0] - l[0][0]).powi(2) + (l[l.len() - 1][1] - l[0][1]).powi(2)).sqrt();
        assert!((len - 1000.0).abs() < 1e-9, "{l:?}");
    }

    #[test]
    fn segments_are_clipped_to_the_height_range() {
        // A vertical edge from z 0 to 3000 projects to a point (length 0) inside the range.
        let s = clip_z([1.0, 2.0, 0.0], [1.0, 2.0, 3000.0], 0.0, 1200.0).unwrap();
        assert_eq!(s, [[1.0, 2.0], [1.0, 2.0]]);
        // A sloped edge from (0,0,0) to (1000,0,2000), kept between z 500 and 1000.
        let s = clip_z([0.0, 0.0, 0.0], [1000.0, 0.0, 2000.0], 500.0, 1000.0).unwrap();
        assert_eq!(s, [[250.0, 0.0], [500.0, 0.0]]);
        // Entirely above.
        assert!(clip_z([0.0, 0.0, 2000.0], [5.0, 0.0, 2000.0], 0.0, 1200.0).is_none());
    }
}
