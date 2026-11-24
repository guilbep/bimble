//! Verbs and queries by name, with JSON arguments, for MCP agents and the web UI (ADR 0048):
//! list the verbs, describe one (JSON schema of its arguments), run one.
//! Arguments are untrusted: unknown fields are refused and sizes are bounded.

use crate::{query, verbs};
use bimble_core::{Command, Generator, GlobalId, Model, Placement};
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

const VERBS: [VerbInfo; 14] = [
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
        name: "set_note",
        summary: "Write a one-line note on an element (the property \"Note\"); an empty text removes it.",
    },
    VerbInfo {
        name: "set_project_field",
        summary: "Set one project information field by its label (the key is made from it); an empty value removes it.",
    },
    VerbInfo {
        name: "rotate",
        summary: "Rotate elements by angle_deg about the vertical axis through about = [x, y] mm (default: the centre of their origins).",
    },
    VerbInfo {
        name: "mirror",
        summary: "Mirror elements in the vertical plane through axis.point along axis.dir; placements stay right-handed.",
    },
    VerbInfo {
        name: "add_sheet",
        summary: "Add a sheet (default: the next free number, \"New sheet\", A1 landscape) with a new GlobalId.",
    },
    VerbInfo {
        name: "set_sheet",
        summary: "Change a sheet's number, name, paper (A4..A0), portrait or revision; missing ones are kept, an empty revision removes it.",
    },
];

/// Arguments of `add_sheet`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct AddSheetArgs {
    /// Drawing number, unique; default: the next free one ("S-101" → "S-102").
    number: Option<String>,
    name: Option<String>,
    /// A4, A3, A2, A1 or A0.
    paper: Option<String>,
    portrait: Option<bool>,
}

/// Arguments of `set_sheet`: the fields to change, others kept.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetSheetArgs {
    /// GlobalId of the sheet.
    #[schemars(with = "String")]
    id: GlobalId,
    number: Option<String>,
    name: Option<String>,
    paper: Option<String>,
    portrait: Option<bool>,
    /// The title block's {revision}; empty: removed.
    revision: Option<String>,
}

/// A sheet as `AddSheet` / `SetSheet` check it: valid, its number not used by another sheet.
fn check_sheet(m: &Model, s: &bimble_core::sheet::Sheet) -> Result<(), String> {
    let ids = m.view_ids();
    s.validate(|v| ids.contains(&v))?;
    if m.sheets
        .values()
        .any(|o| o.id != s.id && o.number == s.number)
    {
        return Err(format!("sheet number {} is already used", s.number));
    }
    Ok(())
}

/// One `AddSheet`, defaults as the viewer's New sheet (`Sheet::new`, `next_number`).
fn add_sheet(m: &Model, a: AddSheetArgs) -> Result<Vec<Command>, String> {
    use bimble_core::sheet::{Sheet, next_number};
    let number = a
        .number
        .unwrap_or_else(|| next_number(m.sheets.values().map(|s| s.number.as_str())));
    let mut s = Sheet::new(number, a.name.unwrap_or_else(|| "New sheet".into()));
    if let Some(p) = a.paper {
        s.paper = p;
    }
    if let Some(p) = a.portrait {
        s.portrait = p;
    }
    check_sheet(m, &s)?;
    Ok(vec![Command::AddSheet(s)])
}

/// One `SetSheet` with the given fields replaced; unchanged: no command.
fn set_sheet(m: &Model, a: SetSheetArgs) -> Result<Vec<Command>, String> {
    let old = m.sheets.get(&a.id).ok_or(format!("no sheet {:?}", a.id))?;
    let mut s = old.clone();
    if let Some(n) = a.number {
        s.number = n;
    }
    if let Some(n) = a.name {
        s.name = n;
    }
    if let Some(p) = a.paper {
        s.paper = p;
    }
    if let Some(p) = a.portrait {
        s.portrait = p;
    }
    if let Some(r) = a.revision {
        if r.chars().count() > 200 {
            return Err("revision: at most 200 characters".into());
        }
        if r.is_empty() {
            s.fields.remove("revision");
        } else {
            s.fields.insert("revision".into(), r);
        }
    }
    if s == *old {
        return Ok(vec![]);
    }
    check_sheet(m, &s)?;
    Ok(vec![Command::SetSheet(s)])
}

