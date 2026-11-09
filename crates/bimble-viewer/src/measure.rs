//! Measuring ("prendre des cotes"): snap to a corner, an edge or a face of the element under the
//! cursor, two clicks make a dimension. Each dimension gives the distance, its X/Y/Z parts and,
//! when both points lie on parallel faces, the perpendicular distance between those faces (a
//! slab's thickness, the gap between two walls).
//!
//! Coordinates are scene coordinates (mm, relative to the scene origin): distances don't depend
//! on the origin.
//!
//! Keys, as in Revit and AutoCAD: Shift keeps the second point on the X, Y or Z axis through the
//! first (Revit's Shift constraint, AutoCAD's Ortho); Alt (Option) measures to the surface under
//! the cursor without snapping; Ctrl (Cmd) + click goes on from that point (a chain); Esc drops
//! the first point, a second Esc leaves the tool; a right click or Enter ends the chain.

use bimble_geom::Mesh;
use bimble_render::Hit;
use eframe::egui::{Pos2, Rect};
use glam::{Mat4, Vec3};

use crate::section::project;

/// Screen distance (pixels) within which a corner wins.
pub const VERTEX_SNAP_PX: f32 = 10.0;
/// Screen distance (pixels) within which an edge wins over the face.
pub const EDGE_SNAP_PX: f32 = 8.0;
/// Faces are "parallel" within this angle: cos(1°).
const PARALLEL_COS: f32 = 0.99985;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SnapKind {
    Vertex,
    Edge,
    Face,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Snap {
    pub point: Vec3,
    pub kind: SnapKind,
    /// The face normal, for points on a face.
    pub normal: Option<Vec3>,
    pub element: usize,
}

/// Closest point on segment `a`–`b` to the line `o + t·d` (d unit).
fn closest_on_segment_to_ray(a: Vec3, b: Vec3, o: Vec3, d: Vec3) -> Vec3 {
    let u = b - a;
    let w = a - o;
    let (uu, ud, dd) = (u.dot(u), u.dot(d), d.dot(d));
    let (uw, dw) = (u.dot(w), d.dot(w));
    let den = uu * dd - ud * ud;
    let s = if den.abs() < 1e-9 * uu.max(1.0) {
        0.0
    } else {
        ((ud * dw - dd * uw) / den).clamp(0.0, 1.0)
    };
    a + u * s
}

/// What the cursor points at on the hit element: a corner, else an edge, else the face.
pub fn snap(
    mesh: &Mesh,
    hit: &Hit,
    ray: (Vec3, Vec3),
    vp: &Mat4,
    rect: Rect,
    cursor: Pos2,
) -> Snap {
    // Corners and edges behind the surface under the cursor are hidden: not snapped. A small
    // tolerance keeps those on the hit face itself (silhouettes, rounding).
    let max_depth = hit.distance + (hit.distance * 0.002).max(5.0);
    let visible = |p: Vec3| (p - ray.0).dot(ray.1) <= max_depth;
    let screen_dist = |p: Vec3| {
        if !visible(p) {
            return f32::INFINITY;
        }
        project(vp, rect, p).map_or(f32::INFINITY, |s| s.distance(cursor))
    };
    let mut best: Option<(f32, Vec3)> = None;
    for p in &mesh.positions {
        let p = Vec3::from(*p);
        let d = screen_dist(p);
        if d <= VERTEX_SNAP_PX && best.is_none_or(|(bd, _)| d < bd) {
            best = Some((d, p));
        }
    }
    if let Some((_, p)) = best {
        return Snap {
            point: p,
            kind: SnapKind::Vertex,
            normal: None,
            element: hit.element,
        };
    }
    for [a, b] in &mesh.edges {
        let p = closest_on_segment_to_ray(Vec3::from(*a), Vec3::from(*b), ray.0, ray.1);
        let d = screen_dist(p);
        if d <= EDGE_SNAP_PX && best.is_none_or(|(bd, _)| d < bd) {
            best = Some((d, p));
        }
    }
    if let Some((_, p)) = best {
        return Snap {
            point: p,
            kind: SnapKind::Edge,
            normal: None,
            element: hit.element,
        };
    }
    Snap {
        point: hit.point,
        kind: SnapKind::Face,
        normal: Some(hit.normal),
        element: hit.element,
    }
}

/// The face point under the cursor, no corner or edge snapping (Alt).
pub fn unsnapped(hit: &Hit) -> Snap {
    Snap {
        point: hit.point,
        kind: SnapKind::Face,
        normal: Some(hit.normal),
        element: hit.element,
    }
}

/// Shift: the second point brought onto the X, Y or Z axis through `from`, the one its offset is
/// longest along. It no longer lies on a face, so it has no normal.
pub fn orthogonal(from: Vec3, to: Snap) -> Snap {
    let d = to.point - from;
    let a = d.abs();
    let axis = if a.x >= a.y && a.x >= a.z {
        Vec3::X
    } else if a.y >= a.z {
        Vec3::Y
    } else {
        Vec3::Z
    };
    Snap {
        point: from + axis * d.dot(axis),
        normal: None,
        ..to
    }
}

/// A dimension between two snapped points.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Dimension {
    pub a: Snap,
    pub b: Snap,
}

