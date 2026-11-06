//! Snapping while placing elements (#25): to grid-axis crossings, column centres, element
//! corners, then grid axes and element edges; else the point rounded to 10 mm.
//!
//! Everything here is in plan, model mm (world x, y). Targets are built once per storey and
//! scene revision; a snap is a nearest search within a tolerance given in mm (the caller turns a
//! screen distance into mm at the current zoom).

use bimble_geom::Scene;

/// What a point snapped to, strongest first.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum SnapTo {
    AxisCrossing,
    ColumnCentre,
    Corner,
    Axis,
    Edge,
}

impl SnapTo {
    pub fn label(self) -> &'static str {
        match self {
            SnapTo::AxisCrossing => "axis crossing",
            SnapTo::ColumnCentre => "column centre",
            SnapTo::Corner => "corner",
            SnapTo::Axis => "grid axis",
            SnapTo::Edge => "edge",
        }
    }
}

/// At most this many points and lines are kept (large models: the nearest search stays cheap).
const MAX_TARGETS: usize = 200_000;

#[derive(Clone, Debug, Default)]
pub struct Targets {
    pub points: Vec<([f64; 2], SnapTo)>,
    pub lines: Vec<([f64; 2], [f64; 2], SnapTo)>,
}

/// Cluster sorted values closer than `tol` (their mean).
fn clusters(mut v: Vec<f64>, tol: f64) -> Vec<f64> {
    v.sort_by(f64::total_cmp);
    let mut out: Vec<(f64, usize)> = Vec::new();
    for x in v {
        match out.last_mut() {
            Some((m, n)) if (x - *m / *n as f64).abs() <= tol => {
                *m += x;
                *n += 1;
            }
            _ => out.push((x, 1)),
        }
    }
    out.into_iter().map(|(m, n)| m / n as f64).collect()
}

/// Where two lines cross (as infinite lines), if they are not parallel.
fn crossing(a: ([f64; 2], [f64; 2]), b: ([f64; 2], [f64; 2])) -> Option<[f64; 2]> {
    let d1 = [a.1[0] - a.0[0], a.1[1] - a.0[1]];
    let d2 = [b.1[0] - b.0[0], b.1[1] - b.0[1]];
    let den = d1[0] * d2[1] - d1[1] * d2[0];
    let scale = d1[0].hypot(d1[1]) * d2[0].hypot(d2[1]);
    if den.abs() <= 1e-9 * scale.max(1e-12) {
        return None;
    }
    let w = [b.0[0] - a.0[0], b.0[1] - a.0[1]];
    let t = (w[0] * d2[1] - w[1] * d2[0]) / den;
    Some([a.0[0] + d1[0] * t, a.0[1] + d1[1] * t])
}