/// Arguments of `rotate`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct RotateArgs {
    /// GlobalIds of the elements.
    #[schemars(with = "Vec<String>")]
    ids: Vec<GlobalId>,
    /// Angle, degrees, counter-clockwise seen from above; |angle| ≤ 360.
    angle_deg: f64,
    /// Centre [x, y], mm; default: the centre of the elements' placement origins.
    about: Option<[f64; 2]>,
}

/// A vertical mirror plane: through `point`, along `dir` (plan, mm).
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct MirrorAxis {
    point: [f64; 2],
    dir: [f64; 2],
}

/// Arguments of `mirror`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct MirrorArgs {
    /// GlobalIds of the elements.
    #[schemars(with = "Vec<String>")]
    ids: Vec<GlobalId>,
    axis: MirrorAxis,
}

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

/// Arguments of `set_note`.
#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct SetNoteArgs {
    /// GlobalId of the element.
    #[schemars(with = "String")]
    id: GlobalId,
    /// The note, one line. Empty: remove the note.
    text: String,
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
        "set_note" => schemars::schema_for!(SetNoteArgs),
        "set_project_field" => schemars::schema_for!(SetProjectFieldArgs),
        "rotate" => schemars::schema_for!(RotateArgs),
        "mirror" => schemars::schema_for!(MirrorArgs),
        "add_sheet" => schemars::schema_for!(AddSheetArgs),
        "set_sheet" => schemars::schema_for!(SetSheetArgs),
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
        "set_note" => set_note(m, parse(args)?),
        "set_project_field" => set_project_field(m, parse(args)?),
        "rotate" => rotate(m, parse(args)?),
        "mirror" => mirror(m, parse(args)?),
        "add_sheet" => add_sheet(m, parse(args)?),
        "set_sheet" => set_sheet(m, parse(args)?),
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

/// One `SetPlacement` per element, each placement mapped by `f` (plan transforms: z untouched).
/// Unknown ids are refused; `+ 0.0` turns `-0.0` into `0.0` (canonical).
fn transform(
    m: &Model,
    ids: &[GlobalId],
    f: impl Fn(&Placement) -> Placement,
) -> Result<Vec<Command>, String> {
    let clean = |v: [f64; 3]| v.map(|x| x + 0.0);
    ids.iter()
        .map(|id| {
            let e = m.elements.get(id).ok_or(format!("no element {id:?}"))?;
            let p = f(&e.placement);
            Ok(Command::SetPlacement {
                id: *id,
                placement: Placement {
                    origin: clean(p.origin),
                    z_axis: clean(p.z_axis),
                    x_axis: clean(p.x_axis),
                },
            })
        })
        .collect()
}

/// cos and sin of an angle in degrees, exact for multiples of 90° (no 6e-17 for cos 90°).
fn cos_sin(deg: f64) -> (f64, f64) {
    let r = deg.rem_euclid(360.0);
    match r {
        0.0 => (1.0, 0.0),
        90.0 => (0.0, 1.0),
        180.0 => (-1.0, 0.0),
        270.0 => (0.0, -1.0),
        _ => {
            let (s, c) = r.to_radians().sin_cos();
            (c, s)
        }
    }
}

/// Placements rotated about the vertical axis through `about` (default: centre of the origins).
fn rotate(m: &Model, a: RotateArgs) -> Result<Vec<Command>, String> {
    if !a.angle_deg.is_finite() || a.angle_deg.abs() > 360.0 {
        return Err(format!("angle_deg: {} is not within ±360", a.angle_deg));
    }
    if let Some(c) = a.about {
        mm("about", &c)?;
    }
    let mut sum = [0.0, 0.0];
    for id in &a.ids {
        let e = m.elements.get(id).ok_or(format!("no element {id:?}"))?;
        sum[0] += e.placement.origin[0];
        sum[1] += e.placement.origin[1];
    }
    // A full turn or none: no command, so no empty undo step.
    if a.angle_deg.rem_euclid(360.0) == 0.0 || a.ids.is_empty() {
        return Ok(vec![]);
    }
    let n = a.ids.len() as f64;
    let [cx, cy] = a.about.unwrap_or([sum[0] / n, sum[1] / n]);
    let (c, s) = cos_sin(a.angle_deg);
    let turn = |v: [f64; 3]| [c * v[0] - s * v[1], s * v[0] + c * v[1], v[2]];
    transform(m, &a.ids, |p| {
        let o = turn([p.origin[0] - cx, p.origin[1] - cy, p.origin[2]]);
        Placement {
            origin: [o[0] + cx, o[1] + cy, o[2]],
            z_axis: turn(p.z_axis),
            x_axis: turn(p.x_axis),
        }
    })
}

