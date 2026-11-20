//! Verbs that change the model, shared by the viewer, the web UI and MCP agents (ADR 0048).
//! Each returns `Command`s; the caller applies them through `History` (undo, validation).

use bimble_core::{
    Command, Element, Generator, GlobalId, IfcClass, Material, MaterialCategory, Model, Placement,
};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, schemars::JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    /// One click: its centre.
    Column,
    /// Two clicks: start and end of its axis.
    Beam,
    Wall,
    /// Two clicks: opposite corners of a rectangle.
    Slab,
}

impl Kind {
    pub const ALL: [Kind; 4] = [Kind::Column, Kind::Beam, Kind::Wall, Kind::Slab];

    pub fn label(self) -> &'static str {
        match self {
            Kind::Column => "Column",
            Kind::Beam => "Beam",
            Kind::Wall => "Wall",
            Kind::Slab => "Slab",
        }
    }

    pub fn clicks(self) -> usize {
        if self == Kind::Column { 1 } else { 2 }
    }

    fn class(self) -> IfcClass {
        match self {
            Kind::Column => IfcClass::IfcColumn,
            Kind::Beam => IfcClass::IfcBeam,
            Kind::Wall => IfcClass::IfcWall,
            Kind::Slab => IfcClass::IfcSlab,
        }
    }

    /// Name prefix of new elements (as in the demo frame: P columns, B beams, M walls, D slabs).
    fn prefix(self) -> &'static str {
        match self {
            Kind::Column => "P",
            Kind::Beam => "B",
            Kind::Wall => "M",
            Kind::Slab => "D",
        }
    }
}

/// Sizes of new elements, mm.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, schemars::JsonSchema)]
#[serde(default)]
pub struct Sizes {
    pub column: f64,
    pub beam_width: f64,
    pub beam_height: f64,
    pub wall: f64,
    pub slab: f64,
}

impl Default for Sizes {
    fn default() -> Self {
        Sizes {
            column: 300.0,
            beam_width: 250.0,
            beam_height: 500.0,
            wall: 200.0,
            slab: 250.0,
        }
    }
}

/// Height of a storey: up to the next one above it, else 3 m.
/// A new, empty model (File → New): a fresh project GlobalId and one storey, "Level 0" at 0 mm,
/// so placing works at once.
pub fn new_model(name: &str) -> Model {
    let mut m = Model::new(name);
    let id = GlobalId::new();
    m.storeys.insert(
        id,
        bimble_core::Storey {
            id,
            name: "Level 0".into(),
            elevation: 0.0,
        },
    );
    m
}

pub fn storey_height(m: &Model, storey: GlobalId) -> f64 {
    let Some(z) = m.storeys.get(&storey).map(|s| s.elevation) else {
        return 3000.0;
    };
    m.storeys
        .values()
        .map(|s| s.elevation - z)
        .filter(|&d| d > 1.0)
        .fold(None, |a: Option<f64>, d| Some(a.map_or(d, |a| a.min(d))))
        .unwrap_or(3000.0)
}

/// The material to use: `name` exactly as in the model, else the library grade it names
/// (with a command adding it); `None` is the first concrete of the model, else C30/37.
fn material(m: &Model, name: Option<&str>) -> Result<(GlobalId, Option<Command>), String> {
    let Some(name) = name else {
        if let Some(x) = m
            .materials
            .values()
            .find(|x| x.category == MaterialCategory::Concrete)
        {
            return Ok((x.id, None));
        }
        let id = GlobalId::new();
        return Ok((
            id,
            Some(Command::AddMaterial(Material {
                id,
                name: "C30/37".into(),
                category: MaterialCategory::Concrete,
                props: bimble_core::library::lookup("C30/37")
                    .map(|m| m.props)
                    .unwrap_or_default(),
            })),
        ));
    };
    if let Some(x) = m.materials.values().find(|x| x.name == name) {
        return Ok((x.id, None));
    }
    let lib = bimble_core::library::lookup(name).ok_or(format!("unknown material: {name}"))?;
    // "Concrete C35/45 XC4" twice: the second call reuses the C35/45 the first one added.
    if let Some(x) = m
        .materials
        .values()
        .find(|x| x.name == lib.name && x.category == lib.category)
    {
        return Ok((x.id, None));
    }
    let mat = bimble_core::library::to_material(&lib);
    Ok((mat.id, Some(Command::AddMaterial(mat))))
}

