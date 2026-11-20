//! Verbs and queries by name, with JSON arguments, for MCP agents and the web UI (ADR 0048):
//! list the verbs, describe one (JSON schema of its arguments), run one.
//! Arguments are untrusted: unknown fields are refused and sizes are bounded.

use crate::{query, verbs};
use bimble_core::{Command, Generator, GlobalId, Model};
use schemars::JsonSchema;
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// Most element ids one call may name.
pub const MAX_IDS: usize = 10_000;
/// Largest coordinate or size, mm (10 km).
pub const MAX_MM: f64 = 1.0e7;

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct VerbInfo {
    pub name: &'static str,
    pub summary: &'static str,
}

const VERBS: [VerbInfo; 9] = [
    VerbInfo {
        name: "place",
        summary: "Place a new column, beam, wall or slab on a storey (storey-relative mm).",
    },
    VerbInfo {
        name: "move",
        summary: "Move elements by a vector d = [dx, dy, dz], mm.",
    },
    VerbInfo {
        name: "copy",
        summary: "Copy elements, moved by d = [dx, dy, dz] mm, with new GlobalIds and names.",
    },
    VerbInfo {
        name: "delete",
        summary: "Delete elements.",
    },
    VerbInfo {
        name: "set_generator",
        summary: "Change an element's sizes (width, height, length, depth, thickness), mm; missing ones are kept.",
    },
    VerbInfo {
        name: "set_view",
        summary: "Change a view's scale (1:N), true_north, crop (null: none) or plan range offsets, mm; missing ones are kept.",
    },
    VerbInfo {
        name: "set_schedule",
        summary: "Change a schedule's grouping by level, family filter (null: every family) or columns (field names); missing ones are kept.",
    },
    VerbInfo {
        name: "set_material",
        summary: "Give elements a material: a model material's name or a library grade (\"C35/45\").",
    },
    VerbInfo {
        name: "set_project_field",
        summary: "Set one project information field by its label (the key is made from it); an empty value removes it.",
    },
];

/// Arguments of `place`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct PlaceArgs {
    kind: verbs::Kind,
    /// GlobalId of the storey.
    #[schemars(with = "String")]
    storey: GlobalId,
    /// First point [x, y], mm: a column's centre, an axis start, a slab corner.
    a: [f64; 2],
    /// Second point [x, y], mm; not used for a column.
    b: Option<[f64; 2]>,
    /// Sizes of new elements, mm; missing ones take defaults.
    sizes: Option<verbs::Sizes>,
    /// A model material's name or a library grade ("C35/45"); default: the model's concrete.
    material: Option<String>,
}

/// Arguments of `move` and `copy`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct MoveArgs {
    /// GlobalIds of the elements.
    #[schemars(with = "Vec<String>")]
    ids: Vec<GlobalId>,
    /// Vector [dx, dy, dz], mm.
    d: [f64; 3],
}

/// Arguments of `delete`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct IdsArgs {
    /// GlobalIds of the elements.
    #[schemars(with = "Vec<String>")]
    ids: Vec<GlobalId>,
}

/// Arguments of `set_material`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetMaterialArgs {
    /// GlobalIds of the elements.
    #[schemars(with = "Vec<String>")]
    ids: Vec<GlobalId>,
    /// A model material's name or a library grade ("C35/45").
    material: String,
}

/// Arguments of `set_project_field`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetProjectFieldArgs {
    /// The field's label ("Maître d'ouvrage"); the key is made from it (`project::key_of`).
    label: String,
    /// The value; empty removes the field.
    value: String,
}

/// Arguments of `set_generator`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetGeneratorArgs {
    /// GlobalId of the element.
    #[schemars(with = "String")]
    id: GlobalId,
    /// New sizes, mm, > 0; only the ones the element's generator has.
    params: GeneratorParams,
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct GeneratorParams {
    width: Option<f64>,
    height: Option<f64>,
    length: Option<f64>,
    depth: Option<f64>,
    thickness: Option<f64>,
}