/// Placements reflected in a vertical plane. The model stores origin + z and x axes (y = z × x,
/// IFC Axis2Placement3D), so a placement is always right-handed: a true mirror (left-handed)
/// cannot be stored. The origin, x and z axes are reflected, so y = z × x points opposite the
/// reflected y: the element's local y is not mirrored. Elements symmetric in their local y
/// (rectangular columns and beams, walls on their axis) land exactly where a mirror puts them;
/// an asymmetric one (an off-axis wall, a slab profile not symmetric about local x) lands
/// reflected about its own x axis instead.
fn mirror(m: &Model, a: MirrorArgs) -> Result<Vec<Command>, String> {
    mm("axis.point", &a.axis.point)?;
    mm("axis.dir", &a.axis.dir)?;
    let [dx, dy] = a.axis.dir;
    let len = dx.hypot(dy);
    if len == 0.0 {
        return Err("axis.dir must not be zero".into());
    }
    let (ux, uy) = (dx / len, dy / len);
    let [px, py] = a.axis.point;
    // Reflection of a plan vector: 2 (v·u) u − v.
    let refl = |v: [f64; 3]| {
        let k = 2.0 * (v[0] * ux + v[1] * uy);
        [k * ux - v[0], k * uy - v[1], v[2]]
    };
    transform(m, &a.ids, |p| {
        let o = refl([p.origin[0] - px, p.origin[1] - py, p.origin[2]]);
        Placement {
            origin: [o[0] + px, o[1] + py, o[2]],
            z_axis: refl(p.z_axis),
            x_axis: refl(p.x_axis),
        }
    })
}

/// The property a note is kept in. A bare name: the IFC export writes it in `Bimble_Properties`
/// and the import brings it back under the same name (ADR 0032, ADR 0064).
pub const NOTE_KEY: &str = "Note";