/// The next free name with a prefix: P1, P2, …
fn next_name(m: &Model, prefix: &str) -> String {
    let used: std::collections::BTreeSet<&str> =
        m.elements.values().map(|e| e.name.as_str()).collect();
    (1..)
        .map(|n| format!("{prefix}{n}"))
        .find(|n| !used.contains(n.as_str()))
        .expect("an unused name")
}

/// Commands placing a new element on `storey` from clicked points (storey-relative mm: x, y;
/// z is ignored). Columns and walls stand on the storey level and are as high as the storey;
/// beams and slabs hang from the level above (top flush with it), like the demo frame.
/// `material`: a model material's name or a library grade ("C35/45"); `None` for concrete.
pub fn new_element(
    m: &Model,
    kind: Kind,
    storey: GlobalId,
    a: [f64; 2],
    b: [f64; 2],
    sizes: &Sizes,
    material: Option<&str>,
) -> Result<Vec<Command>, String> {
    if !m.storeys.contains_key(&storey) {
        return Err("choose a storey".into());
    }
    let h = storey_height(m, storey);
    let (mat, add_mat) = self::material(m, material)?;
    let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
    let len = dx.hypot(dy);
    if kind.clicks() == 2 && len < 10.0 {
        return Err("the two points are too close".into());
    }
    let dir = [dx / len, dy / len, 0.0];
    let (placement, generator) = match kind {
        Kind::Column => (
            Placement::at(a[0], a[1], 0.0),
            Generator::RectColumn {
                width: sizes.column,
                depth: sizes.column,
                height: h,
            },
        ),
        Kind::Beam => (
            Placement {
                origin: [a[0], a[1], h - sizes.beam_height / 2.0],
                z_axis: dir,
                x_axis: [-dir[1], dir[0], 0.0],
            },
            Generator::RectBeam {
                width: sizes.beam_width,
                height: sizes.beam_height,
                length: len,
            },
        ),
        Kind::Wall => (
            Placement {
                origin: [a[0], a[1], 0.0],
                z_axis: [0.0, 0.0, 1.0],
                x_axis: dir,
            },
            Generator::Wall {
                length: len,
                thickness: sizes.wall,
                height: h,
            },
        ),
        Kind::Slab => {
            let (x0, x1) = (a[0].min(b[0]), a[0].max(b[0]));
            let (y0, y1) = (a[1].min(b[1]), a[1].max(b[1]));
            if x1 - x0 < 10.0 || y1 - y0 < 10.0 {
                return Err("the slab rectangle is too thin".into());
            }
            (
                Placement::at(x0, y0, h - sizes.slab),
                Generator::Slab {
                    outline: vec![
                        [0.0, 0.0],
                        [x1 - x0, 0.0],
                        [x1 - x0, y1 - y0],
                        [0.0, y1 - y0],
                    ],
                    thickness: sizes.slab,
                },
            )
        }
    };
    let e = Element {
        id: GlobalId::new(),
        class: kind.class(),
        name: next_name(m, kind.prefix()),
        storey,
        material: mat,
        placement,
        generator,
        openings: Vec::new(),
        properties: Default::default(),
        ifc_entity: None,
    };
    Ok(add_mat
        .into_iter()
        .chain([Command::AddElement(e)])
        .collect())
}

/// Move elements by `d` (mm).
pub fn move_by(m: &Model, ids: &[GlobalId], d: [f64; 3]) -> Vec<Command> {
    ids.iter()
        .filter_map(|id| m.elements.get(id))
        .map(|e| {
            let mut p = e.placement.clone();
            for (o, d) in p.origin.iter_mut().zip(d) {
                *o += d;
            }
            Command::SetPlacement {
                id: e.id,
                placement: p,
            }
        })
        .collect()
}