/// Arguments of `set_view`: the `View` fields to change, others kept.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetViewArgs {
    /// GlobalId of the view.
    #[schemars(with = "String")]
    id: GlobalId,
    /// 1:N, N from 1 to 5000.
    scale: Option<u32>,
    /// Drawn in true north rather than project north.
    true_north: Option<bool>,
    /// The crop rectangle in the view's plane, world mm; null: no crop.
    #[serde(default, deserialize_with = "some")]
    crop: Option<Option<CropArgs>>,
    /// A plan's view range: the planes' offsets from their levels, mm.
    range: Option<RangeArgs>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct CropArgs {
    min: [f64; 2],
    max: [f64; 2],
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct RangeArgs {
    top: Option<PlaneArgs>,
    cut: Option<PlaneArgs>,
    bottom: Option<PlaneArgs>,
    depth: Option<PlaneArgs>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct PlaneArgs {
    offset_mm: Option<f64>,
}

/// A present field, even `null`, is `Some`: `crop: null` clears, a missing `crop` keeps.
fn some<'de, D: serde::Deserializer<'de>, T: Deserialize<'de>>(
    d: D,
) -> Result<Option<T>, D::Error> {
    T::deserialize(d).map(Some)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NoArgs {}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct IdArgs {
    id: GlobalId,
}

pub fn list() -> Vec<VerbInfo> {
    VERBS.to_vec()
}

/// JSON schema of a verb's arguments.
pub fn describe(name: &str) -> Option<Value> {
    let s = match name {
        "place" => schemars::schema_for!(PlaceArgs),
        "move" | "copy" => schemars::schema_for!(MoveArgs),
        "delete" => schemars::schema_for!(IdsArgs),
        "set_generator" => schemars::schema_for!(SetGeneratorArgs),
        "set_view" => schemars::schema_for!(SetViewArgs),
        "set_schedule" => schemars::schema_for!(SetScheduleArgs),
        "set_material" => schemars::schema_for!(SetMaterialArgs),
        "set_project_field" => schemars::schema_for!(SetProjectFieldArgs),
        _ => return None,
    };
    Some(s.to_value())
}

fn names() -> String {
    VERBS.map(|v| v.name).join(", ")
}

fn parse<T: DeserializeOwned>(args: Value) -> Result<T, String> {
    // Before parsing: a huge id list costs nothing to refuse.
    if let Some(ids) = args.get("ids").and_then(Value::as_array)
        && ids.len() > MAX_IDS
    {
        return Err(format!("{} ids: at most {MAX_IDS} per call", ids.len()));
    }
    let args = if args.is_null() {
        Value::Object(Default::default())
    } else {
        args
    };
    serde_json::from_value(args).map_err(|e| format!("bad arguments: {e}"))
}

fn mm(what: &str, v: &[f64]) -> Result<(), String> {
    match v.iter().find(|x| !x.is_finite() || x.abs() > MAX_MM) {
        Some(x) => Err(format!(
            "{what}: {x} is not a finite value within ±{MAX_MM} mm"
        )),
        None => Ok(()),
    }
}

/// Run a verb: the commands to apply through `History` (undo, validation).
pub fn run(m: &Model, name: &str, args: Value) -> Result<Vec<Command>, String> {
    match name {
        "place" => {
            let p: PlaceArgs = parse(args)?;
            mm("a", &p.a)?;
            let b = p.b.unwrap_or(p.a);
            mm("b", &b)?;
            let sizes = p.sizes.unwrap_or_default();
            for x in [
                sizes.column,
                sizes.beam_width,
                sizes.beam_height,
                sizes.wall,
                sizes.slab,
            ] {
                verbs::size("sizes", x)?;
            }
            if p.b.is_none() && p.kind.clicks() == 2 {
                return Err(format!("{}: b is required", p.kind.label()));
            }
            verbs::new_element(m, p.kind, p.storey, p.a, b, &sizes, p.material.as_deref())
        }
        "move" | "copy" => {
            let a: MoveArgs = parse(args)?;
            mm("d", &a.d)?;
            Ok(if name == "move" {
                verbs::move_by(m, &a.ids, a.d)
            } else {
                verbs::copy_by(m, &a.ids, a.d).0
            })
        }
        "delete" => Ok(verbs::delete(&parse::<IdsArgs>(args)?.ids)),
        "set_generator" => set_generator(m, parse(args)?),
        "set_view" => set_view(m, parse(args)?),
        "set_schedule" => set_schedule(m, parse(args)?),
        "set_material" => set_material(m, parse(args)?),
        "set_project_field" => set_project_field(m, parse(args)?),
        _ => Err(format!("unknown verb {name:?}; known: {}", names())),
    }
}

/// One `SetProject`: the whole map with the label's key set (or removed, for an empty value,
/// ADR 0030), validated as `SetProject` is. Unchanged: no command, so no empty undo step.
fn set_project_field(m: &Model, a: SetProjectFieldArgs) -> Result<Vec<Command>, String> {
    let key = bimble_core::project::key_of(&a.label);
    if key.is_empty() {
        return Err(format!("{:?}: a label needs letters or digits", a.label));
    }
    let mut project = m.project.clone();
    if a.value.is_empty() {
        project.remove(&key);
    } else {
        project.insert(key, a.value);
    }
    if project == m.project {
        return Ok(vec![]);
    }
    bimble_core::project::validate(&project)?;
    Ok(vec![Command::SetProject { project }])
}

/// `SetElementMaterial` for each element whose material changes (after an `AddMaterial` for a
/// library grade not in the model yet); none changed: no command, so no empty undo step.
fn set_material(m: &Model, a: SetMaterialArgs) -> Result<Vec<Command>, String> {
    let (material, add) = verbs::material(m, Some(&a.material))?;
    let mut cmds = Vec::new();
    for id in a.ids {
        let e = m.elements.get(&id).ok_or(format!("no element {id:?}"))?;
        if e.material != material {
            cmds.push(Command::SetElementMaterial { id, material });
        }
    }
    if !cmds.is_empty() {
        cmds.splice(0..0, add);
    }
    Ok(cmds)
}

/// One `SetGenerator`: the element's generator with the given sizes replaced, through its
/// serde form, so the names are the generator's own field names (as `query("element")` shows).
fn set_generator(m: &Model, a: SetGeneratorArgs) -> Result<Vec<Command>, String> {
    let e = m
        .elements
        .get(&a.id)
        .ok_or(format!("no element {:?}", a.id))?;
    let p = a.params;
    let given = [
        ("width", p.width),
        ("height", p.height),
        ("length", p.length),
        ("depth", p.depth),
        ("thickness", p.thickness),
    ];
    let mut g = serde_json::to_value(&e.generator).map_err(|e| e.to_string())?;
    let mut n = 0;
    for (k, v) in given {
        let Some(v) = v else { continue };
        verbs::size(k, v)?;
        match g.get_mut(k) {
            Some(x) if x.is_number() => *x = v.into(),
            _ => return Err(format!("{k}: not a size of this {}", g["kind"])),
        }
        n += 1;
    }
    if n == 0 {
        return Err("params: no size given".into());
    }
    let generator: Generator = serde_json::from_value(g).map_err(|e| e.to_string())?;
    // The same sizes again: no command, so no empty undo step.
    if generator == e.generator {
        return Ok(vec![]);
    }
    Ok(vec![Command::SetGenerator {
        id: a.id,
        generator,
    }])
}

/// One `SetView` (`AddView` for a default view): the view with the given fields replaced, validated as the command will be.
fn set_view(m: &Model, a: SetViewArgs) -> Result<Vec<Command>, String> {
    // A default view not changed yet isn't stored: the first change adds it (as the viewer does).
    let old = bimble_core::view::effective(&m.views, &m.levels())
        .into_iter()
        .find(|v| v.id == a.id)
        .ok_or(format!("no view {:?}", a.id))?;
    let mut v = old.clone();
    if let Some(s) = a.scale {
        v.scale = s;
    }
    if let Some(t) = a.true_north {
        v.true_north = t;
    }
    if let Some(c) = a.crop {
        v.crop = match c {
            Some(c) => {
                mm("crop", &[c.min, c.max].concat())?;
                Some(bimble_core::view::Crop {
                    min: c.min,
                    max: c.max,
                })
            }
            None => None,
        };
    }
    if let Some(r) = a.range {
        let bimble_core::view::ViewKind::Plan { range, .. } = &mut v.kind else {
            return Err(format!("view {}: only a plan has a view range", v.name));
        };
        for (k, p, plane) in [
            ("top", r.top, &mut range.top),
            ("cut", r.cut, &mut range.cut),
            ("bottom", r.bottom, &mut range.bottom),
            ("depth", r.depth, &mut range.depth),
        ] {
            if let Some(o) = p.and_then(|p| p.offset_mm) {
                mm(k, &[o])?;
                plane.offset_mm = o + 0.0; // no -0.0
            }
        }
    }
    v.validate(&m.levels())?;
    // The same values again: no command, so no empty undo step.
    if v == old {
        return Ok(vec![]);
    }
    Ok(vec![if m.views.contains_key(&v.id) {
        Command::SetView(v)
    } else {
        Command::AddView(v)
    }])
}

/// Arguments of `set_schedule`: the `ScheduleDef` fields to change, others kept.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetScheduleArgs {
    /// The schedule's name (built-in or the model's own), case-insensitive.
    name: String,
    /// Rows grouped by level, a subtotal under each.
    group_by_level: Option<bool>,
    /// Only elements of this material family ("concrete", "steel", ...); null: every family.
    #[serde(default, deserialize_with = "some")]
    #[schemars(with = "Option<String>")]
    family: Option<Option<bimble_core::MaterialCategory>>,
    /// The columns, in order: field names ("level", "name", "volume", ...).
    #[schemars(with = "Option<Vec<String>>")]
    columns: Option<Vec<bimble_core::schedule::Field>>,
}

/// One `SetSchedules`: the stored list with the named schedule changed (a built-in one not
/// stored yet is added, as the viewer does).
fn set_schedule(m: &Model, a: SetScheduleArgs) -> Result<Vec<Command>, String> {
    let same = |d: &bimble_core::schedule::ScheduleDef| d.name.eq_ignore_ascii_case(&a.name);
    let old = bimble_core::schedule::effective(&m.schedules)
        .into_iter()
        .find(same)
        .ok_or(format!("no schedule {:?}", a.name))?;
    let mut d = old.clone();
    if let Some(g) = a.group_by_level {
        d.group_by_level = g;
    }
    if let Some(f) = a.family {
        d.family = f;
    }
    if let Some(c) = a.columns {
        d.fields = c;
    }
    if d == old {
        return Ok(vec![]);
    }
    let mut v = m.schedules.clone();
    match v.iter_mut().find(|s| same(s)) {
        Some(s) => *s = d,
        None => v.push(d),
    }
    bimble_core::schedule::validate_all(&v)?;
    Ok(vec![Command::SetSchedules { schedules: v }])
}

/// Read-only queries by name: "storeys", "materials", "elements" (an `ElementFilter`),
/// "element" (`{"id": ...}`), "project", "views", "sheets", "schedules".
pub fn query(m: &Model, name: &str, args: Value) -> Result<Value, String> {
    let v = match name {
        "storeys" => {
            parse::<NoArgs>(args)?;
            serde_json::to_value(query::storeys(m))
        }
        "materials" => {
            parse::<NoArgs>(args)?;
            serde_json::to_value(query::materials(m))
        }
        "elements" => serde_json::to_value(query::elements(m, &parse(args)?)),
        "project" | "views" | "sheets" | "schedules" => {
            parse::<NoArgs>(args)?;
            match name {
                "project" => serde_json::to_value(query::project(m)),
                "views" => serde_json::to_value(query::views(m)),
                "sheets" => serde_json::to_value(query::sheets(m)),
                _ => serde_json::to_value(query::schedules(m)),
            }
        }
        "element" => {
            let id = parse::<IdArgs>(args)?.id;
            let e = query::element(m, id).ok_or(format!("no element {id:?}"))?;
            serde_json::to_value(e)
        }
        _ => {
            return Err(format!(
                "unknown query {name:?}; known: storeys, materials, elements, element, project, views, sheets, schedules"
            ));
        }
    };
    v.map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::History;
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use serde_json::json;

    fn demo() -> (Model, GlobalId) {
        let m = concrete_frame(&FrameSpec::default());
        let s = m.storeys.values().find(|s| s.elevation == 0.0).unwrap().id;
        (m, s)
    }

    #[test]
    fn list_and_describe() {
        let n: Vec<_> = list().iter().map(|v| v.name).collect();
        assert_eq!(
            n,
            [
                "place",
                "move",
                "copy",
                "delete",
                "set_generator",
                "set_view",
                "set_schedule",
                "set_material",
                "set_project_field"
            ]
        );
        let s = describe("place").unwrap();
        let p = s["properties"].as_object().unwrap();
        for k in ["kind", "storey", "a", "material"] {
            assert!(p.contains_key(k), "{k} in {s}");
        }
        for v in list() {
            assert!(describe(v.name).is_some(), "{}", v.name);
        }
        assert!(describe("nope").is_none());
    }

    /// "Maître d'ouvrage" → "ma_tre_d_ouvrage" (key_of: non-ASCII and punctuation become one
    /// `_`); same again: nothing; too long: refused; empty value: the key removed.
    #[test]
    fn set_project_field_by_label() {
        let (mut m, _) = demo();
        let set = |m: &Model, value: &str| {
            run(
                m,
                "set_project_field",
                json!({"label": "Maître d'ouvrage", "value": value}),
            )
        };
        let cmds = set(&m, "Ville de Lausanne").unwrap();
        assert_eq!(cmds.len(), 1);
        History::default().apply(&mut m, cmds).unwrap();
        assert_eq!(m.project["ma_tre_d_ouvrage"], "Ville de Lausanne");
        assert!(set(&m, "Ville de Lausanne").unwrap().is_empty());
        let before = m.clone();
        let long = "x".repeat(bimble_core::project::MAX_VALUE + 1);
        assert!(set(&m, &long).is_err());
        assert_eq!(m, before, "model unchanged");
        assert!(run(&m, "set_project_field", json!({"label": "°", "value": "x"})).is_err());
        let cmds = set(&m, "").unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        assert!(!m.project.contains_key("ma_tre_d_ouvrage"));
        assert!(set(&m, "").unwrap().is_empty());
    }

    /// Two ids, a library grade (added once), unknown material refused, same material: nothing.
    #[test]
    fn set_material_by_name() {
        let (mut m, _) = demo();
        let ids: Vec<GlobalId> = m.elements.keys().take(2).copied().collect();
        let before = m.clone();
        let err = run(
            &m,
            "set_material",
            json!({"ids": ids, "material": "Unobtainium"}),
        );
        assert!(err.unwrap_err().contains("unknown material"));
        assert_eq!(m, before);
        let cmds = run(
            &m,
            "set_material",
            json!({"ids": ids, "material": "C35/45"}),
        )
        .unwrap();
        assert!(
            matches!(
                &cmds[..],
                [
                    Command::AddMaterial(_),
                    Command::SetElementMaterial { .. },
                    Command::SetElementMaterial { .. }
                ]
            ),
            "{cmds:?}"
        );
        let mut h = History::default();
        h.apply(&mut m, cmds).unwrap();
        let c35 = m.elements[&ids[0]].material;
        assert_eq!(m.materials[&c35].name, "C35/45");
        assert_eq!(m.elements[&ids[1]].material, c35);
        let again = run(
            &m,
            "set_material",
            json!({"ids": ids, "material": "C35/45"}),
        )
        .unwrap();
        assert!(again.is_empty(), "{again:?}");
        assert!(h.undo(&mut m));
        assert_eq!(m, before);
    }

    #[test]
    fn place_a_column_applies() {
        let (mut m, s) = demo();
        let before = m.elements.len();
        let cmds = run(
            &m,
            "place",
            json!({"kind": "column", "storey": s, "a": [0, 0]}),
        )
        .unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        assert_eq!(m.elements.len(), before + 1);
    }

    #[test]
    fn place_a_slab_with_a_library_grade() {
        let (m, s) = demo();
        let cmds = run(
            &m,
            "place",
            json!({"kind": "slab", "storey": s, "a": [0, 0], "b": [4000, 3000], "material": "C35/45"}),
        )
        .unwrap();
        assert!(
            matches!(&cmds[..], [Command::AddMaterial(_), Command::AddElement(_)]),
            "{cmds:?}"
        );
    }

    #[test]
    fn move_copy_delete_by_name() {
        let (m, _) = demo();
        let id = *m.elements.keys().next().unwrap();
        for (verb, args) in [
            ("move", json!({"ids": [id], "d": [0, 0, 100]})),
            ("copy", json!({"ids": [id], "d": [0, 1000, 0]})),
            ("delete", json!({"ids": [id]})),
        ] {
            let cmds = run(&m, verb, args).unwrap();
            assert_eq!(cmds.len(), 1, "{verb}");
            History::default().apply(&mut m.clone(), cmds).unwrap();
        }
    }

    #[test]
    fn bad_input_is_refused() {
        let (m, s) = demo();
        let err = run(&m, "explode", json!({})).unwrap_err();
        assert!(err.contains("place"), "{err}");
        let place = |extra: Value| {
            let mut a = json!({"kind": "column", "storey": s, "a": [0, 0]});
            a.as_object_mut()
                .unwrap()
                .extend(extra.as_object().unwrap().clone());
            run(&m, "place", a)
        };
        assert!(place(json!({"colour": "red"})).is_err());
        assert!(place(json!({"a": [1e9, 0]})).is_err());
        assert!(place(json!({"sizes": {"column": 0}})).is_err());
        assert!(place(json!({"kind": "beam"})).is_err(), "beam without b");
        // JSON has no NaN: test the bound itself.
        assert!(mm("a", &[f64::NAN]).is_err());
        assert!(mm("a", &[f64::INFINITY]).is_err());
        assert!(mm("a", &[MAX_MM]).is_ok());
        let ids = vec![s; MAX_IDS + 1];
        let err = run(&m, "delete", json!({ "ids": ids })).unwrap_err();
        assert!(err.contains("10000"), "{err}");
        let ids = vec![s; MAX_IDS];
        assert!(run(&m, "delete", json!({ "ids": ids })).is_ok());
    }

    #[test]
    fn queries_by_name() {
        let (m, _) = demo();
        let v = query(&m, "storeys", json!({})).unwrap();
        assert_eq!(v.as_array().unwrap().len(), m.storeys.len());
        assert!(m.storeys.len() > 1);
        let v = query(&m, "materials", Value::Null).unwrap();
        assert_eq!(v.as_array().unwrap().len(), m.materials.len());
        let v = query(&m, "elements", json!({"limit": 2})).unwrap();
        assert_eq!(v.as_array().unwrap().len(), 2);
        let id = *m.elements.keys().next().unwrap();
        let v = query(&m, "element", json!({ "id": id })).unwrap();
        assert_eq!(v["id"], json!(id));
        assert!(query(&m, "storeys", json!({"x": 1})).is_err());
        let err = query(&m, "nope", json!({})).unwrap_err();
        for k in [
            "storeys",
            "element",
            "project",
            "views",
            "sheets",
            "schedules",
        ] {
            assert!(err.contains(k), "{err}");
        }
        assert_eq!(
            query(&m, "project", json!({})).unwrap()["name"],
            json!(m.project_name)
        );
        for k in ["views", "sheets", "schedules"] {
            let v = query(&m, k, Value::Null).unwrap();
            assert!(v.is_array(), "{k}: {v}");
        }
        assert!(query(&m, "views", json!({"limit": 1})).is_err());
    }

    #[test]
    fn set_generator_changes_one_size() {
        let (mut m, _) = demo();
        let beam = m
            .elements
            .values()
            .find(|e| matches!(e.generator, Generator::RectBeam { .. }))
            .unwrap();
        let (id, name) = (beam.id, beam.name.clone());
        let Generator::RectBeam { height, length, .. } = beam.generator else {
            unreachable!()
        };
        let other = m.elements.values().find(|e| e.id != id).unwrap().clone();
        let cmds = run(
            &m,
            "set_generator",
            json!({"id": id, "params": {"width": 345}}),
        )
        .unwrap();
        assert_eq!(cmds.len(), 1);
        History::default().apply(&mut m, cmds).unwrap();
        let e = &m.elements[&id];
        assert_eq!(
            e.generator,
            Generator::RectBeam {
                width: 345.0,
                height,
                length
            }
        );
        assert_eq!((e.id, &e.name), (id, &name), "GlobalId kept");
        assert_eq!(m.elements[&other.id], other, "other element unchanged");
    }

    #[test]
    fn set_generator_same_sizes_is_no_command() {
        let (m, _) = demo();
        let beam = m
            .elements
            .values()
            .find(|e| matches!(e.generator, Generator::RectBeam { .. }))
            .unwrap();
        let Generator::RectBeam { height, .. } = beam.generator else {
            unreachable!()
        };
        let args = serde_json::json!({"id": beam.id, "params": {"height": height}});
        assert!(run(&m, "set_generator", args).unwrap().is_empty());
    }

    #[test]
    fn set_generator_refuses_bad_input() {
        let (m, s) = demo();
        let id = *m.elements.keys().next().unwrap();
        let before = m.clone();
        for args in [
            json!({"id": s, "params": {"width": 300}}),
            json!({"id": id, "params": {"width": 0}}),
            json!({"id": id, "params": {"width": -1}}),
            json!({"id": id, "params": {"width": MAX_MM * 2.0}}),
            json!({"id": id, "params": {}}),
            json!({"id": id, "params": {"radius": 3}}),
        ] {
            assert!(run(&m, "set_generator", args.clone()).is_err(), "{args}");
        }
        let slab = m
            .elements
            .values()
            .find(|e| matches!(e.generator, Generator::Slab { .. }))
            .unwrap()
            .id;
        let err = run(
            &m,
            "set_generator",
            json!({"id": slab, "params": {"width": 300}}),
        )
        .unwrap_err();
        assert!(err.contains("slab"), "{err}");
        assert_eq!(m, before, "model unchanged");
    }

    fn first_plan(m: &Model) -> bimble_core::view::View {
        bimble_core::view::effective(&m.views, &m.levels())
            .into_iter()
            .find(|v| matches!(v.kind, bimble_core::view::ViewKind::Plan { .. }))
            .unwrap()
    }

    #[test]
    fn set_schedule_changes_the_named_schedule() {
        let (mut m, _) = demo();
        let name = bimble_core::schedule::effective(&m.schedules)[0]
            .name
            .clone();
        let cmds = run(
            &m,
            "set_schedule",
            json!({"name": name.to_uppercase(), "family": "steel", "columns": ["name", "volume"]}),
        )
        .unwrap();
        let [Command::SetSchedules { schedules }] = cmds.as_slice() else {
            panic!("{cmds:?}");
        };
        let d = schedules.iter().find(|d| d.name == name).unwrap();
        assert_eq!(d.family, Some(bimble_core::MaterialCategory::Steel));
        assert_eq!(d.fields.len(), 2);
        History::default().apply(&mut m, cmds).unwrap();
        // Unchanged: no command; null family clears.
        assert!(
            run(&m, "set_schedule", json!({"name": name, "family": "steel"}))
                .unwrap()
                .is_empty()
        );
        let cmds = run(&m, "set_schedule", json!({"name": name, "family": null})).unwrap();
        let [Command::SetSchedules { schedules }] = cmds.as_slice() else {
            panic!("{cmds:?}");
        };
        assert_eq!(schedules.len(), 1, "replaced, not added");
        assert_eq!(schedules[0].family, None);
        for args in [
            json!({"name": "nope"}),
            json!({"name": name, "columns": ["nope"]}),
            json!({"name": name, "columns": []}),
            json!({"name": name, "family": "cheese"}),
        ] {
            assert!(run(&m, "set_schedule", args.clone()).is_err(), "{args}");
        }
    }

    #[test]
    fn set_view_changes_only_the_given_fields() {
        let (mut m, _) = demo();
        let old = first_plan(&m);
        assert_eq!(old.scale, 100);
        let cmds = run(&m, "set_view", json!({"id": old.id, "scale": 50})).unwrap();
        assert_eq!(cmds.len(), 1);
        History::default().apply(&mut m, cmds).unwrap();
        let mut want = old.clone();
        want.scale = 50;
        assert_eq!(m.views[&old.id], want, "only the scale changed");
        assert_eq!(
            first_plan(&m),
            want,
            "the stored view replaces the default one"
        );

        let cmds = run(
            &m,
            "set_view",
            json!({"id": old.id, "range": {"cut": {"offset_mm": 300}}}),
        )
        .unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        let bimble_core::view::ViewKind::Plan { range, .. } = &m.views[&old.id].kind else {
            unreachable!()
        };
        let bimble_core::view::ViewKind::Plan { range: was, .. } = &old.kind else {
            unreachable!()
        };
        assert_eq!(range.cut.offset_mm, 300.0);
        assert_ne!(was.cut.offset_mm, 300.0);
        assert_eq!(
            (range.top, range.bottom, range.depth),
            (was.top, was.bottom, was.depth)
        );
        assert_eq!(range.cut.level, was.cut.level);

        let crop = json!({"id": old.id, "crop": {"min": [0, 0], "max": [1000, 2000]}});
        let cmds = run(&m, "set_view", crop).unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        assert!(m.views[&old.id].crop.is_some());
        let clear = json!({"id": old.id, "crop": null});
        let cmds = run(&m, "set_view", clear).unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        assert!(m.views[&old.id].crop.is_none());
    }

    #[test]
    fn set_view_same_values_is_no_command() {
        let (m, _) = demo();
        let v = first_plan(&m);
        let args = json!({"id": v.id, "scale": v.scale, "true_north": v.true_north});
        assert!(run(&m, "set_view", args).unwrap().is_empty());
        assert!(run(&m, "set_view", json!({"id": v.id})).unwrap().is_empty());
    }

    #[test]
    fn set_view_refuses_bad_input() {
        let (m, s) = demo();
        let id = first_plan(&m).id;
        let before = m.clone();
        for args in [
            json!({"id": s, "scale": 50}),
            json!({"id": id, "scale": 0}),
            json!({"id": id, "scale": 5001}),
            json!({"id": id, "scale": 1.5}),
            json!({"id": id, "range": {"cut": {"offset_mm": MAX_MM * 2.0}}}),
            json!({"id": id, "range": {"cut": {"offset_mm": -5000}}}),
            json!({"id": id, "range": {"cut": {"level": "above"}}}),
            json!({"id": id, "crop": {"min": [10, 0], "max": [0, 10]}}),
            json!({"id": id, "name": "x"}),
        ] {
            assert!(run(&m, "set_view", args.clone()).is_err(), "{args}");
        }
        // JSON has no NaN; the offset goes through `mm`, which refuses it.
        assert!(mm("cut", &[f64::NAN]).is_err());
        assert_eq!(m, before, "model unchanged");
    }
}
