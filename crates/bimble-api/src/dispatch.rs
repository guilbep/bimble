//! Verbs and queries by name, with JSON arguments, for MCP agents and the web UI (ADR 0048):
//! list the verbs, describe one (JSON schema of its arguments), run one.
//! Arguments are untrusted: unknown fields are refused and sizes are bounded.

use crate::{query, verbs};
use bimble_core::{Command, GlobalId, Model};
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

const VERBS: [VerbInfo; 5] = [
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
            let s = [
                sizes.column,
                sizes.beam_width,
                sizes.beam_height,
                sizes.wall,
                sizes.slab,
            ];
            mm("sizes", &s)?;
            if s.iter().any(|&x| x <= 0.0) {
                return Err("sizes must be > 0".into());
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
        _ => Err(format!("unknown verb {name:?}; known: {}", names())),
    }
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
        mm(k, &[v])?;
        if v <= 0.0 {
            return Err(format!("{k} must be > 0"));
        }
        match g.get_mut(k) {
            Some(x) if x.is_number() => *x = v.into(),
            _ => return Err(format!("{k}: not a size of this {}", g["kind"])),
        }
        n += 1;
    }
    if n == 0 {
        return Err("params: no size given".into());
    }
    let generator = serde_json::from_value(g).map_err(|e| e.to_string())?;
    Ok(vec![Command::SetGenerator {
        id: a.id,
        generator,
    }])
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
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use bimble_core::{Generator, History};
    use serde_json::json;

    fn demo() -> (Model, GlobalId) {
        let m = concrete_frame(&FrameSpec::default());
        let s = m.storeys.values().find(|s| s.elevation == 0.0).unwrap().id;
        (m, s)
    }

    #[test]
    fn list_and_describe() {
        let n: Vec<_> = list().iter().map(|v| v.name).collect();
        assert_eq!(n, ["place", "move", "copy", "delete", "set_generator"]);
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
}