impl Dimension {
    pub fn delta(&self) -> Vec3 {
        self.b.point - self.a.point
    }
    pub fn distance(&self) -> f32 {
        self.delta().length()
    }
    /// Distance between the two faces, when both points are on parallel faces.
    pub fn perpendicular(&self) -> Option<f32> {
        let (n1, n2) = (self.a.normal?, self.b.normal?);
        (n1.dot(n2).abs() >= PARALLEL_COS).then(|| self.delta().dot(n1).abs())
    }
}

/// A length in mm for display: whole mm from 100 mm, one decimal below. Thousands separated by
/// a thin space, as on drawings.
pub fn format_mm(x: f32) -> String {
    let s = if x.abs() >= 100.0 {
        format!("{}", x.round() as i64)
    } else {
        format!("{x:.1}")
    };
    let (int, frac) = s
        .split_once('.')
        .map_or((s.as_str(), None), |(i, f)| (i, Some(f)));
    let (sign, digits) = int.strip_prefix('-').map_or(("", int), |d| ("-", d));
    let mut grouped = String::new();
    for (k, c) in digits.chars().enumerate() {
        if k > 0 && (digits.len() - k).is_multiple_of(3) {
            grouped.push('\u{2009}');
        }
        grouped.push(c);
    }
    match frac {
        Some(f) => format!("{sign}{grouped}.{f} mm"),
        None => format!("{sign}{grouped} mm"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Shift keeps the second point on the axis it is nearest to, whatever the sign.
    #[test]
    fn shift_keeps_the_second_point_on_an_axis() {
        let snap = |p: Vec3| Snap {
            point: p,
            kind: SnapKind::Vertex,
            normal: Some(Vec3::Z),
            element: 0,
        };
        let from = Vec3::new(100.0, 200.0, 300.0);
        for (to, want) in [
            (
                Vec3::new(1100.0, 250.0, 280.0),
                Vec3::new(1100.0, 200.0, 300.0),
            ),
            (
                Vec3::new(90.0, -800.0, 310.0),
                Vec3::new(100.0, -800.0, 300.0),
            ),
            (
                Vec3::new(140.0, 230.0, 2300.0),
                Vec3::new(100.0, 200.0, 2300.0),
            ),
        ] {
            let s = orthogonal(from, snap(to));
            assert!((s.point - want).length() < 1e-3, "{to} -> {}", s.point);
            assert_eq!(s.normal, None);
        }
    }
    use eframe::egui::Vec2;

    fn face(p: Vec3, n: Vec3) -> Snap {
        Snap {
            point: p,
            kind: SnapKind::Face,
            normal: Some(n),
            element: 0,
        }
    }

    #[test]
    fn distance_parts_and_perpendicular() {
        // Top and bottom of a 250 mm slab, picked at different places.
        let d = Dimension {
            a: face(Vec3::new(0.0, 0.0, 250.0), Vec3::Z),
            b: face(Vec3::new(3000.0, 4000.0, 0.0), -Vec3::Z),
        };
        assert!(
            (d.distance() - (3000f32.powi(2) + 4000f32.powi(2) + 250f32.powi(2)).sqrt()).abs()
                < 0.01
        );
        assert_eq!(d.delta(), Vec3::new(3000.0, 4000.0, -250.0));
        assert!((d.perpendicular().unwrap() - 250.0).abs() < 1e-3);
        // Not parallel: no perpendicular distance.
        let tilted = Dimension {
            a: face(Vec3::ZERO, Vec3::Z),
            b: face(Vec3::X, Vec3::new(0.0, 0.1, 1.0).normalize()),
        };
        assert!(tilted.perpendicular().is_none());
        // A corner has no face normal.
        let corner = Dimension {
            a: Snap {
                normal: None,
                kind: SnapKind::Vertex,
                ..face(Vec3::ZERO, Vec3::Z)
            },
            b: face(Vec3::X, Vec3::Z),
        };
        assert!(corner.perpendicular().is_none());
    }

    #[test]
    fn closest_point_on_an_edge_to_the_view_ray() {
        // Edge along x at y = 0, z = 0; ray straight down through x = 3.
        let p = closest_on_segment_to_ray(
            Vec3::new(0.0, 0.0, 0.0),
            Vec3::new(10.0, 0.0, 0.0),
            Vec3::new(3.0, 0.5, 100.0),
            -Vec3::Z,
        );
        assert!((p - Vec3::new(3.0, 0.0, 0.0)).length() < 1e-4, "{p}");
        // Beyond the end: clamped to the end.
        let p = closest_on_segment_to_ray(Vec3::ZERO, Vec3::X, Vec3::new(5.0, 0.0, 10.0), -Vec3::Z);
        assert_eq!(p, Vec3::X);
    }

    #[test]
    fn snapping_prefers_corners_then_edges_then_the_face() {
        // A 1000 mm square in z = 0, seen from above with an orthographic-like projection:
        // scene mm → screen px with 1 px = 10 mm, origin at the rect's top-left.
        let mut m = Mesh::default();
        let q = [
            [0.0, 0.0, 0.0],
            [1000.0, 0.0, 0.0],
            [1000.0, 1000.0, 0.0],
            [0.0, 1000.0, 0.0],
        ];
        m.positions = q.to_vec();
        m.edges = vec![[q[0], q[1]], [q[1], q[2]], [q[2], q[3]], [q[3], q[0]]];
        let rect = Rect::from_min_size(Pos2::ZERO, Vec2::new(200.0, 200.0));
        // x: 0..2000 mm → ndc -1..1; y: 0..2000 mm → ndc 1..-1 (screen y down).
        let vp = Mat4::from_cols_array(&[
            0.001, 0.0, 0.0, 0.0, //
            0.0, 0.001, 0.0, 0.0, //
            0.0, 0.0, 0.0, 0.0, //
            -1.0, -1.0, 0.5, 1.0,
        ]);
        let at = |px: Pos2| {
            let p = Vec3::new(px.x * 10.0, (200.0 - px.y) * 10.0, 0.0);
            let hit = Hit {
                element: 0,
                // The ray starts 1000 mm above the face.
                distance: 1000.0,
                point: p,
                normal: Vec3::Z,
            };
            snap(&m, &hit, (p + Vec3::Z * 1000.0, -Vec3::Z), &vp, rect, px)
        };
        // Near the corner (1000, 1000) mm = (100, 100) px.
        let s = at(Pos2::new(104.0, 103.0));
        assert_eq!(s.kind, SnapKind::Vertex);
        assert_eq!(s.point, Vec3::new(1000.0, 1000.0, 0.0));
        // Near the middle of the edge x = 1000 mm, away from corners.
        let s = at(Pos2::new(105.0, 150.0));
        assert_eq!(s.kind, SnapKind::Edge);
        assert!(
            (s.point - Vec3::new(1000.0, 500.0, 0.0)).length() < 1.0,
            "{}",
            s.point
        );
        // In the middle of the square.
        let s = at(Pos2::new(50.0, 150.0));
        assert_eq!(s.kind, SnapKind::Face);
        assert_eq!(s.normal, Some(Vec3::Z));
    }

    /// Review 2026-11-05: in the middle of a 220 mm slab, the slab's bottom edge (hidden under the
    /// top face) projected right under the cursor and won over the face.
    #[test]
    fn hidden_corners_and_edges_are_not_snapped() {
        let mut m = Mesh::default();
        let q = [
            [0.0, 0.0, 0.0],
            [1000.0, 0.0, 0.0],
            [1000.0, 1000.0, 0.0],
            [0.0, 1000.0, 0.0],
        ];
        m.positions = q.to_vec();
        // An edge and a corner 220 mm below the face, straight under the cursor.
        m.positions.push([500.0, 1500.0, -220.0]);
        m.edges = vec![[[0.0, 1500.0, -220.0], [1000.0, 1500.0, -220.0]]];
        let rect = Rect::from_min_size(Pos2::ZERO, Vec2::new(200.0, 200.0));
        let vp = Mat4::from_cols_array(&[
            0.001, 0.0, 0.0, 0.0, //
            0.0, 0.001, 0.0, 0.0, //
            0.0, 0.0, 0.0, 0.0, //
            -1.0, -1.0, 0.5, 1.0,
        ]);
        // Cursor over (500, 1500) mm on the face z = 0, seen from above.
        let cursor = Pos2::new(50.0, 50.0);
        let p = Vec3::new(500.0, 1500.0, 0.0);
        let hit = Hit {
            element: 0,
            distance: 1000.0,
            point: p,
            normal: Vec3::Z,
        };
        let s = snap(
            &m,
            &hit,
            (p + Vec3::Z * 1000.0, -Vec3::Z),
            &vp,
            rect,
            cursor,
        );
        assert_eq!(s.kind, SnapKind::Face, "snapped to {s:?}");
    }

    #[test]
    fn lengths_read_like_on_a_drawing() {
        assert_eq!(format_mm(3245.4), "3\u{2009}245 mm");
        assert_eq!(format_mm(1234567.0), "1\u{2009}234\u{2009}567 mm");
        assert_eq!(format_mm(250.0), "250 mm");
        assert_eq!(format_mm(12.345), "12.3 mm");
        assert_eq!(format_mm(-4500.0), "-4\u{2009}500 mm");
        assert_eq!(format_mm(0.0), "0.0 mm");
    }
}
