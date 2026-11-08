//! Convert an IFC file into an editable `.bimble` model (ADR 0032).
//!
//! Extruded solids become the parametric generators when their profile is a rectangle
//! (detected back out of Revit's polylines), an `ExtrudedPolygon` when it isn't, and a `Mesh`
//! when the body is explicit geometry (polygonal face sets, faceted breps). Openings are
//! recorded and reported, not cut yet. Everything unsupported is skipped and reported.

use crate::read::{Affine, Budget, Emit, Reader, signed_area};
use crate::step::{self, StepFile};
use bimble_core::{
    Element, Generator, GlobalId, IfcClass, Material, MaterialCategory, Model, Placement, Storey,
};
use std::collections::{BTreeMap, HashMap, HashSet};

/// How the import went: counts and warnings, shown by the CLI after saving.
#[derive(Default, Debug)]
pub struct ImportReport {
    /// Elements written into the model.
    pub elements: usize,
    /// Elements that became a rectangle generator (rect_column, rect_beam, wall, pad_footing).
    pub parametric: usize,
    /// Elements that became an `ExtrudedPolygon` (non-rectangular extrusion).
    pub extruded: usize,
    /// Elements that became an explicit `Mesh`.
    pub meshes: usize,
    /// Imported elements per IFC class.
    pub by_class: BTreeMap<String, usize>,
    /// Openings found on imported elements (kept uncut in this version).
    pub openings: usize,
    /// Products of supported classes with no usable geometry, and other losses.
    pub skipped: BTreeMap<String, usize>,
    pub warnings: Vec<String>,
}

impl ImportReport {
    fn warn(&mut self, w: impl Into<String>) {
        self.warnings.push(w.into());
    }
    fn skip(&mut self, what: impl Into<String>) {
        *self.skipped.entry(what.into()).or_default() += 1;
    }
}

const SUPPORTED: &[(&str, IfcClass)] = &[
    ("IFCCOLUMN", IfcClass::IfcColumn),
    ("IFCBEAM", IfcClass::IfcBeam),
    ("IFCSLAB", IfcClass::IfcSlab),
    ("IFCWALL", IfcClass::IfcWall),
    ("IFCWALLSTANDARDCASE", IfcClass::IfcWall),
    ("IFCWALLEMENTEDEDCASE", IfcClass::IfcWall),
    ("IFCFOOTING", IfcClass::IfcFooting),
];

fn class_of(upper: &str) -> Option<IfcClass> {
    SUPPORTED.iter().find(|(n, _)| *n == upper).map(|(_, c)| *c)
}

