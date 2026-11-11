//! View cube and compass, drawn in a corner of the 3D view (like Revit's ViewCube).
//!
//! The cube is split into 26 targets: 6 faces, 12 edges, 8 corners. Clicking one turns the camera
//! to look at the model from that direction (Top, Front, an edge at 45°, an isometric corner).
//! Dragging the cube orbits. North is +Y (the IFC TrueNorth is not read yet).
//!
//! Geometry is plain functions on the camera angles, so it is tested without a window.

use eframe::egui::{self, Align2, Color32, FontId, Pos2, Rect, Sense, Shape, Stroke, Vec2};
use glam::Vec3;

/// A view direction on the cube: each component -1, 0 or 1 (not all 0). Faces have one non-zero
/// component, edges two, corners three. It points from the model towards the camera.
pub type Dir = [i8; 3];

/// Camera yaw and pitch (radians) looking at the model from `d`.
pub fn orientation(d: Dir, current_yaw: f32) -> (f32, f32) {
    let v = Vec3::new(d[0] as f32, d[1] as f32, d[2] as f32).normalize();
    let horizontal = Vec3::new(v.x, v.y, 0.0);
    // Straight above or below: keep north up (yaw -90°: looking towards +Y).
    if horizontal.length() < 1e-6 {
        let _ = current_yaw;
        // 1.57: the camera's own pitch limit (Camera::orbit).
        return (-std::f32::consts::FRAC_PI_2, v.z.signum() * 1.57);
    }
    (v.y.atan2(v.x), v.z.asin())
}

pub fn label(d: Dir) -> Option<&'static str> {
    Some(match d {
        [0, 0, 1] => "TOP",
        [0, 0, -1] => "BOTTOM",
        [0, -1, 0] => "FRONT",
        [0, 1, 0] => "BACK",
        [1, 0, 0] => "RIGHT",
        [-1, 0, 0] => "LEFT",
        _ => return None,
    })
}

/// Screen axes of the camera: right, up, towards the viewer (unit vectors, world space).
pub fn camera_axes(yaw: f32, pitch: f32) -> (Vec3, Vec3, Vec3) {
    let (cp, sp) = (pitch.cos(), pitch.sin());
    let back = Vec3::new(cp * yaw.cos(), cp * yaw.sin(), sp);
    // Same "up" choice as Camera::view_proj near the poles.
    let world_up = if pitch.abs() > 1.55 {
        Vec3::new(-yaw.cos(), -yaw.sin(), 0.0)
    } else {
        Vec3::Z
    };
    let right = world_up.cross(back).normalize_or(Vec3::X);
    let up = back.cross(right);
    (right, up, back)
}

/// One clickable cell of a cube face: its screen polygon, its direction and its depth.
pub struct Cell {
    pub poly: [Pos2; 4],
    pub dir: Dir,
    /// Larger is nearer the viewer.
    pub depth: f32,
}

/// Width of the edge and corner bands, as a fraction of the half cube size.
const BAND: f32 = 0.35;

/// Visible cells of the cube, back to front, for a cube of half-size `r` pixels centred on `c`.
pub fn cells(yaw: f32, pitch: f32, c: Pos2, r: f32) -> Vec<Cell> {
    let (right, up, back) = camera_axes(yaw, pitch);
    let to_screen = |p: Vec3| Pos2::new(c.x + p.dot(right) * r, c.y - p.dot(up) * r);
    let bands = [
        (-1.0, -1.0 + BAND),
        (-1.0 + BAND, 1.0 - BAND),
        (1.0 - BAND, 1.0),
    ];
    let mut out = Vec::new();
    for axis in 0..3 {
        for side in [-1i8, 1] {
            let mut n = Vec3::ZERO;
            n[axis] = side as f32;
            if n.dot(back) <= 1e-4 {
                continue;
            }
            let (ua, wa) = ((axis + 1) % 3, (axis + 2) % 3);
            for (bi, &(u0, u1)) in bands.iter().enumerate() {
                for (bj, &(w0, w1)) in bands.iter().enumerate() {
                    let corner = |u: f32, w: f32| {
                        let mut p = n;
                        p[ua] = u;
                        p[wa] = w;
                        to_screen(p)
                    };
                    let mut dir = [0i8; 3];
                    dir[axis] = side;
                    dir[ua] = bi as i8 - 1;
                    dir[wa] = bj as i8 - 1;
                    let mut centre = n;
                    centre[ua] = (u0 + u1) / 2.0;
                    centre[wa] = (w0 + w1) / 2.0;
                    out.push(Cell {
                        poly: [
                            corner(u0, w0),
                            corner(u1, w0),
                            corner(u1, w1),
                            corner(u0, w1),
                        ],
                        dir,
                        depth: centre.dot(back),
                    });
                }
            }
        }
    }
    out.sort_by(|a, b| a.depth.total_cmp(&b.depth));
    out
}