/// One `SetProperty` on "Note": the trimmed text, or its removal for an empty one. Unchanged: no
/// command, so no empty undo step.
fn set_note(m: &Model, a: SetNoteArgs) -> Result<Vec<Command>, String> {
    let e = m
        .elements
        .get(&a.id)
        .ok_or(format!("no element {:?}", a.id))?;
    let text = a.text.trim();
    let value = (!text.is_empty()).then(|| text.to_owned());
    if e.properties.get(NOTE_KEY) == value.as_ref() {
        return Ok(Vec::new());
    }
    Ok(vec![Command::SetProperty {
        id: a.id,
        key: NOTE_KEY.into(),
        value,
    }])
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
                "set_note",
                "set_project_field",
                "rotate",
                "mirror",
                "add_sheet",
                "set_sheet"
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

    /// "Maître d'ouvrage" → "maitre_d_ouvrage" (key_of folds accents; punctuation becomes one
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
        assert_eq!(m.project["maitre_d_ouvrage"], "Ville de Lausanne");
        assert!(set(&m, "Ville de Lausanne").unwrap().is_empty());
        let before = m.clone();
        let long = "x".repeat(bimble_core::project::MAX_VALUE + 1);
        assert!(set(&m, &long).is_err());
        assert_eq!(m, before, "model unchanged");
        assert!(run(&m, "set_project_field", json!({"label": "°", "value": "x"})).is_err());
        let cmds = set(&m, "").unwrap();
        History::default().apply(&mut m, cmds).unwrap();
        assert!(!m.project.contains_key("maitre_d_ouvrage"));
        assert!(set(&m, "").unwrap().is_empty());
    }

    fn beam(m: &Model) -> &bimble_core::Element {
        m.elements
            .values()
            .find(|e| e.placement.z_axis == [1.0, 0.0, 0.0] && e.placement.origin[0] > 0.0)
            .unwrap()
    }

    fn placement(c: &Command) -> &Placement {
        match c {
            Command::SetPlacement { placement, .. } => placement,
            _ => panic!("{c:?}"),
        }
    }

    fn no_neg_zero(p: &Placement) {
        for v in p.origin.iter().chain(&p.z_axis).chain(&p.x_axis) {
            assert!(!(*v == 0.0 && v.is_sign_negative()), "-0.0 in {p:?}");
        }
    }

    /// 90° about the beam's own origin: axes turned exactly, origin unchanged, no -0.0.
    #[test]
    fn rotate_beam_about_its_origin() {
        let (m, _) = demo();
        let b = beam(&m);
        let o = b.placement.origin;
        let c = run(
            &m,
            "rotate",
            json!({"ids": [b.id], "angle_deg": 90, "about": [o[0], o[1]]}),
        )
        .unwrap();
        assert_eq!(c.len(), 1);
        let p = placement(&c[0]);
        assert_eq!(p.origin, o);
        assert_eq!(p.z_axis, [0.0, 1.0, 0.0]);
        assert_eq!(p.x_axis, [-1.0, 0.0, 0.0]);
        no_neg_zero(p);
        History::default().apply(&mut m.clone(), c).unwrap();
    }

    /// Two elements, 180° about their centre (the default): they swap origins.
    #[test]
    fn rotate_two_about_their_centre() {
        let (m, _) = demo();
        let mut cols = m.elements.values().filter(|e| {
            e.placement.z_axis == [0.0, 0.0, 1.0] && e.placement.x_axis == [1.0, 0.0, 0.0]
        });
        let (a, b) = (cols.next().unwrap(), cols.next().unwrap());
        let c = run(
            &m,
            "rotate",
            json!({"ids": [a.id, b.id], "angle_deg": -180}),
        )
        .unwrap();
        // Plan origins swap; each keeps its own z.
        let (oa, ob) = (a.placement.origin, b.placement.origin);
        assert_eq!(placement(&c[0]).origin, [ob[0], ob[1], oa[2]]);
        assert_eq!(placement(&c[1]).origin, [oa[0], oa[1], ob[2]]);
        assert_eq!(placement(&c[0]).x_axis, [-1.0, 0.0, 0.0]);
        no_neg_zero(placement(&c[0]));
    }

    #[test]
    fn rotate_zero_and_errors() {
        let (m, _) = demo();
        let id = beam(&m).id;
        assert!(
            run(&m, "rotate", json!({"ids": [id], "angle_deg": 0}))
                .unwrap()
                .is_empty()
        );
        assert!(
            run(&m, "rotate", json!({"ids": [id], "angle_deg": 360}))
                .unwrap()
                .is_empty()
        );
        assert!(run(&m, "rotate", json!({"ids": [id], "angle_deg": 1e300})).is_err());
        assert!(run(&m, "rotate", json!({"ids": [id], "angle_deg": 361})).is_err());
        let gone = GlobalId::new();
        let e = run(&m, "rotate", json!({"ids": [gone], "angle_deg": 90})).unwrap_err();
        assert!(e.contains("no element"), "{e}");
    }

    /// A note is the property "Note": written trimmed, unchanged gives no command, an empty
    /// text removes it, and a text Rust refuses (a line break) fails when applied.
    #[test]
    fn set_note_writes_changes_and_removes() {
        let (mut m, _) = demo();
        let id = beam(&m).id;
        let cmds = run(&m, "set_note", json!({"id": id, "text": "  check cover "})).unwrap();
        assert_eq!(
            cmds,
            vec![Command::SetProperty {
                id,
                key: NOTE_KEY.into(),
                value: Some("check cover".into()),
            }]
        );
        for c in cmds {
            c.apply(&mut m).unwrap();
        }
        assert_eq!(m.elements[&id].properties[NOTE_KEY], "check cover");
        assert!(
            run(&m, "set_note", json!({"id": id, "text": "check cover"}))
                .unwrap()
                .is_empty()
        );
        let cmds = run(&m, "set_note", json!({"id": id, "text": " "})).unwrap();
        assert_eq!(cmds.len(), 1);
        for c in cmds {
            c.apply(&mut m).unwrap();
        }
        assert!(!m.elements[&id].properties.contains_key(NOTE_KEY));
        assert!(
            run(&m, "set_note", json!({"id": id, "text": ""}))
                .unwrap()
                .is_empty()
        );
        let two = run(&m, "set_note", json!({"id": id, "text": "a\nb"})).unwrap();
        assert!(two.into_iter().next().unwrap().apply(&mut m).is_err());
        let e = run(&m, "set_note", json!({"id": GlobalId::new(), "text": "x"})).unwrap_err();
        assert!(e.contains("no element"), "{e}");
        assert!(describe("set_note").is_some());
    }

    /// Mirror in the plane x = 0 (dir +Y): origin x negated, the beam's axis reversed, and the
    /// placement still valid (right-handed).
    #[test]
    fn mirror_beam_in_x0() {
        let (m, _) = demo();
        let b = beam(&m);
        let o = b.placement.origin;
        let c = run(
            &m,
            "mirror",
            json!({"ids": [b.id], "axis": {"point": [0, 0], "dir": [0, 5]}}),
        )
        .unwrap();
        let p = placement(&c[0]);
        assert_eq!(p.origin, [-o[0], o[1], o[2]]);
        assert_eq!(p.z_axis, [-1.0, 0.0, 0.0]);
        assert_eq!(p.x_axis, [0.0, 1.0, 0.0]);
        no_neg_zero(p);
        p.validate().unwrap();
        let e = run(
            &m,
            "mirror",
            json!({"ids": [b.id], "axis": {"point": [0, 0], "dir": [0, 0]}}),
        )
        .unwrap_err();
        assert!(e.contains("dir"), "{e}");
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

    /// add_sheet on the demo: "S-101", then "S-102"; the id survives a canonical round trip;
    /// set_sheet number/paper/revision; unchanged: none; bad id, duplicate number, bad paper:
    /// refused, model unchanged.
    #[test]
    fn add_and_set_sheet() {
        let (mut m, _) = demo();
        let mut h = History::default();
        let cmds = run(&m, "add_sheet", json!({})).unwrap();
        h.apply(&mut m, cmds).unwrap();
        let cmds = run(&m, "add_sheet", json!({"name": "Beams"})).unwrap();
        h.apply(&mut m, cmds).unwrap();
        let mut nums: Vec<_> = m.sheets.values().map(|s| s.number.clone()).collect();
        nums.sort();
        assert_eq!(nums, ["S-101", "S-102"]);
        let id = m.sheets.values().find(|s| s.number == "S-101").unwrap().id;
        let json = serde_json::to_string(&m).unwrap();
        let again: Model = serde_json::from_str(&json).unwrap();
        assert!(again.sheets.contains_key(&id));

        let set = json!({"id": id, "number": "S-200", "paper": "A3", "revision": "B"});
        let cmds = run(&m, "set_sheet", set.clone()).unwrap();
        assert_eq!(cmds.len(), 1);
        h.apply(&mut m, cmds).unwrap();
        let s = &m.sheets[&id];
        assert_eq!((s.number.as_str(), s.paper.as_str()), ("S-200", "A3"));
        assert_eq!(s.fields["revision"], "B");
        assert!(run(&m, "set_sheet", set).unwrap().is_empty());

        let before = m.clone();
        for bad in [
            json!({"id": GlobalId::new(), "number": "X"}),
            json!({"id": id, "number": "S-102"}),
            json!({"id": id, "paper": "B5"}),
            json!({"id": id, "number": " "}),
        ] {
            assert!(run(&m, "set_sheet", bad.clone()).is_err(), "{bad}");
        }
        assert!(run(&m, "add_sheet", json!({"number": "S-102"})).is_err());
        assert_eq!(m, before);
    }
}
