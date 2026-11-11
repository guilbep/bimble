//! Section box overlay: the box outline and one drag handle per face, drawn over the 3D view.
//!
//! The clipping itself happens on the GPU (`DrawOptions::clip`); this module only shows the box
//! and turns handle drags into face moves. A drag is projected onto the face's axis as it appears
//! on screen, so the face follows the mouse whatever the view direction.

use bimble_render::ClipBox;
use eframe::egui::{self, Color32, Pos2, Rect, Sense, Stroke, Vec2};
use glam::{Mat4, Vec3};

/// Smallest box size along an axis, mm.
const MIN_SIZE: f32 = 10.0;

/// Screen position of a scene point, or None if it is behind the camera.
pub fn project(vp: &Mat4, rect: Rect, p: Vec3) -> Option<Pos2> {
    let c = *vp * p.extend(1.0);
    if c.w <= 1e-6 {
        return None;
    }
    let (x, y) = (c.x / c.w, c.y / c.w);
    Some(Pos2::new(
        rect.min.x + (x + 1.0) * 0.5 * rect.width(),
        rect.min.y + (1.0 - y) * 0.5 * rect.height(),
    ))
}

fn corner(b: &ClipBox, i: usize) -> Vec3 {
    Vec3::new(
        if i & 1 == 0 { b.min.x } else { b.max.x },
        if i & 2 == 0 { b.min.y } else { b.max.y },
        if i & 4 == 0 { b.min.z } else { b.max.z },
    )
}

/// Face k: axis k / 2, at the min (even k) or max (odd k) side.
fn face_center(b: &ClipBox, k: usize) -> Vec3 {
    let mut c = (b.min + b.max) * 0.5;
    let a = k / 2;
    c[a] = if k.is_multiple_of(2) {
        b.min[a]
    } else {
        b.max[a]
    };
    c
}

/// Draws the box and its handles; applies handle drags to `b`. Returns true if `b` changed.
pub fn overlay(ui: &mut egui::Ui, rect: Rect, vp: &Mat4, b: &mut ClipBox, accent: Color32) -> bool {
    let painter = ui.painter_at(rect);
    let stroke = Stroke::new(1.5, accent);
    for (i, j) in [
        (0, 1),
        (2, 3),
        (4, 5),
        (6, 7), // along x
        (0, 2),
        (1, 3),
        (4, 6),
        (5, 7), // along y
        (0, 4),
        (1, 5),
        (2, 6),
        (3, 7), // along z
    ] {
        if let (Some(p), Some(q)) = (
            project(vp, rect, corner(b, i)),
            project(vp, rect, corner(b, j)),
        ) {
            painter.line_segment([p, q], stroke);
        }
    }

    let mut changed = false;
    for k in 0..6 {
        let c = face_center(b, k);
        let Some(p) = project(vp, rect, c) else {
            continue;
        };
        if !rect.contains(p) {
            continue;
        }
        let a = k / 2;
        // Screen direction of the face's outward axis, from a step of a tenth of the box size.
        let len = ((b.max - b.min).max_element() * 0.1).max(1.0);
        let mut dir = Vec3::ZERO;
        dir[a] = if k.is_multiple_of(2) { -len } else { len };
        let Some(q) = project(vp, rect, c + dir) else {
            continue;
        };
        let s = q - p;
        let id = ui.scope_id().with(("section-handle", k));
        let r = ui.interact(
            Rect::from_center_size(p, Vec2::splat(18.0)),
            id,
            Sense::drag(),
        );
        let hot = r.hovered() || r.dragged();
        // An arrow pointing outward from the face.
        let tip = p + s.normalized() * 14.0;
        let fill = if hot { accent } else { Color32::WHITE };
        painter.circle(p, 6.0, fill, stroke);
        if s.length() > 1e-3 {
            painter.arrow(p, tip - p, stroke);
        }
        if hot {
            ui.ctx().set_cursor_icon(egui::CursorIcon::Grab);
        }
        let d = r.drag_delta();
        if d != Vec2::ZERO && s.length_sq() > 1e-6 {
            // Mouse movement along the axis on screen, as a distance along the axis in mm.
            let t = d.dot(s) / s.length_sq() * len;
            if k.is_multiple_of(2) {
                b.min[a] = (b.min[a] - t).min(b.max[a] - MIN_SIZE);
            } else {
                b.max[a] = (b.max[a] + t).max(b.min[a] + MIN_SIZE);
            }
            changed = true;
        }
    }
    changed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn faces_and_corners() {
        let b = ClipBox {
            min: Vec3::new(0.0, 0.0, 0.0),
            max: Vec3::new(10.0, 20.0, 30.0),
            yaw: 0.0,
        };
        assert_eq!(corner(&b, 0), Vec3::ZERO);
        assert_eq!(corner(&b, 7), Vec3::new(10.0, 20.0, 30.0));
        assert_eq!(face_center(&b, 0), Vec3::new(0.0, 10.0, 15.0));
        assert_eq!(face_center(&b, 5), Vec3::new(5.0, 10.0, 30.0));
    }

    #[test]
    fn projection_maps_ndc_to_the_rect() {
        let rect = Rect::from_min_size(Pos2::new(100.0, 50.0), Vec2::new(200.0, 100.0));
        let p = project(&Mat4::IDENTITY, rect, Vec3::new(0.0, 0.0, 0.5)).unwrap();
        assert_eq!(p, Pos2::new(200.0, 100.0));
        let p = project(&Mat4::IDENTITY, rect, Vec3::new(-1.0, 1.0, 0.5)).unwrap();
        assert_eq!(p, Pos2::new(100.0, 50.0));
    }
}