/// Read an IFC file (STEP text) into an editable model and a report.
pub fn import_str(text: &str) -> Result<(Model, ImportReport), String> {
    let (file, skipped) = step::parse(text).map_err(|e| format!("parse: {e}"))?;
    if !file.schemas.iter().any(|s| s.starts_with("IFC")) {
        return Err(format!(
            "not an IFC file (schema {:?})",
            file.schemas.join(",")
        ));
    }
    let mut rep = ImportReport::default();
    if skipped > 0 {
        rep.warn(format!("{skipped} malformed entity instance(s) skipped"));
    }

    let mut r = Reader {
        f: &file,
        placements: HashMap::new(),
        warnings: BTreeMap::new(),
        arc_tol: crate::read::ARC_TOLERANCE_MM,
        angle_scale: 1.0,
        map_stack: Vec::new(),
    };
    let scale = r.length_scale_to_mm();
    r.arc_tol = crate::read::ARC_TOLERANCE_MM / scale;
    r.angle_scale = r.plane_angle_to_radians();

    // Storeys: step id → (GlobalId, storey, world placement in file units).
    let mut storeys: BTreeMap<GlobalId, Storey> = BTreeMap::new();
    let mut storey_gid: HashMap<u64, GlobalId> = HashMap::new();
    let mut storey_frame: HashMap<u64, Affine> = HashMap::new();
    let mut storey_ids: HashSet<u64> = HashSet::new();
    for (id, st) in file.by_type("IFCBUILDINGSTOREY") {
        let name = st.arg(2).as_str().unwrap_or("").to_owned();
        let (elevation, frame) = match st.arg(5).as_ref() {
            Some(pl) => {
                let f = r.object_placement(pl, 0);
                (f.t[2] * scale, f)
            }
            None => (st.arg(9).as_f64().unwrap_or(0.0) * scale, Affine::IDENTITY),
        };
        let raw = st.arg(0).as_str().unwrap_or("");
        let gid = GlobalId::try_from(raw.to_owned()).unwrap_or_else(|_| {
            rep.warn(format!(
                "storey {name:?}: invalid GlobalId; a new one was derived"
            ));
            GlobalId::derived(format!("storey/{id}").as_bytes())
        });
        storeys.insert(
            gid,
            Storey {
                id: gid,
                name,
                elevation,
            },
        );
        storey_gid.insert(id, gid);
        storey_frame.insert(id, frame);
        storey_ids.insert(id);
    }
    if storeys.is_empty() {
        rep.warn("no IfcBuildingStorey in the file; elements go into a synthetic storey");
        let gid = GlobalId::derived(b"storey/synthetic");
        storeys.insert(
            gid,
            Storey {
                id: gid,
                name: "(no storey)".into(),
                elevation: 0.0,
            },
        );
    }
    let fallback_storey = *storeys.keys().next().unwrap();

    let contained = contained_storeys(&file);
    let materials_of = r.materials();
    let openings = r.voids();

    let mut model = Model::new("Imported model");
    model.storeys = storeys;
    let mut model_materials: BTreeMap<GlobalId, Material> = BTreeMap::new();
    let mut material_id: BTreeMap<(String, MaterialCategory), GlobalId> = BTreeMap::new();

    for (id, e) in file.entities.iter() {
        let Some(class) = class_of(&e.name) else {
            continue;
        };
        let name = e.arg(2).as_str().unwrap_or("").to_owned();
        let Ok(gid) = GlobalId::try_from(e.arg(0).as_str().unwrap_or("").to_owned()) else {
            rep.skip(format!("{}: invalid GlobalId", class.name()));
            continue;
        };

        // Which storey (and its frame) this element hangs from.
        let storey_step = contained.storey_of(*id, &storey_ids);
        let (storey, storey_elevation, world) = match storey_step {
            Some(s) => (
                storey_gid[&s],
                model.storeys[&storey_gid[&s]].elevation,
                storey_frame[&s],
            ),
            None => (
                fallback_storey,
                model.storeys[&fallback_storey].elevation,
                {
                    // The synthetic or first storey: elements keep world coordinates minus elevation.
                    Affine::IDENTITY
                },
            ),
        };

        // The element's placement relative to its storey (file units).
        let element_world = match e.arg(5).as_ref() {
            Some(pl) => r.object_placement(pl, 0),
            None => Affine::IDENTITY,
        };
        let rel = storey_inverse(&world, &element_world);

        if let Some(ops) = openings.get(id) {
            rep.openings += ops.len();
        }

        // First: an extruded body → parametric rectangle, ExtrudedPolygon.
        let element = match extruded_solid(&file, e).and_then(|solid| {
            extruded_element(&mut r, solid, &rel, class, gid, &name, storey, scale)
        }) {
            Some((el, parametric)) => {
                if parametric {
                    rep.parametric += 1;
                } else {
                    rep.extruded += 1;
                }
                el
            }
            None => {
                // Otherwise: explicit geometry → tessellate into a Mesh (storey-relative mm).
                let budget = Budget::new(&Default::default());
                let mut mb = bimble_geom::MeshBuilder::default();
                {
                    let mut emit = Emit {
                        mb: &mut mb,
                        scale,
                        budget: &budget,
                    };
                    r.product_body(e, &mut emit);
                }
                if mb.tris.is_empty() {
                    rep.skip(format!("{}: no usable geometry", class.name()));
                    continue;
                }
                let (positions, indices) = mesh_from_tris(&mb.tris, storey_elevation);
                rep.meshes += 1;
                Element {
                    id: gid,
                    class,
                    name,
                    storey,
                    material: GlobalId::derived(b"material/none"),
                    placement: Placement::at(0.0, 0.0, 0.0),
                    generator: Generator::Mesh { positions, indices },
                }
            }
        };

        // Material (name → deterministic id, category from the grade).
        let (mat_name, cat) = materials_of
            .get(id)
            .cloned()
            .unwrap_or_else(|| (String::new(), MaterialCategory::Other));
        let mat_name = if mat_name.is_empty() {
            "Undefined".to_owned()
        } else {
            mat_name
        };
        let mid = *material_id
            .entry((mat_name.clone(), cat))
            .or_insert_with(|| GlobalId::derived(format!("material/{mat_name}").as_bytes()));
        model_materials.entry(mid).or_insert(Material {
            id: mid,
            name: mat_name,
            category: cat,
            props: Default::default(),
        });

        *rep.by_class.entry(class.name().to_owned()).or_default() += 1;
        rep.elements += 1;
        model.elements.insert(
            gid,
            Element {
                material: mid,
                ..element
            },
        );
    }
    model.materials = model_materials;

    // Reader warnings (units, placements…) go into the report too, with counts.
    for (k, n) in r.warnings {
        rep.warn(if n > 1 { format!("{k} (×{n})") } else { k });
    }
    if rep.elements == 0 {
        return Err(
            "no importable elements (IfcColumn, IfcBeam, IfcSlab, IfcWall, IfcFooting) found"
                .into(),
        );
    }
    Ok((model, rep))
}

