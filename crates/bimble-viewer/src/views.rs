//! Views in the viewer (ADR 0021): how a view sets the camera and the clip box, where its
//! reference plans' lines are, what is new by default. Plain functions, tested without a window.

use bimble_core::GlobalId;
use bimble_core::view::{
    Facing, Level, Look, Reference, View, ViewKind, ViewRange, Visibility, resolve,
};
use bimble_geom::Scene;
use bimble_render::ClipBox;
use glam::Vec3;

/// Camera yaw and pitch (radians) and mirroring for a 2D view; None for 3D views.
pub fn camera_angles(v: &View) -> Option<(f32, f32, bool)> {
    let half = std::f32::consts::FRAC_PI_2;
    match &v.kind {
        ViewKind::ThreeD => None,
        // From above, north up. Looking up: from below, mirrored so north stays up and east
        // right (the reflected ceiling plan).
        ViewKind::Plan { look, .. } => Some(match look {
            Look::Down => (-half, half, false),
            Look::Up => (-half, -half, true),
        }),
        // The eye on the other side of the cut, looking along `facing`.
        ViewKind::Section { facing, .. } => Some((
            match facing {
                Facing::North => -half,
                Facing::South => half,
                Facing::East => std::f32::consts::PI,
                Facing::West => 0.0,
            },
            0.0,
            false,
        )),
    }
}

/// Screen-right direction (world x, y) of a section looking along `facing`.
pub fn section_right(facing: Facing) -> [f64; 2] {
    let d = facing.dir();
    [d[1], -d[0]]
}

/// The box a 2D view shows (scene coordinates), from its range or section; None for 3D views.
/// An unlimited side stops at the model's bounds.
pub fn view_clip(v: &View, scene: &Scene, levels: &[Level]) -> Result<Option<ClipBox>, String> {
    let Some(all) = ClipBox::around(scene, |_| true) else {
        return Ok(None);
    };
    let o = scene.origin_mm;
    let mut b = all;
    match &v.kind {
        ViewKind::ThreeD => return Ok(None),
        ViewKind::Plan {
            storey,
            look,
            range,
        } => {
            let r = resolve(range, *look, storey, levels)?;
            let (lo, hi) = r.visible(*look);
            if lo.is_finite() {
                b.min.z = b.min.z.max((lo - o[2]) as f32);
            }
            if hi.is_finite() {
                b.max.z = b.max.z.min((hi - o[2]) as f32);
            }
        }
        ViewKind::Section {
            origin,
            facing,
            depth_mm,
        } => {
            // Keep what lies beyond the cut, along the facing direction, within the depth.
            let d = facing.dir();
            let (k, s) = if d[0] != 0.0 { (0, d[0]) } else { (1, d[1]) };
            let cut = (origin[k] - o[k]) as f32;
            let far = if *depth_mm > 0.0 {
                cut + s as f32 * *depth_mm as f32
            } else if s > 0.0 {
                f32::INFINITY
            } else {
                f32::NEG_INFINITY
            };
            let (lo, hi) = if s > 0.0 { (cut, far) } else { (far, cut) };
            b.min[k] = b.min[k].max(lo);
            b.max[k] = b.max[k].min(hi);
        }
    }
    Ok(Some(b))
}

/// A reference's segments in scene coordinates, on the view's plane: a plan's cut plane, a
/// section's cut (x along the screen, y up). Empty for 3D views.
pub fn reference_segments(
    v: &View,
    r: &Reference,
    scene: &Scene,
    levels: &[Level],
) -> Vec<[Vec3; 2]> {
    let o = scene.origin_mm;
    let to_scene: Box<dyn Fn([f64; 2]) -> Vec3> = match &v.kind {
        ViewKind::ThreeD => return Vec::new(),
        ViewKind::Plan {
            storey,
            look,
            range,
        } => {
            let z = resolve(range, *look, storey, levels)
                .map(|r| r.cut)
                .unwrap_or(0.0);
            Box::new(move |p| {
                Vec3::new(
                    (p[0] - o[0]) as f32,
                    (p[1] - o[1]) as f32,
                    (z - o[2]) as f32,
                )
            })
        }
        ViewKind::Section { origin, facing, .. } => {
            let (origin, right) = (*origin, section_right(*facing));
            Box::new(move |p| {
                Vec3::new(
                    (origin[0] + p[0] * right[0] - o[0]) as f32,
                    (origin[1] + p[0] * right[1] - o[1]) as f32,
                    (p[1] - o[2]) as f32,
                )
            })
        }
    };
    let mut out = Vec::new();
    for line in &r.polylines {
        for w in line.windows(2) {
            out.push([to_scene(r.place(w[0])), to_scene(r.place(w[1]))]);
        }
    }
    out
}

/// A drag on screen, in world mm, turned into a move of a reference in its own plane.
pub fn reference_move(v: &View, world: Vec3) -> [f64; 2] {
    match &v.kind {
        ViewKind::Section { facing, .. } => {
            let r = section_right(*facing);
            [
                world.x as f64 * r[0] + world.y as f64 * r[1],
                world.z as f64,
            ]
        }
        _ => [world.x as f64, world.y as f64],
    }
}

