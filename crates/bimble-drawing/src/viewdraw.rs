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
use std::collections::HashSet;

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
    /// How a point of the scene is turned for this view.
    pub turn: Turn,
}

/// How the scene is turned so the view looks straight down.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Turn {
    /// A plan looking down, turned by −a (radians) so its north is up: project north turns by
    /// the building's angle (ADR 0045); 0 leaves the scene as it is.
    Plan(f64),
    /// A ceiling plan: turned the same, then reflected, z → −z.
    Reflect(f64),
    /// A section looking along `[dx, dy]`.
    Section([f64; 2]),
}

/// Where a view's drawing is on paper: from world mm to paper mm and back (#34).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewFrame {
    pub turn: Turn,
    /// The scene's origin, world mm.
    pub origin: [f64; 3],
    /// Paper = (ox + (x − x0)·k, oy + (y1 − y)·k), x, y in the turned scene.
    pub x0: f64,
    pub y1: f64,
    pub k: f64,
    pub ox: f64,
    pub oy: f64,
    /// Height of the cut plane in the turned scene: points picked on paper lie on it.
    pub cut: f64,
}

impl ViewFrame {
    pub fn to_paper(&self, world: [f64; 3]) -> [f64; 2] {
        let o = self.origin;
        let q = self
            .turn
            .apply([world[0] - o[0], world[1] - o[1], world[2] - o[2]]);
        [
            self.ox + (q[0] - self.x0) * self.k,
            self.oy + (self.y1 - q[1]) * self.k,
        ]
    }

    /// The world point (on the cut plane) drawn at `paper`.
    pub fn to_world(&self, paper: [f64; 2]) -> [f64; 3] {
        let q = [
            self.x0 + (paper[0] - self.ox) / self.k,
            self.y1 - (paper[1] - self.oy) / self.k,
            self.cut,
        ];
        let p = self.turn.invert(q);
        let o = self.origin;
        [p[0] + o[0], p[1] + o[1], p[2] + o[2]]
    }

    /// The same frame moved by `d` on paper.
    pub fn shifted(mut self, d: [f64; 2]) -> ViewFrame {
        self.ox += d[0];
        self.oy += d[1];
        self
    }
}

impl Turn {
    /// How a view turns the scene (3D views: not at all).
    pub fn of(v: &View, g: &bimble_core::georef::Georef) -> Turn {
        let a = v.plan_turn(g);
        match &v.kind {
            ViewKind::Plan { look: Look::Up, .. } => Turn::Reflect(a),
            ViewKind::Section { facing, .. } => Turn::Section(facing.dir_turned(a)),
            _ => Turn::Plan(a),
        }
    }

    /// The scene point (relative to its origin) that `apply` turns into `q`.
    pub fn invert(self, q: [f64; 3]) -> [f64; 3] {
        match self {
            Turn::Plan(a) => rotate(q, a),
            Turn::Reflect(a) => rotate([q[0], q[1], -q[2]], a),
            Turn::Section([dx, dy]) => [q[0] * dy - q[2] * dx, -q[0] * dx - q[2] * dy, q[1]],
        }
    }

    /// A point of the scene (mm, relative to its origin) in the turned scene.
    pub fn apply(self, p: [f64; 3]) -> [f64; 3] {
        match self {
            Turn::Plan(a) => rotate(p, -a),
            Turn::Reflect(a) => {
                let q = rotate(p, -a);
                [q[0], q[1], -q[2]]
            }
            Turn::Section([dx, dy]) => [dy * p[0] - dx * p[1], p[2], -(dx * p[0] + dy * p[1])],
        }
    }
}