/// Element step id → storey step id (containment, then aggregation up the spatial tree).
struct ContainedStoreys {
    direct: HashMap<u64, u64>,
    parent: HashMap<u64, u64>,
}

impl ContainedStoreys {
    fn storey_of(&self, el: u64, storeys: &HashSet<u64>) -> Option<u64> {
        let mut cur = *self.direct.get(&el)?;
        if storeys.contains(&cur) {
            return Some(cur);
        }
        for _ in 0..8 {
            match self.parent.get(&cur) {
                Some(&p) => cur = p,
                None => break,
            }
            if storeys.contains(&cur) {
                return Some(cur);
            }
        }
        None
    }
}

fn contained_storeys(file: &StepFile) -> ContainedStoreys {
    let mut direct = HashMap::new();
    for (_, rel) in file.by_type("IFCRELCONTAINEDINSPATIALSTRUCTURE") {
        let Some(container) = rel.arg(5).as_ref() else {
            continue;
        };
        for el in rel.arg(4).as_list().unwrap_or(&[]) {
            if let Some(id) = el.as_ref() {
                direct.insert(id, container);
            }
        }
    }
    let mut parent = HashMap::new();
    for (_, rel) in file.by_type("IFCRELAGGREGATES") {
        let Some(p) = rel.arg(4).as_ref() else {
            continue;
        };
        for c in rel.arg(5).as_list().unwrap_or(&[]) {
            if let Some(c) = c.as_ref() {
                parent.insert(c, p);
            }
        }
    }
    ContainedStoreys { direct, parent }
}

/// rel = storey_world⁻¹ ∘ element_world, assuming the storey frame is orthogonal (the usual
/// case: a placement under the building). rel maps the element's local space to storey space.
fn storey_inverse(storey: &Affine, element: &Affine) -> Affine {
    // Inverse of an orthogonal frame: rotate = transpose of the column frame.
    let tr = |v: [f64; 3]| [dot3(storey.x, v), dot3(storey.y, v), dot3(storey.z, v)];
    Affine {
        x: tr(element.x),
        y: tr(element.y),
        z: tr(element.z),
        t: tr(sub_v(element.t, storey.t)),
    }
}

fn unit(v: [f64; 3]) -> Option<[f64; 3]> {
    let n = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    (n > 1e-12 && n.is_finite()).then(|| v.map(|c| c / n))
}

