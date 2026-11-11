//! Views in the viewer (ADR 0021): how a view sets the camera and the clip box, where its
//! reference plans' lines are, what is new by default. Plain functions, tested without a window.

use bimble_core::GlobalId;
use bimble_core::georef::Georef;
use bimble_core::view::{
    Facing, Level, Look, Reference, View, ViewKind, ViewRange, Visibility, resolve,
};
use bimble_geom::Scene;
use bimble_render::ClipBox;
use glam::Vec3;

/// Camera yaw and pitch (radians) and mirroring for a 2D view; None for 3D views.
/// Turned by the view's angle (project north, ADR 0045): the camera turns with the building, so
/// the picture is the one an unturned building gives.
pub fn camera_angles(v: &View, g: &Georef) -> Option<(f32, f32, bool)> {
    let half = std::f32::consts::FRAC_PI_2;
    let turn = v.plan_turn(g) as f32;
    let angles = match &v.kind {
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
    };
    angles.map(|(yaw, pitch, mirror)| (yaw + turn, pitch, mirror))
}

/// Screen-right direction (world x, y) of a section looking along `d` (its facing, turned).
pub fn section_right(d: [f64; 2]) -> [f64; 2] {
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
            // In the section's frame: the scene turned by its angle (project north), where the
            // facing is along an axis again. The box keeps that turn.
            let yaw = v.plan_turn(&scene.georef) as f32;
            if yaw != 0.0 {
                let turned = ClipBox { yaw, ..b };
                let (lo, hi) = (turned.to_frame(b.min), turned.to_frame(b.max));
                let (mut lo2, mut hi2) = (lo.min(hi), lo.max(hi));
                for c in [
                    Vec3::new(b.min.x, b.max.y, 0.0),
                    Vec3::new(b.max.x, b.min.y, 0.0),
                ] {
                    let q = turned.to_frame(c);
                    lo2 = lo2.min(q);
                    hi2 = hi2.max(q);
                }
                b = ClipBox {
                    min: Vec3::new(lo2.x, lo2.y, b.min.z),
                    max: Vec3::new(hi2.x, hi2.y, b.max.z),
                    yaw,
                };
            }
            let d = facing.dir();
            let (k, s) = if d[0] != 0.0 { (0, d[0]) } else { (1, d[1]) };
            let at = b.to_frame(Vec3::new(
                (origin[0] - o[0]) as f32,
                (origin[1] - o[1]) as f32,
                0.0,
            ));
            let cut = at[k];
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
    // The crop region (ADR 0046): in the view's plane, so the box turns with the view.
    if let Some(c) = &v.crop {
        let t = bimble_drawing::viewdraw::Turn::of(v, &scene.georef).apply(o);
        let (lo, hi) = (
            [c.min[0] - t[0], c.min[1] - t[1]],
            [c.max[0] - t[0], c.max[1] - t[1]],
        );
        match &v.kind {
            ViewKind::Plan { .. } => {
                // The box's frame is the turned scene: x, y are the crop's.
                let yaw = v.plan_turn(&scene.georef) as f32;
                let (zmin, zmax) = (b.min.z, b.max.z);
                b = ClipBox {
                    min: Vec3::new(lo[0] as f32, lo[1] as f32, zmin),
                    max: Vec3::new(hi[0] as f32, hi[1] as f32, zmax),
                    yaw,
                };
            }
            ViewKind::Section { facing, .. } => {
                // Along the cut: the frame axis the section's right lies on; up: the height.
                let right = section_right(facing.dir());
                let k = if right[0] != 0.0 { 0 } else { 1 };
                let (a, z) = if right[k] > 0.0 {
                    (lo[0] as f32, hi[0] as f32)
                } else {
                    (-(hi[0] as f32), -(lo[0] as f32))
                };
                b.min[k] = b.min[k].max(a);
                b.max[k] = b.max[k].min(z);
                b.min.z = b.min.z.max(lo[1] as f32);
                b.max.z = b.max.z.min(hi[1] as f32);
            }
            ViewKind::ThreeD => {}
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
            let (origin, right) = (*origin, section_right(v.facing_dir(*facing, &scene.georef)));
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
pub fn reference_move(v: &View, g: &Georef, world: Vec3) -> [f64; 2] {
    match &v.kind {
        ViewKind::Section { facing, .. } => {
            let r = section_right(v.facing_dir(*facing, g));
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
        true_north: false,
        scale: 100,
        crop: None,
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
        true_north: false,
        scale: 100,
        crop: None,
    }
}

/// A reference plan read again (Manage links, #58): the new file's lines, kept where the old
/// one was placed (offset, rotation, visibility, id: views and sheets refer to it).
pub(crate) fn reload_reference(old: &mut Reference, new: Reference) {
    old.name = new.name;
    old.format = new.format;
    old.source = new.source.or(old.source.take());
    old.polylines = new.polylines;
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

    /// A section of a building turned 30° (project north, ADR 0045): its box is turned with
    /// it, keeping what lies ahead of the cut along the building's north, not behind it.
    #[test]
    fn a_turned_section_keeps_what_is_ahead_along_the_building() {
        let mut scene = Scene::from_model(&bimble_core::demo::concrete_frame(
            &bimble_core::demo::FrameSpec::default(),
        ));
        scene.georef.rotation_deg = 30.0;
        let o = scene.origin_mm;
        let sec = View {
            id: GlobalId::new(),
            name: "Section A".into(),
            kind: ViewKind::Section {
                origin: [o[0], o[1]],
                facing: Facing::North,
                depth_mm: 0.0,
            },
            visibility: Default::default(),
            references: Vec::new(),
            annotations: Vec::new(),
            true_north: false,
            scale: 100,
            crop: None,
        };
        let b = view_clip(&sec, &scene, &scene.levels()).unwrap().unwrap();
        assert!((b.yaw - 30f32.to_radians()).abs() < 1e-6);
        let d = sec.facing_dir(Facing::North, &scene.georef);
        let at = |k: f32| Vec3::new(d[0] as f32 * k, d[1] as f32 * k, 1000.0);
        assert!(b.contains(at(1000.0)), "ahead");
        assert!(!b.contains(at(-1000.0)), "behind");
        // Sideways along the cut, on both sides: still inside (the cut is across the facing).
        let side = Vec3::new(d[1] as f32, -d[0] as f32, 0.0) * 3000.0;
        assert!(b.contains(at(10.0) + side) && b.contains(at(10.0) - side));
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
        assert!(
            camera_angles(&up, &Georef::default()).unwrap().2,
            "mirrored"
        );
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
        assert_eq!(section_right(Facing::North.dir()), [1.0, 0.0]);
        assert_eq!(section_right(Facing::East.dir()), [0.0, -1.0]);
    }

    #[test]
    fn a_reloaded_reference_keeps_its_place() {
        let r = |name: &str, x: f64, source: Option<&str>| Reference {
            id: GlobalId::new(),
            name: name.into(),
            format: "dxf".into(),
            source: source.map(Into::into),
            offset_mm: [x, 2.0],
            rotation_deg: x,
            visible: x > 0.0,
            polylines: vec![vec![[x, 0.0], [x, 1.0]]],
        };
        let mut old = r("a.dxf", 5.0, Some("/plans/a.dxf"));
        let (id, kept) = (old.id, old.clone());
        reload_reference(&mut old, r("b.dxf", -1.0, None));
        assert_eq!(old.id, id);
        assert_eq!(
            (old.offset_mm, old.rotation_deg, old.visible),
            (kept.offset_mm, kept.rotation_deg, kept.visible)
        );
        assert_eq!(old.name, "b.dxf");
        assert_eq!(old.polylines, vec![vec![[-1.0, 0.0], [-1.0, 1.0]]]);
        // Read in a browser (no path): where the old one came from is kept.
        assert_eq!(old.source.as_deref(), Some("/plans/a.dxf"));
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
            source: None,
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
            reference_move(&sec, &Georef::default(), Vec3::new(0.0, -10.0, 5.0)),
            [10.0, 5.0]
        );
    }
}