/// `p` turned by `a` radians counter-clockwise about the vertical (0: unchanged, exactly).
fn rotate(p: [f64; 3], a: f64) -> [f64; 3] {
    if a == 0.0 {
        return p;
    }
    let (s, c) = a.sin_cos();
    [c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]]
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
    let shown = Shown::of(&view.visibility);
    // What the drawing can show: an element wholly outside the view's heights is left out before
    // the scene is copied and turned (ADR 0046); its drawing doesn't change.
    let range = match &view.kind {
        ViewKind::Plan {
            storey,
            look,
            range,
        } => resolve(range, *look, storey, &levels)
            .ok()
            .map(|r| match look {
                Look::Down => (r.depth.min(r.cut), r.top.max(r.cut)),
                Look::Up => {
                    let (lo, hi) = r.visible(*look);
                    (lo.min(hi), hi.max(lo))
                }
            }),
        _ => None,
    };
    let visible = |e: &SceneElement| {
        shown.keeps(e) && range.is_none_or(|(lo, hi)| within_heights(e, oz, lo, hi))
    };
    let turn = Turn::of(view, &scene.georef);
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
                    scene: if view.visibility.is_empty() && turn == Turn::Plan(0.0) {
                        Cow::Borrowed(scene)
                    } else {
                        Cow::Owned(map_scene(scene, &visible, turn, false, true))
                    },
                    level: r.depth - oz,
                    cut: r.cut - oz,
                    next: r.top - oz,
                    storey: Some(st.name.clone()),
                    storey_name: st.name,
                    storey_elevation_mm: st.elevation_mm,
                    plan_down: true,
                    axes: true,
                    turn,
                }),
                Look::Up => Ok(ViewSetup {
                    // Reflected: what is higher is nearer the viewer below.
                    scene: Cow::Owned(map_scene(scene, &visible, turn, true, true)),
                    level: -(r.depth - oz),
                    cut: -(r.cut - oz),
                    next: -(r.cut - oz),
                    storey: None,
                    storey_name: st.name,
                    storey_elevation_mm: st.elevation_mm,
                    plan_down: false,
                    axes: true,
                    turn,
                }),
            }
        }
        ViewKind::Section {
            origin,
            facing,
            depth_mm,
        } => {
            // Its facing in project north (or true north): along the building's grid.
            let d = view.facing_dir(*facing, &scene.georef);
            // Right along the section, up, and towards the viewer: a rotation.
            let turned = map_scene(scene, &visible, turn, false, false);
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
                turn,
            })
        }
    }
}

/// Whether any of an element lies between the heights `lo` and `hi` (world mm; infinite for an
/// unlimited side), with a millimetre to spare.
pub(crate) fn within_heights(e: &SceneElement, oz: f64, lo: f64, hi: f64) -> bool {
    let b = e.bounds();
    if b.is_empty() {
        return true;
    }
    let (zl, zh) = (f64::from(b.min[2]) + oz, f64::from(b.max[2]) + oz);
    zh >= lo - 1.0 && zl <= hi + 1.0
}

/// What a view shows (its visibility lists), as sets: asked once per element.
struct Shown<'a> {
    families: HashSet<&'a str>,
    classes: HashSet<&'a str>,
    storeys: HashSet<&'a str>,
    materials: HashSet<&'a str>,
    elements: HashSet<&'a str>,
    isolated: HashSet<&'a str>,
}

impl<'a> Shown<'a> {
    fn of(v: &'a Visibility) -> Self {
        let set = |l: &'a [String]| l.iter().map(String::as_str).collect();
        Shown {
            families: set(&v.hidden_families),
            classes: set(&v.hidden_classes),
            storeys: set(&v.hidden_storeys),
            materials: set(&v.hidden_materials),
            elements: set(&v.hidden_elements),
            isolated: set(&v.isolated_elements),
        }
    }

    fn keeps(&self, e: &SceneElement) -> bool {
        !self.families.contains(e.category.label())
            && !self.classes.contains(e.class.as_str())
            && !self.storeys.contains(e.storey.as_str())
            && !self.materials.contains(e.material.as_str())
            && !self.elements.contains(e.global_id.as_str())
            && (self.isolated.is_empty() || self.isolated.contains(e.global_id.as_str()))
    }
}

/// The scene with every position (and normal) mapped by the linear map `f`; `reflect` when `f`
/// is a reflection, so triangles keep facing outwards.
pub(crate) fn map_scene(
    scene: &Scene,
    visible: &dyn Fn(&SceneElement) -> bool,
    turn: Turn,
    reflect: bool,
    grids: bool,
) -> Scene {
    // Linear (no translation): points relative to the origin and normals alike.
    let f = |p: [f32; 3]| turn.apply(p.map(f64::from)).map(|c| c as f32);
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
            // The view's transform applies to world triangles: an instance becomes its own.
            let mut mesh = e.world_mesh().into_owned();
            let m = &mut mesh;
            for p in &mut m.positions {
                *p = f(*p);
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
            e.geometry = bimble_geom::scene::Geometry::Own(mesh);
            e
        })
        .collect();
    Scene {
        densities: scene.densities.clone(),
        georef: scene.georef.clone(),
        schema: scene.schema.clone(),
        origin_mm: scene.origin_mm,
        elements,
        storeys: scene.storeys.clone(),
        // Grid axes are plan lines: kept for plans, turned with them (a ceiling plan keeps x
        // and y).
        grids: if grids {
            let o = scene.origin_mm;
            let at = |p: [f64; 2]| {
                let q = turn.apply([p[0] - o[0], p[1] - o[1], 0.0]);
                [q[0] + o[0], q[1] + o[1]]
            };
            scene
                .grids
                .iter()
                .map(|g| bimble_geom::GridAxis {
                    tag: g.tag.clone(),
                    start_mm: at(g.start_mm),
                    end_mm: at(g.end_mm),
                })
                .collect()
        } else {
            Vec::new()
        },
        warnings: Vec::new(),
    }
}

