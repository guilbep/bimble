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
    /// How a point of the scene is turned for this view.
    pub turn: Turn,
}

/// How the scene is turned so the view looks straight down.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Turn {
    /// A plan looking down: as it is.
    None,
    /// A ceiling plan: reflected, z → −z.
    Reflect,
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
    pub fn of(v: &View) -> Turn {
        match &v.kind {
            ViewKind::Plan { look: Look::Up, .. } => Turn::Reflect,
            ViewKind::Section { facing, .. } => Turn::Section(facing.dir()),
            _ => Turn::None,
        }
    }

    /// The scene point (relative to its origin) that `apply` turns into `q`.
    pub fn invert(self, q: [f64; 3]) -> [f64; 3] {
        match self {
            Turn::None => q,
            Turn::Reflect => [q[0], q[1], -q[2]],
            Turn::Section([dx, dy]) => [q[0] * dy - q[2] * dx, -q[0] * dx - q[2] * dy, q[1]],
        }
    }

    /// A point of the scene (mm, relative to its origin) in the turned scene.
    pub fn apply(self, p: [f64; 3]) -> [f64; 3] {
        match self {
            Turn::None => p,
            Turn::Reflect => [p[0], p[1], -p[2]],
            Turn::Section([dx, dy]) => [dy * p[0] - dx * p[1], p[2], -(dx * p[0] + dy * p[1])],
        }
    }
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
                    turn: Turn::None,
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
                    turn: Turn::Reflect,
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
                turn: Turn::Section(d),
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
        densities: scene.densities.clone(),
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
    fn turns_invert() {
        for t in [
            Turn::None,
            Turn::Reflect,
            Turn::Section([0.0, 1.0]),
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