/// A new plan of a storey.
pub fn new_plan(level: &Level, look: Look) -> View {
    View {
        id: GlobalId::new(),
        name: match look {
            Look::Down => format!("Plan {} (copy)", level.name),
            Look::Up => format!("Ceiling {} (copy)", level.name),
        },
        kind: ViewKind::Plan {
            storey: level.key.clone(),
            look,
            range: ViewRange::default_for(look),
        },
        visibility: Visibility::default(),
        references: Vec::new(),
        annotations: Vec::new(),
    }
}

/// A new section through the middle of the model, looking along `facing`.
pub fn new_section(scene: &Scene, facing: Facing, n: usize) -> View {
    let o = scene.origin_mm;
    let c = ClipBox::around(scene, |_| true)
        .map(|b| (b.min + b.max) / 2.0)
        .unwrap_or(Vec3::ZERO);
    View {
        id: GlobalId::new(),
        name: format!("Section {n} (looking {})", facing.label()),
        kind: ViewKind::Section {
            origin: [(c.x as f64 + o[0]).round(), (c.y as f64 + o[1]).round()],
            facing,
            depth_mm: 0.0,
        },
        visibility: Visibility::default(),
        references: Vec::new(),
        annotations: Vec::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use bimble_core::view::default_views;

    fn demo() -> (Scene, Vec<Level>) {
        let m = concrete_frame(&FrameSpec::default());
        let s = Scene::from_model(&m);
        let l = s.levels();
        (s, l)
    }

    #[test]
    fn plan_clip_follows_the_view_range() {
        let (s, l) = demo();
        let plan = default_views(&l)
            .into_iter()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        let b = view_clip(&plan, &s, &l).unwrap().unwrap();
        let oz = s.origin_mm[2] as f32;
        // Niveau 1 at 0: from the level up to the cut 1.2 m above it.
        assert!((b.min.z + oz).abs() < 1e-3, "{}", b.min.z + oz);
        assert!((b.max.z + oz - 1200.0).abs() < 1e-3, "{}", b.max.z + oz);
        // Looking up: from the cut (2.3 m) up to the level above (3 m).
        let up = default_views(&l)
            .into_iter()
            .find(|v| v.name == "Ceiling Niveau 1")
            .unwrap();
        let b = view_clip(&up, &s, &l).unwrap().unwrap();
        assert!((b.min.z + oz - 2300.0).abs() < 1e-3, "{}", b.min.z + oz);
        assert!((b.max.z + oz - 3000.0).abs() < 1e-3);
        assert!(camera_angles(&up).unwrap().2, "mirrored");
    }

    #[test]
    fn section_clip_keeps_what_is_beyond_the_cut() {
        let (s, _) = demo();
        let mut v = new_section(&s, Facing::North, 1);
        let o = s.origin_mm;
        let ViewKind::Section { origin, .. } = v.kind else {
            unreachable!()
        };
        let b = view_clip(&v, &s, &[]).unwrap().unwrap();
        assert!((b.min.y as f64 + o[1] - origin[1]).abs() < 1e-3);
        if let ViewKind::Section {
            facing, depth_mm, ..
        } = &mut v.kind
        {
            *facing = Facing::West;
            *depth_mm = 2000.0;
        }
        let b = view_clip(&v, &s, &[]).unwrap().unwrap();
        assert!((b.max.x as f64 + o[0] - origin[0]).abs() < 1e-3);
        assert!((b.min.x as f64 + o[0] - (origin[0] - 2000.0)).abs() < 1e-3);
        // Screen right looking north is east; looking east it is south.
        assert_eq!(section_right(Facing::North), [1.0, 0.0]);
        assert_eq!(section_right(Facing::East), [0.0, -1.0]);
    }

    #[test]
    fn reference_lines_lie_on_the_view_plane() {
        let (s, l) = demo();
        let mut plan = default_views(&l)
            .into_iter()
            .find(|v| v.name == "Plan Niveau 2")
            .unwrap();
        let r = Reference {
            id: GlobalId::new(),
            name: "a.dxf".into(),
            format: "dxf".into(),
            offset_mm: [1000.0, 500.0],
            rotation_deg: 0.0,
            visible: true,
            polylines: vec![vec![[0.0, 0.0], [100.0, 0.0], [100.0, 100.0]]],
        };
        plan.references.push(r.clone());
        let segs = reference_segments(&plan, &r, &s, &l);
        assert_eq!(segs.len(), 2);
        let o = s.origin_mm;
        // On the cut plane of Niveau 2 (3 m + 1.2 m), moved by the offset.
        assert!((segs[0][0].z as f64 + o[2] - 4200.0).abs() < 1e-3);
        assert!((segs[0][0].x as f64 + o[0] - 1000.0).abs() < 1e-3);
        assert!((segs[1][1].y as f64 + o[1] - 600.0).abs() < 1e-3);
        // A drag in a section moves along the screen and up.
        let sec = new_section(&s, Facing::East, 1);
        assert_eq!(
            reference_move(&sec, Vec3::new(0.0, -10.0, 5.0)),
            [10.0, 5.0]
        );
    }
}