/// Direction (unit), the dimension line's two ends, and the length measured, paper mm.
pub type DimOnPaper = ([f64; 2], [f64; 2], [f64; 2], f64);

/// A dimension on paper: from points `pa`, `pb` (paper mm), its line `off` mm to the left of
/// its direction (aligned: a→b; horizontal; vertical). Returns the direction (unit), the line's
/// ends and the length measured (paper mm); None when there is nothing to measure.
pub fn dimension_on_paper(
    pa: [f64; 2],
    pb: [f64; 2],
    off: f64,
    axis: bimble_core::view::DimAxis,
) -> Option<DimOnPaper> {
    use bimble_core::view::DimAxis;
    let d0 = [pb[0] - pa[0], pb[1] - pa[1]];
    if d0[0].hypot(d0[1]) < 1e-6 {
        return None;
    }
    let d = match axis {
        DimAxis::Aligned => d0,
        DimAxis::Horizontal => [if d0[0] < 0.0 { -1.0 } else { 1.0 }, 0.0],
        DimAxis::Vertical => [0.0, if d0[1] < 0.0 { -1.0 } else { 1.0 }],
    };
    let dl = d[0].hypot(d[1]);
    let u = [d[0] / dl, d[1] / dl];
    let n = [u[1], -u[0]];
    let qa = [pa[0] + n[0] * off, pa[1] + n[1] * off];
    let along = (pb[0] - qa[0]) * u[0] + (pb[1] - qa[1]) * u[1];
    let qb = [qa[0] + u[0] * along, qa[1] + u[1] * along];
    (along.abs() > 1e-6).then_some((u, qa, qb, along.abs()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_view_leaves_out_what_it_hides() {
        let e = |id: &str, material: &str| SceneElement {
            global_id: id.into(),
            class: "IfcWall".into(),
            name: String::new(),
            storey: "L1".into(),
            material: material.into(),
            category: bimble_core::MaterialCategory::Other,
            geometry: bimble_geom::scene::Geometry::Own(Default::default()),
        };
        let (a, b) = (e("a", "Plaster"), e("b", "Aluminium"));
        let shows = |v: &Visibility| {
            let s = Shown::of(v);
            [s.keeps(&a), s.keeps(&b)]
        };
        assert_eq!(shows(&Visibility::default()), [true, true]);
        let v = |f: fn(&mut Visibility)| {
            let mut v = Visibility::default();
            f(&mut v);
            v
        };
        assert_eq!(
            shows(&v(|v| v.hidden_materials = vec!["Aluminium".into()])),
            [true, false]
        );
        assert_eq!(
            shows(&v(|v| v.hidden_elements = vec!["a".into()])),
            [false, true]
        );
        assert_eq!(
            shows(&v(|v| v.isolated_elements = vec!["a".into()])),
            [true, false]
        );
        assert_eq!(
            shows(&v(|v| v.hidden_families = vec!["Other".into()])),
            [false, false]
        );
    }

    /// A building turned 30° in its site (ADR 0045): its plan in project north is the unturned
    /// building, element by element and grid axis by grid axis; in true north it stays turned.
    #[test]
    fn a_turned_building_is_drawn_along_its_grid() {
        use bimble_core::demo::{FrameSpec, concrete_frame};
        use bimble_core::view::default_views;
        let mut plain = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let o = plain.origin_mm;
        plain.grids = vec![bimble_geom::GridAxis {
            tag: "A".into(),
            start_mm: [o[0] - 5000.0, o[1]],
            end_mm: [o[0] + 5000.0, o[1]],
        }];
        let a = 30f64.to_radians();
        // The same building turned by 30° (Turn::Plan turns by the opposite of its angle).
        let mut turned = map_scene(&plain, &|_| true, Turn::Plan(-a), false, true);
        turned.georef.rotation_deg = 30.0;
        let plan = default_views(&plain.levels())
            .into_iter()
            .find(|v| {
                matches!(
                    v.kind,
                    ViewKind::Plan {
                        look: Look::Down,
                        ..
                    }
                )
            })
            .unwrap();
        // By GlobalId: a plan keeps only what lies within its range.
        let boxes = |s: &Scene| -> std::collections::BTreeMap<String, [f32; 4]> {
            s.elements
                .iter()
                .map(|e| {
                    let m = e.world_mesh();
                    let (mut lo, mut hi) = ([f32::MAX; 2], [f32::MIN; 2]);
                    for p in &m.positions {
                        for k in 0..2 {
                            lo[k] = lo[k].min(p[k]);
                            hi[k] = hi[k].max(p[k]);
                        }
                    }
                    (e.global_id.clone(), [lo[0], lo[1], hi[0], hi[1]])
                })
                .collect()
        };
        let want = boxes(&plain);
        type Boxes = std::collections::BTreeMap<String, [f32; 4]>;
        let near = |a: &Boxes, b: &Boxes| {
            !a.is_empty()
                && a.iter().all(|(id, x)| {
                    b.get(id)
                        .is_some_and(|y| (0..4).all(|k| (x[k] - y[k]).abs() < 0.05))
                })
        };
        let up = setup(&turned, &plan).unwrap();
        assert_eq!(up.turn, Turn::Plan(a));
        assert!(
            near(&boxes(&up.scene), &want),
            "project north: along the grid"
        );
        let g = &up.scene.grids[0];
        assert!((g.start_mm[1] - o[1]).abs() < 1e-6 && (g.end_mm[1] - o[1]).abs() < 1e-6);
        let mut true_north = plan.clone();
        true_north.true_north = true;
        let tn = setup(&turned, &true_north).unwrap();
        assert!(!near(&boxes(&tn.scene), &want), "true north: still turned");
        // A section looking "north" looks along the building's north.
        let sec = View {
            kind: ViewKind::Section {
                origin: [o[0], o[1]],
                facing: bimble_core::view::Facing::North,
                depth_mm: 0.0,
            },
            ..plan.clone()
        };
        let Turn::Section(d) = setup(&turned, &sec).unwrap().turn else {
            panic!("a section");
        };
        assert!(
            (d[0] + a.sin()).abs() < 1e-12 && (d[1] - a.cos()).abs() < 1e-12,
            "{d:?}"
        );
    }

    /// A turned plan of the lowest storey copies only what lies within its range: the upper
    /// storey's beams and slab, wholly above, are left out (ADR 0046); before, every element was
    /// copied and turned.
    #[test]
    fn a_plan_leaves_out_what_is_above_its_range() {
        use bimble_core::demo::{FrameSpec, concrete_frame};
        use bimble_core::view::default_views;
        let mut scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        scene.georef.rotation_deg = 30.0;
        let levels = scene.levels();
        let low = levels
            .iter()
            .min_by(|a, b| a.elevation_mm.total_cmp(&b.elevation_mm))
            .unwrap();
        let plan = default_views(&levels)
            .into_iter()
            .find(|v| matches!(&v.kind, ViewKind::Plan { storey, look: Look::Down, .. } if *storey == low.key))
            .unwrap();
        let ViewKind::Plan {
            storey,
            look,
            range,
        } = &plan.kind
        else {
            unreachable!()
        };
        let top = resolve(range, *look, storey, &levels).unwrap().top;
        let up = setup(&scene, &plan).unwrap();
        assert!(up.scene.elements.len() < scene.elements.len());
        let oz = scene.origin_mm[2];
        let above: Vec<&str> = scene
            .elements
            .iter()
            .filter(|e| f64::from(e.bounds().min[2]) + oz > top + 1.0)
            .map(|e| e.global_id.as_str())
            .collect();
        assert!(
            !above.is_empty(),
            "the frame has elements above the lowest plan"
        );
        assert!(
            up.scene
                .elements
                .iter()
                .all(|e| !above.contains(&e.global_id.as_str()))
        );
    }

    #[test]
    fn turns_invert() {
        for t in [
            Turn::Plan(0.0),
            Turn::Reflect(0.0),
            Turn::Plan(0.6),
            Turn::Reflect(-1.2),
            Turn::Section([0.0, 1.0]),
            Turn::Section([0.866025403784, 0.5]),
            Turn::Section([1.0, 0.0]),
            Turn::Section([0.0, -1.0]),
            Turn::Section([-1.0, 0.0]),
        ] {
            let p = [1234.0, -567.0, 89.0];
            let q = t.invert(t.apply(p));
            assert!((0..3).all(|k| (q[k] - p[k]).abs() < 1e-9), "{t:?}: {q:?}");
        }
        let f = ViewFrame {
            turn: Turn::Section([1.0, 0.0]),
            origin: [100.0, 200.0, 300.0],
            x0: -5000.0,
            y1: 4000.0,
            k: 0.01,
            ox: 20.0,
            oy: 30.0,
            cut: -50.0,
        };
        let w = f.to_world([55.0, 66.0]);
        let back = f.to_paper(w);
        assert!((back[0] - 55.0).abs() < 1e-9 && (back[1] - 66.0).abs() < 1e-9);
    }
}