fn dot3(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn sub_v(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// The first IfcExtrudedAreaSolid of a product's Body representation, following mapped items.
fn extruded_solid(file: &StepFile, e: &crate::step::Entity) -> Option<crate::step::Entity> {
    let shape = file.deref(e.arg(6))?;
    for rep in shape.arg(2).as_list().unwrap_or(&[]) {
        let rep = file.deref(rep)?;
        for item in rep.arg(3).as_list().unwrap_or(&[]) {
            let Some(mut cur) = item.as_ref().and_then(|i| file.get(i)).cloned() else {
                continue;
            };
            // Follow IfcMappedItem → representation map → its first item, a few levels.
            for _ in 0..4 {
                match cur.name.as_str() {
                    "IFCEXTRUDEDAREASOLID" => return Some(cur),
                    "IFCMAPPEDITEM" => {
                        let map = file.get(cur.arg(0).as_ref()?)?;
                        let inner = map.arg(1).as_ref()?;
                        let rep = file.get(inner)?;
                        let first = rep.arg(3).as_list()?.first()?.as_ref()?;
                        cur = file.get(first)?.clone();
                    }
                    _ => break,
                }
            }
        }
    }
    None
}

/// Build an element from an extruded solid. Returns (element, was_rectangular). `None` means
/// "not expressible as an extrusion" — the caller falls back to a Mesh.
#[allow(clippy::too_many_arguments)]
fn extruded_element(
    r: &mut Reader,
    solid: crate::step::Entity,
    rel: &Affine,
    class: IfcClass,
    gid: GlobalId,
    name: &str,
    storey: GlobalId,
    scale: f64,
) -> Option<(Element, bool)> {
    let mut loops = r.profile(solid.arg(0), 0)?;
    if loops.is_empty() || loops[0].len() < 3 {
        return None;
    }
    let sp = r.axis_placement(solid.arg(1));
    let dir = r.direction(solid.arg(2))?;
    let depth = solid.arg(3).as_f64()?;
    if !depth.is_finite() || depth <= 0.0 {
        return None;
    }
    let full = rel.then(&sp);

    // The frame must be orthogonal (proper IFC); skewed frames fall back to a Mesh.
    for (a, b) in [(full.x, full.y), (full.x, full.z), (full.y, full.z)] {
        let (la, lb) = (dot3(a, a).sqrt(), dot3(b, b).sqrt());
        if la < 1e-12 || lb < 1e-12 || dot3(a, b) / (la * lb) > 1e-6 {
            return None;
        }
    }

    // The extrusion direction is defined in the solid's own frame, and the placement frame is
    // that same frame: it must run along ±z (the IFC norm). Anything else falls back to a Mesh.
    if dir[0].hypot(dir[1]) > 1e-6 {
        return None;
    }

    // Downward extrusion: turn the frame 180° about local X so the extrusion runs along +Z.
    // Profile v flips; winding is re-normalised below.
    let flip = dir[2] < 0.0;
    let frame = if flip {
        Affine {
            x: full.x,
            y: full.y.map(|c| -c),
            z: full.z.map(|c| -c),
            t: full.t,
        }
    } else {
        full
    };
    if flip {
        for l in &mut loops {
            for p in l.iter_mut() {
                p[1] = -p[1];
            }
        }
    }

    // Winding: outer CCW, holes CW.
    if signed_area(&loops[0]) < 0.0 {
        for l in &mut loops {
            l.reverse();
        }
    }
    for hole in loops.iter_mut().skip(1) {
        if signed_area(hole) > 0.0 {
            hole.reverse();
        }
    }

    let (placement, generator, parametric) = generator_for(class, &frame, &loops, depth, scale)?;
    Some((
        Element {
            id: gid,
            class,
            name: name.to_owned(),
            storey,
            material: GlobalId::derived(b"material/none"),
            placement,
            generator,
        },
        parametric,
    ))
}

/// Pick the generator for an extruded solid, detecting rectangles back into parametric kinds.
/// The frame is orthogonal, in file units; loops are CCW (holes CW) in the frame's XY.
fn generator_for(
    class: IfcClass,
    frame: &Affine,
    loops: &[Vec<[f64; 2]>],
    depth: f64,
    scale: f64,
) -> Option<(Placement, Generator, bool)> {
    let outer = &loops[0];
    let rect = rectangle(outer);
    let z = unit(frame.z)?;
    // The profile's 2D point p maps into the frame as frame.t + frame.x·p0 + frame.y·p1.
    let to_world = |p: [f64; 2]| {
        [
            frame.t[0] + frame.x[0] * p[0] + frame.y[0] * p[1],
            frame.t[1] + frame.x[1] * p[0] + frame.y[1] * p[1],
            frame.t[2] + frame.x[2] * p[0] + frame.y[2] * p[1],
        ]
    };
    let scaled_origin = |c: [f64; 2]| to_world(c).map(|v| v * scale);

    let scale_pt = |p: [f64; 2]| [p[0] * scale, p[1] * scale];
    let (placement, generator, parametric) = match (class, &rect) {
        (IfcClass::IfcColumn, Some(rc)) => (
            Placement {
                origin: scaled_origin(rc.centre),
                z_axis: z,
                x_axis: unit(frame.x)?,
            },
            Generator::RectColumn {
                width: rc.w * scale,
                depth: rc.h * scale,
                height: depth * scale,
            },
            true,
        ),
        (IfcClass::IfcBeam, Some(rc)) => (
            Placement {
                origin: scaled_origin(rc.centre),
                z_axis: z,
                x_axis: unit(frame.x)?,
            },
            Generator::RectBeam {
                width: rc.w * scale,
                height: rc.h * scale,
                length: depth * scale,
            },
            true,
        ),
        (IfcClass::IfcFooting, Some(rc)) => (
            Placement {
                origin: scaled_origin(rc.centre),
                z_axis: z,
                x_axis: unit(frame.x)?,
            },
            Generator::PadFooting {
                width: rc.w * scale,
                depth: rc.h * scale,
                height: depth * scale,
            },
            true,
        ),
        (IfcClass::IfcWall, Some(rc)) => {
            // Wall profile starts at the origin and runs along +X, centred on its thickness:
            // origin at the middle of one short end, x along the long side.
            let (length, thickness, along_x) = if rc.w >= rc.h {
                (rc.w, rc.h, true)
            } else {
                (rc.h, rc.w, false)
            };
            let u = if along_x { [1.0, 0.0] } else { [0.0, 1.0] };
            let start = [
                rc.centre[0] - u[0] * length / 2.0,
                rc.centre[1] - u[1] * length / 2.0,
            ];
            let x3 = unit([
                frame.x[0] * u[0] + frame.y[0] * u[1],
                frame.x[1] * u[0] + frame.y[1] * u[1],
                frame.x[2] * u[0] + frame.y[2] * u[1],
            ])?;
            (
                Placement {
                    origin: scaled_origin(start),
                    z_axis: z,
                    x_axis: x3,
                },
                Generator::Wall {
                    length: length * scale,
                    thickness: thickness * scale,
                    height: depth * scale,
                },
                true,
            )
        }
        (IfcClass::IfcSlab, _) if loops.len() == 1 => (
            Placement {
                origin: frame.t.map(|c| c * scale),
                z_axis: z,
                x_axis: unit(frame.x)?,
            },
            Generator::Slab {
                outline: outer.iter().map(|p| scale_pt(*p)).collect(),
                thickness: depth * scale,
            },
            rect.is_some(),
        ),
        (_, _) => (
            Placement {
                origin: frame.t.map(|c| c * scale),
                z_axis: z,
                x_axis: unit(frame.x)?,
            },
            Generator::ExtrudedPolygon {
                outline: outer.iter().map(|p| scale_pt(*p)).collect(),
                voids: loops[1..]
                    .iter()
                    .map(|v| v.iter().map(|p| scale_pt(*p)).collect())
                    .collect(),
                height: depth * scale,
            },
            false,
        ),
    };
    Some((placement, generator, parametric))
}

/// A 4-point rectangle: right angles, equal opposite sides. Centre and side lengths
/// (w along the first edge, h along the second).
struct Rect {
    centre: [f64; 2],
    w: f64,
    h: f64,
}

fn rectangle(loop_: &[[f64; 2]]) -> Option<Rect> {
    if loop_.len() != 4 {
        return None;
    }
    let mut edges = [[0.0; 2]; 4];
    for i in 0..4 {
        let a = loop_[i];
        let b = loop_[(i + 1) % 4];
        edges[i] = [b[0] - a[0], b[1] - a[1]];
    }
    let dot2 = |a: [f64; 2], b: [f64; 2]| a[0] * b[0] + a[1] * b[1];
    let len = |e: [f64; 2]| e[0].hypot(e[1]);
    for i in 0..4 {
        let (a, b) = (edges[i], edges[(i + 1) % 4]);
        if len(a) < 1e-9 || len(b) < 1e-9 || (dot2(a, b) / (len(a) * len(b))).abs() > 1e-6 {
            return None;
        }
    }
    // Opposite edges the same length.
    for i in 0..2 {
        let (a, b) = (edges[i], edges[(i + 2) % 4]);
        if (len(a) - len(b)).abs() > 1e-6 * len(a).max(len(b)) {
            return None;
        }
    }
    let centre = [
        loop_.iter().map(|p| p[0]).sum::<f64>() / 4.0,
        loop_.iter().map(|p| p[1]).sum::<f64>() / 4.0,
    ];
    Some(Rect {
        centre,
        w: len(edges[0]),
        h: len(edges[1]),
    })
}

/// Tessellated world-space triangles (mm) → a Mesh generator's data, storey-relative,
/// positions deduplicated in first-use order.
fn mesh_from_tris(tris: &[[[f64; 3]; 3]], storey_elevation: f64) -> (Vec<[f64; 3]>, Vec<u32>) {
    let mut positions: Vec<[f64; 3]> = Vec::new();
    let mut index: HashMap<[u64; 3], u32> = HashMap::new();
    let mut indices: Vec<u32> = Vec::with_capacity(tris.len() * 3);
    for t in tris {
        for p in t {
            let p = [p[0], p[1], p[2] - storey_elevation];
            let next = positions.len() as u32;
            let i = *index.entry(p.map(|c| c.to_bits())).or_insert(next);
            if i == next {
                positions.push(p);
            }
            indices.push(i);
        }
    }
    (positions, indices)
}