/// Copies of elements moved by `d`, with new GlobalIds and names; returns the new ids too.
pub fn copy_by(m: &Model, ids: &[GlobalId], d: [f64; 3]) -> (Vec<Command>, Vec<GlobalId>) {
    let mut cmds = Vec::new();
    let mut new_ids = Vec::new();
    let mut names: std::collections::BTreeSet<String> =
        m.elements.values().map(|e| e.name.clone()).collect();
    for e in ids.iter().filter_map(|id| m.elements.get(id)) {
        let mut c = e.clone();
        c.id = GlobalId::new();
        // New openings too: export derives their GlobalIds from the copy's.
        for o in &mut c.openings {
            o.id = None;
        }
        // "B1" → "B1 (2)", "B1 (3)", …
        c.name = (2..)
            .map(|n| format!("{} ({n})", e.name))
            .find(|n| !names.contains(n))
            .expect("an unused name");
        names.insert(c.name.clone());
        for (o, d) in c.placement.origin.iter_mut().zip(d) {
            *o += d;
        }
        new_ids.push(c.id);
        cmds.push(Command::AddElement(c));
    }
    (cmds, new_ids)
}

pub fn delete(ids: &[GlobalId]) -> Vec<Command> {
    ids.iter()
        .map(|&id| Command::RemoveElement { id })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::History;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn demo() -> (Model, GlobalId) {
        let m = concrete_frame(&FrameSpec::default());
        let s = m.storeys.values().find(|s| s.elevation == 0.0).unwrap().id;
        (m, s)
    }

    #[test]
    fn new_model_has_one_storey_and_round_trips_canonically() {
        let m = new_model("Untitled");
        assert_eq!(m.project_name, "Untitled");
        assert_eq!(m.storeys.len(), 1);
        let s = m.storeys.values().next().unwrap();
        assert_eq!((s.name.as_str(), s.elevation), ("Level 0", 0.0));
        assert!(m.elements.is_empty() && m.materials.is_empty());
        m.validate().unwrap();
        let text = bimble_core::file::to_string(&m);
        let back = bimble_core::file::from_str(&text).unwrap();
        assert_eq!(back, m);
        assert_eq!(bimble_core::file::to_string(&back), text);
        // A fresh project id each time.
        assert_ne!(new_model("Untitled").project_id, m.project_id);
    }

    #[test]
    fn placed_elements_are_valid_and_sized_from_the_storey() {
        let (mut m, s) = demo();
        let sizes = Sizes::default();
        let h = storey_height(&m, s);
        assert_eq!(h, 3000.0);
        for kind in Kind::ALL {
            let cmds =
                new_element(&m, kind, s, [500.0, 500.0], [4500.0, 3500.0], &sizes, None).unwrap();
            let mut hist = History::default();
            hist.apply(&mut m, cmds).unwrap();
        }
        let added: Vec<&Element> = m
            .elements
            .values()
            .filter(|e| {
                ["P", "B", "M", "D"]
                    .iter()
                    .any(|p| e.name == format!("{p}1"))
            })
            .collect();
        assert_eq!(
            added.len(),
            4,
            "{:?}",
            added.iter().map(|e| &e.name).collect::<Vec<_>>()
        );
        // The beam is 5 m long (3-4-5), its top flush with the next level.
        let beam = added.iter().find(|e| e.class == IfcClass::IfcBeam).unwrap();
        let Generator::RectBeam { length, height, .. } = beam.generator else {
            panic!()
        };
        assert!((length - 5000.0).abs() < 1e-9);
        assert_eq!(beam.placement.origin[2] + height / 2.0, h);
        // The meshes are where the clicks were: the column's centre at (500, 500).
        let scene = bimble_geom::Scene::from_model(&m);
        let col = scene.elements.iter().find(|e| e.name == "P1").unwrap();
        let (mut lo, mut hi) = ([f64::MAX; 3], [f64::MIN; 3]);
        for p in &col.world_mesh().positions {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k] as f64 + scene.origin_mm[k]);
                hi[k] = hi[k].max(p[k] as f64 + scene.origin_mm[k]);
            }
        }
        assert!(((lo[0] + hi[0]) / 2.0 - 500.0).abs() < 0.01);
        assert!((hi[2] - lo[2] - 3000.0).abs() < 0.01);
    }

    #[test]
    fn points_too_close_are_refused() {
        let (m, s) = demo();
        assert!(
            new_element(
                &m,
                Kind::Beam,
                s,
                [0.0, 0.0],
                [1.0, 0.0],
                &Sizes::default(),
                None
            )
            .is_err()
        );
        assert!(
            new_element(
                &m,
                Kind::Slab,
                s,
                [0.0, 0.0],
                [5000.0, 2.0],
                &Sizes::default(),
                None
            )
            .is_err()
        );
    }

    /// A copy's openings are new openings: they don't carry the original's GlobalIds, which
    /// would then appear twice in an IFC export.
    #[test]
    fn copy_drops_opening_ids() {
        let (mut m, _) = demo();
        let id = *m.elements.keys().next().unwrap();
        let e = m.elements.get_mut(&id).unwrap();
        e.openings.push(bimble_core::Opening {
            id: Some(GlobalId::new()),
            placement: bimble_core::Placement::at(0.0, 0.0, 0.0),
            generator: e.generator.clone(),
        });
        let (cmds, _) = copy_by(&m, &[id], [0.0, 1000.0, 0.0]);
        let Command::AddElement(c) = &cmds[0] else {
            panic!("expected AddElement");
        };
        assert_eq!(c.openings.len(), 1);
        assert_eq!(c.openings[0].id, None);
    }

    fn slab(m: &Model, s: GlobalId, material: Option<&str>) -> Result<Vec<Command>, String> {
        let sizes = Sizes::default();
        new_element(
            m,
            Kind::Slab,
            s,
            [0.0, 0.0],
            [4000.0, 3000.0],
            &sizes,
            material,
        )
    }

    /// A library grade not in the model is added first, then used by the element.
    #[test]
    fn a_library_grade_is_added_then_used() {
        let (mut m, s) = demo();
        assert!(!m.materials.values().any(|x| x.name == "C35/45"));
        let cmds = slab(&m, s, Some("C35/45")).unwrap();
        let [Command::AddMaterial(mat), Command::AddElement(e)] = &cmds[..] else {
            panic!("{cmds:?}");
        };
        assert_eq!(mat.name, "C35/45");
        assert_eq!(mat.category, MaterialCategory::Concrete);
        assert_eq!(
            mat.props,
            bimble_core::library::lookup("C35/45").unwrap().props
        );
        assert_eq!(e.material, mat.id);
        History::default().apply(&mut m, cmds).unwrap();
    }

    /// A long name finds its grade once: asking again reuses the material added first.
    #[test]
    fn a_library_grade_is_added_once() {
        let (mut m, s) = demo();
        let name = Some("Concrete C35/45 XC4");
        let first = slab(&m, s, name).unwrap();
        let Command::AddMaterial(added) = &first[0] else {
            panic!("{first:?}");
        };
        let id = added.id;
        History::default().apply(&mut m, first).unwrap();
        let cmds = slab(&m, s, name).unwrap();
        let [Command::AddElement(e)] = &cmds[..] else {
            panic!("{cmds:?}");
        };
        assert_eq!(e.material, id);
        History::default().apply(&mut m, cmds).unwrap();
        assert_eq!(
            m.materials.values().filter(|x| x.name == "C35/45").count(),
            1
        );
    }

    /// A material already in the model is used as is: no second copy.
    #[test]
    fn a_model_material_is_used_by_name() {
        let (m, s) = demo();
        let x = m.materials.values().next().unwrap();
        let cmds = slab(&m, s, Some(&x.name)).unwrap();
        let [Command::AddElement(e)] = &cmds[..] else {
            panic!("{cmds:?}");
        };
        assert_eq!(e.material, x.id);
    }

    #[test]
    fn an_unknown_material_is_refused() {
        let (m, s) = demo();
        let err = slab(&m, s, Some("Unobtainium")).unwrap_err();
        assert!(err.contains("unknown material"), "{err}");
    }

    /// Agents send plain JSON: "slab", and only the sizes they change.
    #[test]
    fn kind_and_sizes_read_from_json() {
        let k: Kind = serde_json::from_str("\"slab\"").unwrap();
        assert_eq!(k, Kind::Slab);
        let s: Sizes = serde_json::from_str(r#"{"slab": 300}"#).unwrap();
        assert_eq!(
            s,
            Sizes {
                slab: 300.0,
                ..Sizes::default()
            }
        );
    }
}