/// The snap targets of the storey at `level` (world mm) up to `next` (the storey above, or
/// level + 4 m): grid axes (from the model, else through the columns) and their crossings, the
/// centres of columns, corners and edges of the elements in that height band.
pub fn targets(scene: &Scene, level: f64, next: f64) -> Targets {
    let o = scene.origin_mm;
    let mut t = Targets::default();
    let mut axes: Vec<([f64; 2], [f64; 2])> = scene
        .grids
        .iter()
        .map(|g| (g.start_mm, g.end_mm))
        .filter(|(a, b)| (b[0] - a[0]).hypot(b[1] - a[1]) > 1.0)
        .collect();
    let mut centres = Vec::new();
    let mut corners = std::collections::BTreeSet::new();
    let mut edges = std::collections::BTreeSet::new();
    let key = |p: [f64; 2]| [(p[0] * 10.0).round() as i64, (p[1] * 10.0).round() as i64];
    let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
    for e in &scene.elements {
        let m = &e.mesh;
        let (mut zmin, mut zmax) = (f64::INFINITY, f64::NEG_INFINITY);
        let (mut elo, mut ehi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
        for p in &m.positions {
            let w = [p[0] as f64 + o[0], p[1] as f64 + o[1], p[2] as f64 + o[2]];
            zmin = zmin.min(w[2]);
            zmax = zmax.max(w[2]);
            for k in 0..2 {
                elo[k] = elo[k].min(w[k]);
                ehi[k] = ehi[k].max(w[k]);
            }
        }
        // In the storey's height band (a footing below it counts when it reaches its level).
        if !(zmax >= level - 1.0 && zmin <= next + 1.0) || !elo[0].is_finite() {
            continue;
        }
        for k in 0..2 {
            lo[k] = lo[k].min(elo[k]);
            hi[k] = hi[k].max(ehi[k]);
        }
        if e.class.eq_ignore_ascii_case("IfcColumn") {
            centres.push([(elo[0] + ehi[0]) / 2.0, (elo[1] + ehi[1]) / 2.0]);
        }
        for s in &m.edges {
            let a = [s[0][0] as f64 + o[0], s[0][1] as f64 + o[1]];
            let b = [s[1][0] as f64 + o[0], s[1][1] as f64 + o[1]];
            if corners.len() < MAX_TARGETS {
                corners.insert(key(a));
                corners.insert(key(b));
            }
            if (b[0] - a[0]).hypot(b[1] - a[1]) > 1.0 && edges.len() < MAX_TARGETS {
                let (ka, kb) = (key(a), key(b));
                edges.insert(if ka <= kb { (ka, kb) } else { (kb, ka) });
            }
        }
    }
    // No grid in the model: axes through the columns, across what is on the storey.
    if axes.is_empty() && !centres.is_empty() {
        let (m0, m1) = (
            [lo[0] - 1000.0, lo[1] - 1000.0],
            [hi[0] + 1000.0, hi[1] + 1000.0],
        );
        for x in clusters(centres.iter().map(|c| c[0]).collect(), 50.0) {
            axes.push(([x, m0[1]], [x, m1[1]]));
        }
        for y in clusters(centres.iter().map(|c| c[1]).collect(), 50.0) {
            axes.push(([m0[0], y], [m1[0], y]));
        }
    }
    for (i, a) in axes.iter().enumerate() {
        for b in &axes[i + 1..] {
            if let Some(p) = crossing(*a, *b) {
                t.points.push((p, SnapTo::AxisCrossing));
            }
        }
    }
    t.points
        .extend(centres.into_iter().map(|c| (c, SnapTo::ColumnCentre)));
    let back = |k: [i64; 2]| [k[0] as f64 / 10.0, k[1] as f64 / 10.0];
    t.points
        .extend(corners.into_iter().map(|k| (back(k), SnapTo::Corner)));
    t.lines
        .extend(axes.into_iter().map(|(a, b)| (a, b, SnapTo::Axis)));
    t.lines.extend(
        edges
            .into_iter()
            .map(|(a, b)| (back(a), back(b), SnapTo::Edge)),
    );
    t
}

/// The nearest point of segment a–b to p.
fn on_segment(p: [f64; 2], a: [f64; 2], b: [f64; 2]) -> [f64; 2] {
    let d = [b[0] - a[0], b[1] - a[1]];
    let l2 = d[0] * d[0] + d[1] * d[1];
    if l2 <= 0.0 {
        return a;
    }
    let s = (((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]) / l2).clamp(0.0, 1.0);
    [a[0] + d[0] * s, a[1] + d[1] * s]
}

/// `p` snapped: the strongest kind of point within `tol` mm (the nearest of that kind), else the
/// nearest axis or edge within `tol`, else `p` rounded to 10 mm.
pub fn snap(p: [f64; 2], tol: f64, t: &Targets) -> ([f64; 2], Option<SnapTo>) {
    let dist = |q: [f64; 2]| (q[0] - p[0]).hypot(q[1] - p[1]);
    let best = t
        .points
        .iter()
        .filter(|(q, _)| dist(*q) <= tol)
        .min_by(|a, b| {
            (a.1, dist(a.0))
                .partial_cmp(&(b.1, dist(b.0)))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
    if let Some((q, k)) = best {
        return (*q, Some(*k));
    }
    let best = t
        .lines
        .iter()
        .map(|(a, b, k)| (on_segment(p, *a, *b), *k))
        .filter(|(q, _)| dist(*q) <= tol)
        .min_by(|a, b| {
            (a.1, dist(a.0))
                .partial_cmp(&(b.1, dist(b.0)))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
    if let Some((q, k)) = best {
        // Along the line, the position is rounded to 10 mm where the line is along x or y.
        let r = |v: f64| (v / 10.0).round() * 10.0;
        let q = if t
            .lines
            .iter()
            .any(|(a, b, _)| (a[0] - b[0]).abs() < 1e-9 && (a[0] - q[0]).abs() < 1e-9)
        {
            [q[0], r(q[1])]
        } else if t
            .lines
            .iter()
            .any(|(a, b, _)| (a[1] - b[1]).abs() < 1e-9 && (a[1] - q[1]).abs() < 1e-9)
        {
            [r(q[0]), q[1]]
        } else {
            q
        };
        return (q, Some(k));
    }
    let r = |v: f64| (v / 10.0).round() * 10.0;
    ([r(p[0]), r(p[1])], None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn demo() -> (Scene, f64, f64) {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let lv = scene.levels();
        let (level, next) = (
            lv[0].elevation_mm,
            lv.get(1).map_or(3000.0, |l| l.elevation_mm),
        );
        (scene, level, next)
    }

    /// The demo frame's columns are 6 × 5 m apart: a click 40 mm off a column centre lands on
    /// it exactly, so a beam from column to column is placed centre to centre.
    #[test]
    fn a_click_near_a_column_lands_on_its_centre() {
        let (scene, level, next) = demo();
        let t = targets(&scene, level, next);
        let (p, k) = snap([6037.0, 4958.0], 100.0, &t);
        assert_eq!(p, [6000.0, 5000.0]);
        // The demo has no IfcGrid: axes come from the columns, so their crossing wins.
        assert_eq!(k, Some(SnapTo::AxisCrossing));
        let (p, _) = snap([12_000.0 + 31.0, -22.0], 100.0, &t);
        assert_eq!(p, [12_000.0, 0.0]);
    }

    #[test]
    fn near_an_axis_only_slides_along_it_and_far_rounds() {
        let (scene, level, next) = demo();
        let t = targets(&scene, level, next);
        // Midway between columns, 30 mm off the axis y = 5000 and away from any element.
        let (p, k) = snap([3004.0, 5030.0], 50.0, &t);
        assert_eq!(k, Some(SnapTo::Axis));
        assert_eq!(p, [3000.0, 5000.0]);
        let (p, k) = snap([3004.0, 2533.0], 50.0, &t);
        assert_eq!(k, None);
        assert_eq!(p, [3000.0, 2530.0]);
    }

    #[test]
    fn model_grids_are_used_and_cross() {
        let mut scene = Scene::default();
        scene.grids = vec![
            bimble_geom::scene::GridAxis {
                tag: "A".into(),
                start_mm: [0.0, -1000.0],
                end_mm: [0.0, 9000.0],
            },
            bimble_geom::scene::GridAxis {
                tag: "1".into(),
                start_mm: [-1000.0, 2500.0],
                end_mm: [9000.0, 2500.0],
            },
        ];
        let t = targets(&scene, 0.0, 3000.0);
        assert_eq!(
            snap([20.0, 2480.0], 50.0, &t),
            ([0.0, 2500.0], Some(SnapTo::AxisCrossing))
        );
    }

    #[test]
    fn crossing_of_parallel_lines_is_none() {
        assert!(crossing(([0.0, 0.0], [1.0, 0.0]), ([0.0, 1.0], [2.0, 1.0])).is_none());
        assert_eq!(
            crossing(([0.0, 0.0], [2.0, 2.0]), ([0.0, 2.0], [2.0, 0.0])),
            Some([1.0, 1.0])
        );
    }
}
