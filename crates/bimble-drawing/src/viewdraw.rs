//! Any 2D view on paper (issue #32): the scene turned so the view looks straight down, then
//! drawn like a storey plan.
//!
//! - A plan looking down is the scene as it is.
//! - A ceiling plan looking up is reflected (z → −z): seen from below with north up and east to
//!   the right, as on the screen (ADR 0021).
//! - A section looking north, east, south or west is turned so the look direction is down,
//!   with x along the section to the right and y up.
//!
//! The heights the plan drawing works with (level, cut, next) are then those of the turned
//! scene.

use bimble_core::view::{Look, View, ViewKind, Visibility, resolve};
use bimble_geom::{Scene, SceneElement, pack_normal};
use std::borrow::Cow;

/// A view ready for the plan drawing.
pub struct ViewSetup<'a> {
    /// The scene as seen from above (elements the view hides left out).
    pub scene: Cow<'a, Scene>,
    /// Lowest height seen, the cut plane, and the highest height drawn dashed above the cut
    /// (the storey's elements over a plan), in the turned scene's coordinates.
    pub level: f64,
    pub cut: f64,
    pub next: f64,
    /// The storey whose elements are drawn dashed above the cut (plans looking down).
    pub storey: Option<String>,
    /// For the texts: storey name and level (world mm).
    pub storey_name: String,
    pub storey_elevation_mm: f64,
    /// Plans looking down: slab levels.
    pub plan_down: bool,
    /// Plans (down or up): grid axes, from the model or the columns.
    pub axes: bool,
}

/// Turn `scene` for `view`. 3D views have no paper drawing.
pub fn setup<'a>(scene: &'a Scene, view: &View) -> Result<ViewSetup<'a>, String> {
    let levels = scene.levels();
    let oz = scene.origin_mm[2];
    let storey_of = |key: &str| {
        scene
            .storeys
            .iter()
            .find(|s| s.id == key || (s.id.is_empty() && s.name == key))
            .cloned()
            .ok_or_else(|| format!("view {}: no storey {key} in this model", view.name))
    };
    let visible = |e: &SceneElement| keep(&view.visibility, e);
    match &view.kind {
        ViewKind::ThreeD => Err(format!(
            "view {}: a 3D view has no paper drawing",
            view.name
        )),
        ViewKind::Plan {
            storey,
            look,
            range,
        } => {
            let st = storey_of(storey)?;
            let r = resolve(range, *look, storey, &levels)?;
            match look {
                Look::Down => Ok(ViewSetup {
                    scene: if view.visibility.is_empty() {
                        Cow::Borrowed(scene)
                    } else {
                        Cow::Owned(map_scene(scene, &visible, |p| p, false, true))
                    },
                    level: r.depth - oz,
                    cut: r.cut - oz,
                    next: r.top - oz,
                    storey: Some(st.name.clone()),
                    storey_name: st.name,
                    storey_elevation_mm: st.elevation_mm,
                    plan_down: true,
                    axes: true,
                }),
                Look::Up => Ok(ViewSetup {
                    // Reflected: what is higher is nearer the viewer below.
                    scene: Cow::Owned(map_scene(
                        scene,
                        &visible,
                        |p| [p[0], p[1], -p[2]],
                        true,
                        true,
                    )),
                    level: -(r.depth - oz),
                    cut: -(r.cut - oz),
                    next: -(r.cut - oz),
                    storey: None,
                    storey_name: st.name,
                    storey_elevation_mm: st.elevation_mm,
                    plan_down: false,
                    axes: true,
                }),
            }
        }
        ViewKind::Section {
            origin,
            facing,
            depth_mm,
        } => {
            let d = facing.dir();
            let (dx, dy) = (d[0] as f32, d[1] as f32);
            // Right along the section, up, and towards the viewer: a rotation.
            let turned = map_scene(
                scene,
                &visible,
                |p| [dy * p[0] - dx * p[1], p[2], -(dx * p[0] + dy * p[1])],
                false,
                false,
            );
            let o = [
                origin[0] - scene.origin_mm[0],
                origin[1] - scene.origin_mm[1],
            ];
            let cut = -(d[0] * o[0] + d[1] * o[1]);
            let level = if *depth_mm > 0.0 {
                cut - depth_mm
            } else {
                f64::NEG_INFINITY
            };
            let (name, elevation) = scene
                .storeys
                .first()
                .map_or((String::new(), 0.0), |s| (s.name.clone(), s.elevation_mm));
            Ok(ViewSetup {
                scene: Cow::Owned(turned),
                level,
                cut,
                next: cut,
                storey: None,
                storey_name: name,
                storey_elevation_mm: elevation,
                plan_down: false,
                axes: false,
            })
        }
    }
}

/// Whether a view shows an element (its visibility lists).
fn keep(v: &Visibility, e: &SceneElement) -> bool {
    !v.hidden_families.iter().any(|f| f == e.category.label())
        && !v.hidden_classes.iter().any(|c| c == &e.class)
        && !v.hidden_storeys.iter().any(|s| s == &e.storey)
}

/// The scene with every position (and normal) mapped by the linear map `f`; `reflect` when `f`
/// is a reflection, so triangles keep facing outwards.
fn map_scene(
    scene: &Scene,
    visible: &dyn Fn(&SceneElement) -> bool,
    f: impl Fn([f32; 3]) -> [f32; 3],
    reflect: bool,
    grids: bool,
) -> Scene {
    let unpack = |n: [i8; 4]| {
        [
            n[0] as f32 / 127.0,
            n[1] as f32 / 127.0,
            n[2] as f32 / 127.0,
        ]
    };
    let elements = scene
        .elements
        .iter()
        .filter(|e| visible(e))
        .map(|e| {
            let mut e = e.clone();
            let m = &mut e.mesh;
            for p in &mut m.positions {
                *p = f(*p);
            }
            for n in &mut m.normals {
                *n = f(*n);
            }
            if reflect {
                for t in m.indices.as_chunks_mut::<3>().0 {
                    t.swap(1, 2);
                }
            }
            for s in &mut m.edges {
                *s = [f(s[0]), f(s[1])];
            }
            for n in &mut m.edge_normals {
                for face in n.iter_mut() {
                    // Unknown (zero) stays unknown.
                    if face[..3] != [0, 0, 0] {
                        *face = pack_normal(f(unpack(*face)));
                    }
                }
            }
            e
        })
        .collect();
    Scene {
        origin_mm: scene.origin_mm,
        elements,
        storeys: scene.storeys.clone(),
        // Grid axes are plan lines: kept for plans (a ceiling plan keeps x and y).
        grids: if grids {
            scene.grids.clone()
        } else {
            Vec::new()
        },
        warnings: Vec::new(),
    }
}