fn inside(poly: &[Pos2; 4], p: Pos2) -> bool {
    // Convex quad, either winding: p is on the same side of every edge.
    let mut sign = 0.0f32;
    for k in 0..4 {
        let (a, b) = (poly[k], poly[(k + 1) % 4]);
        let cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if cross.abs() < 1e-6 {
            continue;
        }
        if sign == 0.0 {
            sign = cross.signum();
        } else if cross.signum() != sign {
            return false;
        }
    }
    true
}

/// The cube direction under a screen point, if any (nearest cell wins).
pub fn hit(cells: &[Cell], p: Pos2) -> Option<Dir> {
    cells
        .iter()
        .rev()
        .find(|c| inside(&c.poly, p))
        .map(|c| c.dir)
}

/// What the user did with the cube this frame.
pub enum Action {
    None,
    /// Turn to look from this direction.
    Look(Dir),
    /// Orbit by a pointer drag (pixels).
    Orbit(Vec2),
    /// Turn around the vertical axis by an angle (compass drag), radians.
    Turn(f32),
}

/// Draws the cube and compass in `area` (square) and returns what was clicked or dragged.
pub fn show(ui: &mut egui::Ui, area: Rect, yaw: f32, pitch: f32, accent: Color32) -> Action {
    let painter = ui.painter_at(area);
    // A little above the centre: the ring and its letters sit below the cube.
    let c = area.center() - Vec2::new(0.0, area.height() * 0.08);
    // Cube and ring sized so the ring and its letters stay inside the square in every view.
    let r = area.width() * 0.19;
    let (right, up, _) = camera_axes(yaw, pitch);
    let to_screen = |p: Vec3| Pos2::new(c.x + p.dot(right) * r, c.y - p.dot(up) * r);

    // Compass ring under the cube, in the horizontal plane: the far half is drawn behind the
    // cube, the near half and its letters in front of it.
    let (_, _, back) = camera_axes(yaw, pitch);
    let ring_r = 1.6;
    let z = -1.15;
    let ring_point = |a: f32| Vec3::new(ring_r * a.cos(), ring_r * a.sin(), z);
    let ring_stroke = Stroke::new(5.0, Color32::from_gray(90));
    let draw_ring = |near: bool| {
        for k in 0..48 {
            let (a0, a1) = (
                k as f32 / 48.0 * std::f32::consts::TAU,
                (k + 1) as f32 / 48.0 * std::f32::consts::TAU,
            );
            let mid = ring_point((a0 + a1) / 2.0);
            if (mid.dot(back) >= 0.0) == near {
                painter.line_segment(
                    [to_screen(ring_point(a0)), to_screen(ring_point(a1))],
                    ring_stroke,
                );
            }
        }
        for (t, p) in [
            ("N", Vec3::Y),
            ("E", Vec3::X),
            ("S", -Vec3::Y),
            ("W", -Vec3::X),
        ] {
            let w = Vec3::new(p.x * ring_r, p.y * ring_r, z);
            if (w.dot(back) >= 0.0) == near {
                let col = if t == "N" {
                    Color32::from_rgb(255, 90, 90)
                } else {
                    Color32::WHITE
                };
                let p = to_screen(w);
                painter.circle_filled(p, 7.5, Color32::from_gray(70));
                painter.text(p, Align2::CENTER_CENTER, t, FontId::proportional(10.0), col);
            }
        }
    };
    draw_ring(false);

    let cs = cells(yaw, pitch, c, r);
    let hover_pos = ui
        .input(|i| i.pointer.hover_pos())
        .filter(|p| area.contains(*p));
    let hovered = hover_pos.and_then(|p| hit(&cs, p));
    for cell in &cs {
        let face = cell.dir.iter().filter(|&&x| x != 0).count() == 1;
        let base = if face {
            Color32::from_gray(214)
        } else {
            Color32::from_gray(196)
        };
        let fill = if Some(cell.dir) == hovered {
            accent
        } else {
            base
        };
        painter.add(Shape::convex_polygon(
            cell.poly.to_vec(),
            fill,
            Stroke::new(0.6, Color32::from_gray(150)),
        ));
    }
    draw_ring(true);
    // Face labels, centred on the visible faces.
    for axis in 0..3 {
        for side in [-1i8, 1] {
            let mut d = [0i8; 3];
            d[axis] = side;
            let n = Vec3::new(d[0] as f32, d[1] as f32, d[2] as f32);
            if n.dot(camera_axes(yaw, pitch).2) > 0.25
                && let Some(t) = label(d)
            {
                painter.text(
                    to_screen(n),
                    Align2::CENTER_CENTER,
                    t,
                    FontId::proportional(9.0),
                    Color32::from_gray(60),
                );
            }
        }
    }

    let resp = ui.interact(
        area,
        ui.scope_id().with("view-cube"),
        Sense::click_and_drag(),
    );
    if resp.dragged() {
        let d = resp.drag_delta();
        if d != Vec2::ZERO {
            // On the ring (below the cube): turn around the vertical axis; on the cube: orbit.
            let on_ring = resp
                .interact_pointer_pos()
                .is_some_and(|p| hit(&cs, p).is_none());
            return if on_ring {
                Action::Turn(d.x * 0.01)
            } else {
                Action::Orbit(d)
            };
        }
    } else if resp.clicked()
        && let Some(d) = resp.interact_pointer_pos().and_then(|p| hit(&cs, p))
    {
        return Action::Look(d);
    }
    if hovered.is_some() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    Action::None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::FRAC_PI_2;

    fn close(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-3
    }

    #[test]
    fn faces_edges_and_corners_give_the_expected_views() {
        // Front: camera at -Y, level.
        let (y, p) = orientation([0, -1, 0], 0.0);
        assert!(close(y, -FRAC_PI_2) && close(p, 0.0));
        // Right: camera at +X.
        let (y, p) = orientation([1, 0, 0], 0.0);
        assert!(close(y, 0.0) && close(p, 0.0));
        // Top: from above, north up.
        let (y, p) = orientation([0, 0, 1], 1.0);
        assert!(close(y, -FRAC_PI_2) && p > 1.55);
        // Edge front-top: 45° down from the front.
        let (y, p) = orientation([0, -1, 1], 0.0);
        assert!(close(y, -FRAC_PI_2) && close(p, std::f32::consts::FRAC_PI_4));
        // Corner: isometric (35.26° elevation).
        let (y, p) = orientation([1, -1, 1], 0.0);
        assert!(close(y, -std::f32::consts::FRAC_PI_4) && close(p, (1.0f32 / 3f32.sqrt()).asin()));
    }

    #[test]
    fn looking_from_a_direction_puts_that_face_in_the_middle_of_the_cube() {
        for d in [
            [0, 0, 1],
            [0, -1, 0],
            [1, 0, 0],
            [-1, 0, 0],
            [0, 1, 0],
            [1, -1, 1],
            [0, -1, 1],
            [-1, 1, -1],
        ] {
            let (y, p) = orientation(d, 0.0);
            let cs = cells(y, p, Pos2::new(100.0, 100.0), 40.0);
            assert_eq!(hit(&cs, Pos2::new(100.0, 100.0)), Some(d), "{d:?}");
        }
    }

    #[test]
    fn only_faces_turned_to_the_viewer_are_drawn() {
        let (y, p) = orientation([0, -1, 0], 0.0);
        let cs = cells(y, p, Pos2::ZERO, 40.0);
        // Straight from the front: only the front face, 9 cells.
        assert_eq!(cs.len(), 9);
        let (y, p) = orientation([1, -1, 1], 0.0);
        assert_eq!(
            cells(y, p, Pos2::ZERO, 40.0).len(),
            27,
            "isometric: three faces"
        );
    }

    #[test]
    fn a_click_beside_the_cube_hits_nothing() {
        let (y, p) = orientation([1, -1, 1], 0.0);
        let cs = cells(y, p, Pos2::new(100.0, 100.0), 40.0);
        assert_eq!(hit(&cs, Pos2::new(5.0, 5.0)), None);
    }
}
