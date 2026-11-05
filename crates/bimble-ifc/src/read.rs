//! Read an IFC file into a render-ready `Scene` (display only, no editing yet).
//!
//! Supported body geometry: IfcExtrudedAreaSolid (arbitrary profiles with voids, polylines,
//! indexed poly curves with arcs, rectangle, circle, I-shape), IfcPolygonalFaceSet,
//! IfcTriangulatedFaceSet, IfcFacetedBrep / shell-based surface models, IfcMappedItem.
//! Boolean results are drawn as their first operand (approximation, reported as a warning).
//! Openings are not cut yet. Everything else is skipped and reported.

use crate::step::{self, Entity, StepFile, Value};
use bimble_core::MaterialCategory;
use bimble_geom::scene::{cross, dot, norm, sub};
use bimble_geom::{ElementInfo, MeshBuilder, Scene, SceneBuilder};
use std::collections::{BTreeMap, HashMap};

pub const MAX_FILE_BYTES: u64 = 2 * 1024 * 1024 * 1024;
const MAX_NESTING: usize = 32;
/// Maximum distance between a tessellated arc and the true arc.
pub const ARC_TOLERANCE_MM: f64 = 0.5;

#[derive(Debug)]
pub enum ReadError {
    Io(std::io::Error),
    Parse(step::ParseError),
    TooLarge(u64),
    NotIfc(String),
}

impl std::fmt::Display for ReadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ReadError::Io(e) => write!(f, "io: {e}"),
            ReadError::Parse(e) => write!(f, "parse: {e}"),
            ReadError::TooLarge(n) => write!(f, "file too large: {n} bytes"),
            ReadError::NotIfc(s) => write!(f, "not an IFC file (schema {s:?})"),
        }
    }
}

impl std::error::Error for ReadError {}

pub fn read_file(path: impl AsRef<std::path::Path>) -> Result<Scene, ReadError> {
    let len = std::fs::metadata(path.as_ref())
        .map_err(ReadError::Io)?
        .len();
    if len > MAX_FILE_BYTES {
        return Err(ReadError::TooLarge(len));
    }
    let bytes = std::fs::read(path).map_err(ReadError::Io)?;
    read_str(&String::from_utf8_lossy(&bytes))
}

/// Caps that keep a malicious or broken file from exhausting memory or time. Mapped items can
/// nest (a map inside a map...), so a tiny file can otherwise expand exponentially.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// Stop adding geometry after this many triangles (whole file).
    pub max_triangles: usize,
    /// Stop after visiting this many representation items (whole file).
    pub max_item_visits: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            max_triangles: 10_000_000,
            max_item_visits: 5_000_000,
        }
    }
}

pub fn read_str(text: &str) -> Result<Scene, ReadError> {
    read_str_with_limits(text, Limits::default())
}

pub fn read_str_with_limits(text: &str, limits: Limits) -> Result<Scene, ReadError> {
    let (file, skipped) = step::parse(text).map_err(ReadError::Parse)?;
    if !file.schemas.iter().any(|s| s.starts_with("IFC")) {
        return Err(ReadError::NotIfc(file.schemas.join(",")));
    }
    let mut r = Reader {
        f: &file,
        placements: HashMap::new(),
        warnings: BTreeMap::new(),
        arc_tol: 0.5,
        item_visits_left: limits.max_item_visits,
        map_stack: Vec::new(),
    };
    let mut budget = Budget {
        triangles_left: limits.max_triangles,
        exhausted: false,
    };
    if skipped > 0 {
        r.warn(format!("{skipped} malformed entity instance(s) skipped"));
    }
    let scale = r.length_scale_to_mm();
    // Curves are tessellated so chords deviate at most 0.5 mm from the true arc.
    r.arc_tol = ARC_TOLERANCE_MM / scale;
    let containers = r.containers();
    let materials = r.materials();

    let mut ids: Vec<u64> = file.entities.keys().copied().collect();
    ids.sort_unstable();
    let mut sb = SceneBuilder::default();
    for id in ids {
        let e = &file.entities[&id];
        if SKIP_CLASSES.contains(&e.name.as_str()) {
            continue;
        }
        let (Some(pl), Some(shape)) = (file.deref(e.arg(5)), file.deref(e.arg(6))) else {
            continue;
        };
        if shape.name != "IFCPRODUCTDEFINITIONSHAPE" || !pl.name.ends_with("PLACEMENT") {
            continue;
        }
        let placement = r.object_placement(e.arg(5).as_ref().unwrap(), 0);
        let mut mb = MeshBuilder::default();
        let mut emit = Emit {
            mb: &mut mb,
            scale,
            budget: &mut budget,
        };
        for rep in shape.arg(2).as_list().unwrap_or(&[]) {
            let Some(rep) = file.deref(rep) else { continue };
            if !is_body(rep) {
                continue;
            }
            for item in rep.arg(3).as_list().unwrap_or(&[]) {
                if let Some(item_id) = item.as_ref() {
                    r.item(item_id, &placement, &mut emit, 0);
                }
            }
        }
        let class = class_name(&e.name);
        let (material, mut category) = materials
            .get(&id)
            .cloned()
            .unwrap_or((String::new(), MaterialCategory::Other));
        if material.is_empty() && e.name == "IFCGEOGRAPHICELEMENT" {
            category = MaterialCategory::Earth;
        }
        sb.push(
            ElementInfo {
                global_id: e.arg(0).as_str().unwrap_or("").to_owned(),
                class,
                name: e.arg(2).as_str().unwrap_or("").to_owned(),
                storey: containers.get(&id).cloned().unwrap_or_default(),
                material,
                category,
            },
            mb,
        );
    }
    if budget.exhausted {
        r.warn(format!(
            "model too large or nested too deeply: stopped after {} triangles / {} items; the rest is not shown",
            limits.max_triangles - budget.triangles_left,
            limits.max_item_visits - r.item_visits_left
        ));
    }
    sb.warnings = r
        .warnings
        .into_iter()
        .map(|(k, n)| if n > 1 { format!("{k} (×{n})") } else { k })
        .collect();
    Ok(sb.finish())
}

const SKIP_CLASSES: &[&str] = &[
    "IFCOPENINGELEMENT",
    "IFCOPENINGSTANDARDCASE",
    "IFCSPACE",
    "IFCSITE",
    "IFCBUILDING",
    "IFCBUILDINGSTOREY",
    "IFCVIRTUALELEMENT",
    "IFCANNOTATION",
    "IFCGRID",
    "IFCSPATIALZONE",
    "IFCEXTERNALSPATIALELEMENT",
];

fn is_body(rep: &Entity) -> bool {
    let id = rep.arg(1).as_str().unwrap_or("");
    let ty = rep.arg(2).as_str().unwrap_or("");
    match id {
        "Body" | "Facetation" => true,
        "" => !matches!(
            ty,
            "Curve2D" | "Curve3D" | "GeometricCurveSet" | "Annotation2D" | "BoundingBox" | "Point"
        ),
        _ => false,
    }
}

const CLASS_NAMES: &[&str] = &[
    "IfcBeam",
    "IfcBeamStandardCase",
    "IfcColumn",
    "IfcColumnStandardCase",
    "IfcWall",
    "IfcWallStandardCase",
    "IfcWallElementedCase",
    "IfcSlab",
    "IfcSlabStandardCase",
    "IfcSlabElementedCase",
    "IfcFooting",
    "IfcPile",
    "IfcRoof",
    "IfcStair",
    "IfcStairFlight",
    "IfcRamp",
    "IfcRampFlight",
    "IfcRailing",
    "IfcDoor",
    "IfcWindow",
    "IfcCovering",
    "IfcCurtainWall",
    "IfcPlate",
    "IfcPlateStandardCase",
    "IfcMember",
    "IfcMemberStandardCase",
    "IfcBuildingElementProxy",
    "IfcBuildingElementPart",
    "IfcReinforcingBar",
    "IfcReinforcingMesh",
    "IfcTendon",
    "IfcTendonAnchor",
    "IfcMechanicalFastener",
    "IfcFastener",
    "IfcDiscreteAccessory",
    "IfcFurnishingElement",
    "IfcFurniture",
    "IfcSystemFurnitureElement",
    "IfcFlowSegment",
    "IfcFlowTerminal",
    "IfcFlowFitting",
    "IfcDistributionElement",
    "IfcGeographicElement",
    "IfcElementAssembly",
    "IfcChimney",
    "IfcShadingDevice",
    "IfcTransportElement",
    "IfcEarthworksFill",
    "IfcEarthworksCut",
    "IfcCourse",
    "IfcPavement",
    "IfcKerb",
    "IfcBearing",
    "IfcDeepFoundation",
    "IfcCaissonFoundation",
    "IfcPipeSegment",
    "IfcDuctSegment",
    "IfcCableCarrierSegment",
    "IfcCableSegment",
    "IfcSanitaryTerminal",
    "IfcLightFixture",
];

fn class_name(upper: &str) -> String {
    CLASS_NAMES
        .iter()
        .find(|c| c.eq_ignore_ascii_case(upper))
        .map(|c| (*c).to_owned())
        .unwrap_or_else(|| {
            // Unknown: "IFCFOOBAR" → "IfcFoobar"
            let rest = upper.strip_prefix("IFC").unwrap_or(upper).to_lowercase();
            let mut c = rest.chars();
            format!(
                "Ifc{}{}",
                c.next().map(|x| x.to_ascii_uppercase()).unwrap_or_default(),
                c.as_str()
            )
        })
}

/// Affine transform: columns x, y, z and translation t (file units).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Affine {
    pub x: [f64; 3],
    pub y: [f64; 3],
    pub z: [f64; 3],
    pub t: [f64; 3],
}

impl Affine {
    pub const IDENTITY: Affine = Affine {
        x: [1., 0., 0.],
        y: [0., 1., 0.],
        z: [0., 0., 1.],
        t: [0.; 3],
    };

    pub fn apply(&self, p: [f64; 3]) -> [f64; 3] {
        [0, 1, 2].map(|k| self.x[k] * p[0] + self.y[k] * p[1] + self.z[k] * p[2] + self.t[k])
    }
    pub fn rotate(&self, v: [f64; 3]) -> [f64; 3] {
        [0, 1, 2].map(|k| self.x[k] * v[0] + self.y[k] * v[1] + self.z[k] * v[2])
    }
    /// self ∘ other (apply other first).
    pub fn then(&self, other: &Affine) -> Affine {
        Affine {
            x: self.rotate(other.x),
            y: self.rotate(other.y),
            z: self.rotate(other.z),
            t: self.apply(other.t),
        }
    }
    fn determinant(&self) -> f64 {
        dot(self.x, cross(self.y, self.z))
    }
}

fn normalize(v: [f64; 3]) -> Option<[f64; 3]> {
    let l = norm(v);
    (l > 1e-12 && l.is_finite()).then(|| v.map(|c| c / l))
}

struct Budget {
    triangles_left: usize,
    exhausted: bool,
}

struct Emit<'a> {
    mb: &'a mut MeshBuilder,
    scale: f64,
    budget: &'a mut Budget,
}

impl Emit<'_> {
    /// False once the triangle budget is used up; geometry is then dropped.
    fn has_budget(&mut self) -> bool {
        if self.budget.triangles_left == 0 {
            self.budget.exhausted = true;
        }
        !self.budget.exhausted
    }
    fn charge(&mut self, before: usize) {
        let used = self.mb.tris.len().saturating_sub(before);
        self.budget.triangles_left = self.budget.triangles_left.saturating_sub(used);
    }
    fn polygon(&mut self, outer: &[[f64; 3]], holes: &[Vec<[f64; 3]>]) {
        if !self.has_budget() {
            return;
        }
        let before = self.mb.tris.len();
        let s = self.scale;
        let sc = |l: &[[f64; 3]]| l.iter().map(|p| p.map(|c| c * s)).collect::<Vec<_>>();
        let holes: Vec<_> = holes.iter().map(|h| sc(h)).collect();
        self.mb.add_polygon(&sc(outer), &holes);
        self.charge(before);
    }
    fn triangle(&mut self, t: [[f64; 3]; 3]) {
        if !self.has_budget() {
            return;
        }
        let before = self.mb.tris.len();
        let s = self.scale;
        self.mb.add_triangle(t.map(|p| p.map(|c| c * s)));
        self.charge(before);
    }
}

struct Reader<'a> {
    f: &'a StepFile,
    placements: HashMap<u64, Affine>,
    warnings: BTreeMap<String, usize>,
    /// Arc tessellation tolerance in file units.
    arc_tol: f64,
    item_visits_left: usize,
    /// Representation maps being expanded, to stop a map that contains itself.
    map_stack: Vec<u64>,
}

impl<'a> Reader<'a> {
    fn warn(&mut self, w: impl Into<String>) {
        *self.warnings.entry(w.into()).or_default() += 1;
    }

    fn get(&self, v: &Value) -> Option<&'a Entity> {
        self.f.deref(v)
    }

    // ---------- units ----------

    fn length_scale_to_mm(&mut self) -> f64 {
        let f = self.f;
        let assignment = f
            .by_type("IFCPROJECT")
            .next()
            .and_then(|(_, p)| self.get(p.arg(8)));
        let units = assignment.and_then(|a| a.arg(0).as_list()).unwrap_or(&[]);
        for u in units {
            let Some(u) = self.get(u) else { continue };
            if let Some(s) = self.length_unit_mm(u, 0) {
                return s;
            }
        }
        self.warn("no length unit in project; assuming millimetres");
        1.0
    }

    /// mm per unit if `u` is a length unit.
    fn length_unit_mm(&self, u: &Entity, depth: usize) -> Option<f64> {
        if depth > 4 {
            return None;
        }
        match u.name.as_str() {
            "IFCSIUNIT" if u.arg(1).as_enum() == Some("LENGTHUNIT") => {
                let prefix = match u.arg(2).as_enum() {
                    Some("KILO") => 1e3,
                    Some("HECTO") => 1e2,
                    Some("DECA") => 1e1,
                    Some("DECI") => 1e-1,
                    Some("CENTI") => 1e-2,
                    Some("MILLI") => 1e-3,
                    Some("MICRO") => 1e-6,
                    _ => 1.0,
                };
                Some(1000.0 * prefix)
            }
            "IFCCONVERSIONBASEDUNIT" | "IFCCONVERSIONBASEDUNITWITHOFFSET"
                if u.arg(1).as_enum() == Some("LENGTHUNIT") =>
            {
                let m = self.get(u.arg(3))?;
                let factor = m.arg(0).as_f64()?;
                let base = self.length_unit_mm(self.get(m.arg(1))?, depth + 1)?;
                Some(factor * base)
            }
            _ => None,
        }
    }

    // ---------- relationships ----------

    /// Element id → storey (or other spatial container) name, following aggregation to the parent.
    fn containers(&self) -> HashMap<u64, String> {
        let f = self.f;
        let mut direct: HashMap<u64, String> = HashMap::new();
        for (_, rel) in f.by_type("IFCRELCONTAINEDINSPATIALSTRUCTURE") {
            let name = self
                .get(rel.arg(5))
                .and_then(|s| s.arg(2).as_str())
                .unwrap_or("")
                .to_owned();
            for el in rel.arg(4).as_list().unwrap_or(&[]) {
                if let Some(id) = el.as_ref() {
                    direct.insert(id, name.clone());
                }
            }
        }
        let mut parent: HashMap<u64, u64> = HashMap::new();
        for (_, rel) in f.by_type("IFCRELAGGREGATES") {
            let Some(p) = rel.arg(4).as_ref() else {
                continue;
            };
            for c in rel.arg(5).as_list().unwrap_or(&[]) {
                if let Some(c) = c.as_ref() {
                    parent.insert(c, p);
                }
            }
        }
        let mut out = direct.clone();
        for &child in parent.keys() {
            let mut cur = child;
            for _ in 0..8 {
                if let Some(n) = direct.get(&cur) {
                    out.entry(child).or_insert_with(|| n.clone());
                    break;
                }
                match parent.get(&cur) {
                    Some(&p) => cur = p,
                    None => break,
                }
            }
        }
        out
    }

    /// Element id → (material display name, family). Type-level materials are inherited.
    fn materials(&self) -> HashMap<u64, (String, MaterialCategory)> {
        let f = self.f;
        let mut direct: HashMap<u64, (String, MaterialCategory)> = HashMap::new();
        for (_, rel) in f.by_type("IFCRELASSOCIATESMATERIAL") {
            let Some(m) = self.get(rel.arg(5)) else {
                continue;
            };
            let names = self.material_names(m, 0);
            if names.is_empty() {
                continue;
            }
            let cat = names
                .iter()
                .map(|(n, c)| c.unwrap_or_else(|| MaterialCategory::classify(n)))
                .find(|c| *c != MaterialCategory::Other)
                .unwrap_or(MaterialCategory::Other);
            let mut uniq: Vec<&str> = Vec::new();
            for (n, _) in &names {
                if !uniq.contains(&n.as_str()) {
                    uniq.push(n);
                }
            }
            let display = uniq.join(" / ");
            for o in rel.arg(4).as_list().unwrap_or(&[]) {
                if let Some(o) = o.as_ref() {
                    direct.insert(o, (display.clone(), cat));
                }
            }
        }
        let mut out = direct.clone();
        for (_, rel) in f.by_type("IFCRELDEFINESBYTYPE") {
            let Some(t) = rel.arg(5).as_ref() else {
                continue;
            };
            let Some(m) = direct.get(&t) else { continue };
            for o in rel.arg(4).as_list().unwrap_or(&[]) {
                if let Some(o) = o.as_ref() {
                    out.entry(o).or_insert_with(|| m.clone());
                }
            }
        }
        out
    }

    /// Material names (and IFC4 Category hint) in a material definition, in order.
    fn material_names(&self, m: &Entity, depth: usize) -> Vec<(String, Option<MaterialCategory>)> {
        if depth > 6 {
            return vec![];
        }
        let sub = |v: &Value| {
            self.get(v)
                .map(|e| self.material_names(e, depth + 1))
                .unwrap_or_default()
        };
        let subs = |v: &Value| {
            v.as_list()
                .unwrap_or(&[])
                .iter()
                .flat_map(sub)
                .collect::<Vec<_>>()
        };
        match m.name.as_str() {
            "IFCMATERIAL" => {
                let name = m.arg(0).as_str().unwrap_or("").to_owned();
                let hint = m
                    .arg(2)
                    .as_str()
                    .map(MaterialCategory::classify)
                    .filter(|c| *c != MaterialCategory::Other);
                let from_name = MaterialCategory::classify(&name);
                vec![(
                    name,
                    if from_name == MaterialCategory::Other {
                        hint
                    } else {
                        Some(from_name)
                    },
                )]
            }
            "IFCMATERIALLAYERSETUSAGE"
            | "IFCMATERIALPROFILESETUSAGE"
            | "IFCMATERIALPROFILESETUSAGETAPERING" => sub(m.arg(0)),
            "IFCMATERIALLAYERSET" => subs(m.arg(0)),
            "IFCMATERIALLAYER" | "IFCMATERIALLAYERWITHOFFSETS" => sub(m.arg(0)),
            "IFCMATERIALPROFILESET" | "IFCMATERIALCONSTITUENTSET" => subs(m.arg(2)),
            "IFCMATERIALPROFILE" | "IFCMATERIALPROFILEWITHOFFSETS" | "IFCMATERIALCONSTITUENT" => {
                sub(m.arg(2))
            }
            "IFCMATERIALLIST" => subs(m.arg(0)),
            _ => vec![],
        }
    }

    // ---------- placements ----------

    fn point(&self, v: &Value) -> Option<[f64; 3]> {
        let e = self.get(v)?;
        let c = e.arg(0).as_list()?;
        let g = |i: usize| c.get(i).and_then(Value::as_f64);
        Some([g(0)?, g(1)?, g(2).unwrap_or(0.0)])
    }

    fn direction(&self, v: &Value) -> Option<[f64; 3]> {
        self.point(v).and_then(normalize)
    }

    /// IfcAxis2Placement3D / 2D → Affine.
    fn axis_placement(&self, v: &Value) -> Affine {
        let Some(e) = self.get(v) else {
            return Affine::IDENTITY;
        };
        let t = self.point(e.arg(0)).unwrap_or([0.0; 3]);
        let (z_given, x_given) = match e.name.as_str() {
            "IFCAXIS2PLACEMENT3D" => (self.direction(e.arg(1)), self.direction(e.arg(2))),
            "IFCAXIS2PLACEMENT2D" => (None, self.direction(e.arg(1))),
            _ => (None, None),
        };
        let z = z_given.unwrap_or([0., 0., 1.]);
        let x = x_given
            .and_then(|x| normalize(sub(x, z.map(|c| c * dot(x, z)))))
            .or_else(|| {
                // Default x: world X projected onto the plane, unless Z is (almost exactly) along X.
                let cand = if 1.0 - z[0].abs() > 1e-9 {
                    [1., 0., 0.]
                } else {
                    [0., 1., 0.]
                };
                normalize(sub(cand, z.map(|c| c * dot(cand, z))))
            })
            .unwrap_or([1., 0., 0.]);
        let y = cross(z, x);
        Affine { x, y, z, t }
    }

    fn object_placement(&mut self, id: u64, depth: usize) -> Affine {
        if let Some(a) = self.placements.get(&id) {
            return *a;
        }
        if depth > 64 {
            self.warn("placement chain too deep or cyclic");
            return Affine::IDENTITY;
        }
        let Some(e) = self.f.get(id) else {
            return Affine::IDENTITY;
        };
        let a = match e.name.as_str() {
            "IFCLOCALPLACEMENT" => {
                let rel = self.axis_placement(e.arg(1));
                match e.arg(0).as_ref() {
                    Some(parent) => self.object_placement(parent, depth + 1).then(&rel),
                    None => rel,
                }
            }
            other => {
                self.warn(format!("unsupported placement {other} (used identity)"));
                Affine::IDENTITY
            }
        };
        self.placements.insert(id, a);
        a
    }

    /// IfcCartesianTransformationOperator3D (+ NonUniform) → Affine.
    fn transform_operator(&self, v: &Value) -> Affine {
        let Some(e) = self.get(v) else {
            return Affine::IDENTITY;
        };
        let a1 = self.direction(e.arg(0));
        let a2 = self.direction(e.arg(1));
        let t = self.point(e.arg(2)).unwrap_or([0.0; 3]);
        let s = e.arg(3).as_f64().unwrap_or(1.0);
        let a3 = self.direction(e.arg(4));
        let (s2, s3) = if e.name == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM" {
            (
                e.arg(5).as_f64().unwrap_or(s),
                e.arg(6).as_f64().unwrap_or(s),
            )
        } else {
            (s, s)
        };
        let z = a3.unwrap_or([0., 0., 1.]);
        let x = a1
            .and_then(|x| normalize(sub(x, z.map(|c| c * dot(x, z)))))
            .or_else(|| normalize(sub([1., 0., 0.], z.map(|c| c * z[0]))))
            .unwrap_or([0., 1., 0.]);
        let y = match a2 {
            Some(y) => {
                let y = sub(sub(y, z.map(|c| c * dot(y, z))), x.map(|c| c * dot(y, x)));
                normalize(y).unwrap_or(cross(z, x))
            }
            // IfcSecondProjAxis: project world Y. Unlike cross(z, x) this can give a mirror,
            // which is what the spec (and IfcOpenShell) do.
            None => {
                let v = [0., 1., 0.];
                normalize(sub(
                    sub(v, z.map(|c| c * dot(v, z))),
                    x.map(|c| c * dot(v, x)),
                ))
                .unwrap_or(cross(z, x))
            }
        };
        Affine {
            x: x.map(|c| c * s),
            y: y.map(|c| c * s2),
            z: z.map(|c| c * s3),
            t,
        }
    }

    // ---------- geometry ----------

    fn item(&mut self, id: u64, tf: &Affine, out: &mut Emit, depth: usize) {
        if depth > MAX_NESTING {
            self.warn("representation nesting too deep");
            return;
        }
        if out.budget.exhausted || self.item_visits_left == 0 {
            out.budget.exhausted = true;
            return;
        }
        self.item_visits_left -= 1;
        let Some(e) = self.f.get(id) else { return };
        match e.name.as_str() {
            "IFCEXTRUDEDAREASOLID" => self.extrusion(e, tf, out),
            "IFCPOLYGONALFACESET" => self.polygonal_face_set(e, tf, out),
            "IFCTRIANGULATEDFACESET" | "IFCTRIANGULATEDIRREGULARNETWORK" => {
                self.triangulated_face_set(e, tf, out)
            }
            "IFCFACETEDBREP" | "IFCFACETEDBREPWITHVOIDS" => {
                if let Some(shell) = self.get(e.arg(0)) {
                    self.faces(shell.arg(0), tf, out);
                }
            }
            "IFCSHELLBASEDSURFACEMODEL" | "IFCFACEBASEDSURFACEMODEL" => {
                for s in e.arg(0).as_list().unwrap_or(&[]) {
                    if let Some(shell) = self.get(s) {
                        self.faces(shell.arg(0), tf, out);
                    }
                }
            }
            "IFCMAPPEDITEM" => {
                let Some(map_id) = e.arg(0).as_ref() else {
                    return;
                };
                let Some(map) = self.f.get(map_id) else {
                    return;
                };
                if self.map_stack.contains(&map_id) {
                    self.warn("representation map contains itself (cycle skipped)");
                    return;
                }
                let origin = self.axis_placement(map.arg(0));
                let target = self.transform_operator(e.arg(1));
                let t = tf.then(&target).then(&origin);
                let Some(rep) = self.get(map.arg(1)) else {
                    return;
                };
                self.map_stack.push(map_id);
                for it in rep.arg(3).as_list().unwrap_or(&[]) {
                    if let Some(i) = it.as_ref() {
                        self.item(i, &t, out, depth + 1);
                    }
                }
                self.map_stack.pop();
            }
            "IFCBOOLEANCLIPPINGRESULT" | "IFCBOOLEANRESULT" => {
                self.warn("boolean result drawn without its cut (first operand only)");
                if let Some(first) = e.arg(1).as_ref() {
                    self.item(first, tf, out, depth + 1);
                }
            }
            "IFCSTYLEDITEM" => {}
            other => self.warn(format!("unsupported geometry {other} (skipped)")),
        }
    }

    fn extrusion(&mut self, e: &Entity, tf: &Affine, out: &mut Emit) {
        let Some(loops) = self.profile(e.arg(0), 0) else {
            return;
        };
        let pos = tf.then(&self.axis_placement(e.arg(1)));
        let Some(dir) = self.direction(e.arg(2)) else {
            return;
        };
        let Some(depth) = e.arg(3).as_f64().filter(|d| d.is_finite() && *d > 0.0) else {
            return;
        };
        let d = dir.map(|c| c * depth);
        // Orientation: outer loop CCW, holes CW, in profile coordinates; flip if the extrusion
        // goes "down" in profile space or the placement mirrors.
        let flip = (d[2] < 0.0) != (pos.determinant() < 0.0);
        let mut loops = loops;
        for (i, l) in loops.iter_mut().enumerate() {
            let ccw = signed_area(l) > 0.0;
            if ccw == (i > 0) {
                l.reverse();
            }
            if flip {
                l.reverse();
            }
        }
        let bottom: Vec<Vec<[f64; 3]>> = loops
            .iter()
            .map(|l| l.iter().map(|p| pos.apply([p[0], p[1], 0.0])).collect())
            .collect();
        let top: Vec<Vec<[f64; 3]>> = loops
            .iter()
            .map(|l| {
                l.iter()
                    .map(|p| pos.apply([p[0] + d[0], p[1] + d[1], d[2]]))
                    .collect()
            })
            .collect();
        // Caps: bottom faces against the extrusion, top faces along it.
        let rev = |l: &Vec<[f64; 3]>| l.iter().rev().copied().collect::<Vec<_>>();
        out.polygon(&rev(&bottom[0]), &bottom[1..]);
        out.polygon(&top[0], &top[1..]);
        for (b, t) in bottom.iter().zip(&top) {
            let n = b.len();
            for i in 0..n {
                let j = (i + 1) % n;
                out.triangle([b[i], b[j], t[j]]);
                out.triangle([b[i], t[j], t[i]]);
            }
        }
    }

    /// Profile → loops in profile coordinates (outer first, then holes). Points are 2D.
    fn profile(&mut self, v: &Value, depth: usize) -> Option<Vec<Vec<[f64; 2]>>> {
        let e = self.get(v)?;
        let placed = |r: &Self, loops: Vec<Vec<[f64; 2]>>, pos: &Value| {
            let a = r.axis_placement(pos);
            loops
                .into_iter()
                .map(|l| {
                    l.into_iter()
                        .map(|p| {
                            let q = a.apply([p[0], p[1], 0.0]);
                            [q[0], q[1]]
                        })
                        .collect()
                })
                .collect()
        };
        let loops = match e.name.as_str() {
            "IFCARBITRARYCLOSEDPROFILEDEF" => vec![self.curve(e.arg(2))?],
            "IFCARBITRARYPROFILEDEFWITHVOIDS" => {
                let mut l = vec![self.curve(e.arg(2))?];
                for h in e.arg(3).as_list().unwrap_or(&[]) {
                    if let Some(c) = self.curve(h) {
                        l.push(c);
                    }
                }
                l
            }
            "IFCRECTANGLEPROFILEDEF" => {
                let (x, y) = (e.arg(3).as_f64()? / 2.0, e.arg(4).as_f64()? / 2.0);
                placed(
                    self,
                    vec![vec![[-x, -y], [x, -y], [x, y], [-x, y]]],
                    e.arg(2),
                )
            }
            "IFCCIRCLEPROFILEDEF" => {
                let r = e.arg(3).as_f64()?;
                let n = arc_segments(r, std::f64::consts::TAU, self.arc_tol);
                let c = (0..n)
                    .map(|i| {
                        let a = i as f64 / n as f64 * std::f64::consts::TAU;
                        [r * a.cos(), r * a.sin()]
                    })
                    .collect();
                placed(self, vec![c], e.arg(2))
            }
            "IFCISHAPEPROFILEDEF" => {
                // Without fillets.
                let (w, h, tw, tf) = (
                    e.arg(3).as_f64()? / 2.0,
                    e.arg(4).as_f64()? / 2.0,
                    e.arg(5).as_f64()? / 2.0,
                    e.arg(6).as_f64()?,
                );
                let c = vec![
                    [-w, -h],
                    [w, -h],
                    [w, -h + tf],
                    [tw, -h + tf],
                    [tw, h - tf],
                    [w, h - tf],
                    [w, h],
                    [-w, h],
                    [-w, h - tf],
                    [-tw, h - tf],
                    [-tw, -h + tf],
                    [-w, -h + tf],
                ];
                placed(self, vec![c], e.arg(2))
            }
            "IFCDERIVEDPROFILEDEF" if depth < 4 => {
                let base = self.profile(e.arg(2), depth + 1)?;
                let op = self.transform_operator_2d(e.arg(3));
                base.into_iter()
                    .map(|l| {
                        l.into_iter()
                            .map(|p| {
                                let q = op.apply([p[0], p[1], 0.0]);
                                [q[0], q[1]]
                            })
                            .collect()
                    })
                    .collect()
            }
            other => {
                self.warn(format!("unsupported profile {other} (skipped)"));
                return None;
            }
        };
        let loops: Vec<_> = loops
            .into_iter()
            .filter(|l: &Vec<[f64; 2]>| l.len() >= 3)
            .collect();
        (!loops.is_empty()).then_some(loops)
    }

    fn transform_operator_2d(&self, v: &Value) -> Affine {
        let Some(e) = self.get(v) else {
            return Affine::IDENTITY;
        };
        let x = self.direction(e.arg(0)).unwrap_or([1., 0., 0.]);
        let t = self.point(e.arg(2)).unwrap_or([0.0; 3]);
        let s = e.arg(3).as_f64().unwrap_or(1.0);
        let y = cross([0., 0., 1.], x);
        Affine {
            x: x.map(|c| c * s),
            y: y.map(|c| c * s),
            z: [0., 0., 1.],
            t,
        }
    }

    /// Closed 2D curve → points (without repeating the first).
    fn curve(&mut self, v: &Value) -> Option<Vec<[f64; 2]>> {
        let e = self.get(v)?;
        let mut pts: Vec<[f64; 2]> = match e.name.as_str() {
            "IFCPOLYLINE" => e
                .arg(0)
                .as_list()?
                .iter()
                .filter_map(|p| self.point(p).map(|q| [q[0], q[1]]))
                .collect(),
            "IFCINDEXEDPOLYCURVE" => {
                let list = self.get(e.arg(0))?.arg(0).as_list()?;
                let coords: Vec<[f64; 2]> = list
                    .iter()
                    .filter_map(|c| {
                        let c = c.as_list()?;
                        Some([c.first()?.as_f64()?, c.get(1)?.as_f64()?])
                    })
                    .collect();
                let at = |i: &Value| -> Option<[f64; 2]> {
                    coords.get((i.as_i64()? as usize).checked_sub(1)?).copied()
                };
                match e.arg(1).as_list() {
                    None => coords.clone(),
                    Some(segs) => {
                        let mut out: Vec<[f64; 2]> = Vec::new();
                        for s in segs {
                            let Value::Typed(kind, inner) = s else {
                                continue;
                            };
                            let idx = inner.as_list().unwrap_or(&[]);
                            let p: Vec<[f64; 2]> = idx.iter().filter_map(at).collect();
                            if p.len() != idx.len() || p.is_empty() {
                                continue;
                            }
                            if out.is_empty() {
                                out.push(p[0]);
                            }
                            match kind.as_str() {
                                "IFCARCINDEX" if p.len() == 3 => out.extend(
                                    arc(p[0], p[1], p[2], self.arc_tol).into_iter().skip(1),
                                ),
                                _ => out.extend_from_slice(&p[1..]),
                            }
                        }
                        out
                    }
                }
            }
            other => {
                self.warn(format!("unsupported profile curve {other} (skipped)"));
                return None;
            }
        };
        if pts.len() > 1 && dist2(pts[0], *pts.last().unwrap()) < 1e-18 {
            pts.pop();
        }
        pts.dedup_by(|a, b| dist2(*a, *b) < 1e-18);
        (pts.len() >= 3).then_some(pts)
    }

    fn point_list_3d(&self, v: &Value) -> Vec<[f64; 3]> {
        self.get(v)
            .and_then(|e| e.arg(0).as_list())
            .unwrap_or(&[])
            .iter()
            .filter_map(|c| {
                let c = c.as_list()?;
                Some([
                    c.first()?.as_f64()?,
                    c.get(1)?.as_f64()?,
                    c.get(2)?.as_f64()?,
                ])
            })
            .collect()
    }

    fn polygonal_face_set(&mut self, e: &Entity, tf: &Affine, out: &mut Emit) {
        let coords: Vec<[f64; 3]> = self
            .point_list_3d(e.arg(0))
            .into_iter()
            .map(|p| tf.apply(p))
            .collect();
        let pn: Option<Vec<i64>> = e
            .arg(3)
            .as_list()
            .map(|l| l.iter().filter_map(Value::as_i64).collect());
        let resolve = |i: &Value| -> Option<[f64; 3]> {
            let mut k = i.as_i64()?;
            if let Some(pn) = &pn {
                k = *pn.get((k as usize).checked_sub(1)?)?;
            }
            coords.get((k as usize).checked_sub(1)?).copied()
        };
        let loop_of =
            |v: &Value| -> Option<Vec<[f64; 3]>> { v.as_list()?.iter().map(resolve).collect() };
        for f in e.arg(2).as_list().unwrap_or(&[]) {
            let Some(face) = self.get(f) else { continue };
            let Some(outer) = loop_of(face.arg(0)) else {
                continue;
            };
            let holes: Vec<Vec<[f64; 3]>> = if face.name == "IFCINDEXEDPOLYGONALFACEWITHVOIDS" {
                face.arg(1)
                    .as_list()
                    .unwrap_or(&[])
                    .iter()
                    .filter_map(loop_of)
                    .collect()
            } else {
                vec![]
            };
            out.polygon(&outer, &holes);
        }
    }

    fn triangulated_face_set(&mut self, e: &Entity, tf: &Affine, out: &mut Emit) {
        let coords: Vec<[f64; 3]> = self
            .point_list_3d(e.arg(0))
            .into_iter()
            .map(|p| tf.apply(p))
            .collect();
        let pn: Option<Vec<i64>> = e
            .arg(4)
            .as_list()
            .map(|l| l.iter().filter_map(Value::as_i64).collect());
        let resolve = |k: i64| -> Option<[f64; 3]> {
            let k = match &pn {
                Some(pn) => *pn.get((k as usize).checked_sub(1)?)?,
                None => k,
            };
            coords.get((k as usize).checked_sub(1)?).copied()
        };
        for t in e.arg(3).as_list().unwrap_or(&[]) {
            let Some(t) = t.as_list() else { continue };
            let idx: Option<Vec<[f64; 3]>> =
                t.iter().map(|v| v.as_i64().and_then(resolve)).collect();
            if let Some(p) = idx.filter(|p| p.len() == 3) {
                out.triangle([p[0], p[1], p[2]]);
            }
        }
    }

    /// IfcFace list (faceted B-rep / shells).
    fn faces(&mut self, list: &Value, tf: &Affine, out: &mut Emit) {
        for f in list.as_list().unwrap_or(&[]) {
            let Some(face) = self.get(f) else { continue };
            let mut outer: Option<Vec<[f64; 3]>> = None;
            let mut holes = Vec::new();
            for b in face.arg(0).as_list().unwrap_or(&[]) {
                let Some(bound) = self.get(b) else { continue };
                let Some(lp) = self.get(bound.arg(0)) else {
                    continue;
                };
                if lp.name != "IFCPOLYLOOP" {
                    self.warn(format!("unsupported face loop {} (skipped)", lp.name));
                    continue;
                }
                let mut pts: Vec<[f64; 3]> = lp
                    .arg(0)
                    .as_list()
                    .unwrap_or(&[])
                    .iter()
                    .filter_map(|p| self.point(p).map(|q| tf.apply(q)))
                    .collect();
                if bound.arg(1).as_enum() == Some("F") {
                    pts.reverse();
                }
                if bound.name == "IFCFACEOUTERBOUND" && outer.is_none() {
                    outer = Some(pts);
                } else {
                    holes.push(pts);
                }
            }
            // A face with only IfcFaceBound: the first one is the outer loop.
            if outer.is_none() && !holes.is_empty() {
                outer = Some(holes.remove(0));
            }
            if let Some(o) = outer {
                out.polygon(&o, &holes);
            }
        }
    }
}

fn dist2(a: [f64; 2], b: [f64; 2]) -> f64 {
    (a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)
}

fn signed_area(l: &[[f64; 2]]) -> f64 {
    l.iter()
        .zip(l.iter().cycle().skip(1))
        .map(|(a, b)| a[0] * b[1] - b[0] * a[1])
        .sum::<f64>()
        / 2.0
}

/// Segments needed so that chords stay within `tol` of a circle of radius `r` over `sweep` radians.
fn arc_segments(r: f64, sweep: f64, tol: f64) -> usize {
    let step = if tol > 0.0 && tol < r {
        2.0 * (1.0 - tol / r).acos()
    } else {
        sweep.abs()
    };
    ((sweep.abs() / step.max(1e-3)).ceil() as usize).clamp(8, 1024)
}

/// Points along the circular arc from a through b to c (inclusive), within `tol` of the true arc.
fn arc(a: [f64; 2], b: [f64; 2], c: [f64; 2], tol: f64) -> Vec<[f64; 2]> {
    let d = 2.0 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
    if d.abs() < 1e-12 {
        return vec![a, b, c]; // collinear
    }
    let sq = |p: [f64; 2]| p[0] * p[0] + p[1] * p[1];
    let ux = (sq(a) * (b[1] - c[1]) + sq(b) * (c[1] - a[1]) + sq(c) * (a[1] - b[1])) / d;
    let uy = (sq(a) * (c[0] - b[0]) + sq(b) * (a[0] - c[0]) + sq(c) * (b[0] - a[0])) / d;
    let r = ((a[0] - ux).powi(2) + (a[1] - uy).powi(2)).sqrt();
    let ang = |p: [f64; 2]| (p[1] - uy).atan2(p[0] - ux);
    let tau = std::f64::consts::TAU;
    let (a0, ab, ac) = (
        ang(a),
        (ang(b) - ang(a)).rem_euclid(tau),
        (ang(c) - ang(a)).rem_euclid(tau),
    );
    // Counter-clockwise if b comes before c going CCW from a.
    let sweep = if ab < ac { ac } else { ac - tau };
    let n = arc_segments(r, sweep, tol);
    let mut out: Vec<[f64; 2]> = (0..=n)
        .map(|i| {
            let t = a0 + sweep * i as f64 / n as f64;
            [ux + r * t.cos(), uy + r * t.sin()]
        })
        .collect();
    out[0] = a;
    out[n] = c;
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    #[test]
    fn arc_half_circle() {
        let p = arc([1.0, 0.0], [0.0, 1.0], [-1.0, 0.0], 1e-3);
        assert!(p.len() > 10);
        // Midpoints of chords stay within tolerance of the circle.
        for w in p.windows(2) {
            let m = [(w[0][0] + w[1][0]) / 2.0, (w[0][1] + w[1][1]) / 2.0];
            assert!(1.0 - m[0].hypot(m[1]) <= 1e-3 + 1e-12);
        }
        assert!(
            p.iter()
                .all(|q| (q[0].hypot(q[1]) - 1.0).abs() < 1e-9 && q[1] >= -1e-12)
        );
        let p = arc([1.0, 0.0], [0.0, -1.0], [-1.0, 0.0], 1e-3);
        assert!(p.iter().all(|q| q[1] <= 1e-12));
    }

    /// Our own export, read back by our own reader: same elements, same volumes, same places.
    #[test]
    fn roundtrip_own_export() {
        let m = concrete_frame(&FrameSpec::default());
        let ifc = crate::export(&m, "t.ifc", "t");
        let s = read_str(&ifc).unwrap();
        assert!(s.warnings.is_empty(), "{:?}", s.warnings);
        let native = Scene::from_model(&m);
        assert_eq!(s.elements.len(), native.elements.len());
        let by_id: HashMap<&str, &bimble_geom::SceneElement> = native
            .elements
            .iter()
            .map(|e| (e.global_id.as_str(), e))
            .collect();
        for e in &s.elements {
            let n = by_id[e.global_id.as_str()];
            assert_eq!(e.class, n.class);
            assert_eq!(e.storey, n.storey);
            assert_eq!(e.category, MaterialCategory::Concrete);
            let (v1, v2) = (e.mesh.volume(), n.mesh.volume());
            assert!((v1 - v2).abs() / v2 < 1e-5, "{}: {v1} vs {v2}", e.name);
            // World bounding boxes agree to 0.01 mm.
            let bb = |el: &bimble_geom::SceneElement, o: [f64; 3]| {
                let mut lo = [f64::MAX; 3];
                let mut hi = [f64::MIN; 3];
                for p in &el.mesh.positions {
                    for k in 0..3 {
                        let w = p[k] as f64 + o[k];
                        lo[k] = lo[k].min(w);
                        hi[k] = hi[k].max(w);
                    }
                }
                (lo, hi)
            };
            let (a, b) = (bb(e, s.origin_mm), bb(n, native.origin_mm));
            for k in 0..3 {
                assert!(
                    (a.0[k] - b.0[k]).abs() < 0.01 && (a.1[k] - b.1[k]).abs() < 0.01,
                    "{}",
                    e.name
                );
            }
        }
    }

    #[test]
    fn rejects_non_ifc() {
        assert!(matches!(
            read_str("ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('AP214'));\nENDSEC;\nDATA;\nENDSEC;"),
            Err(ReadError::NotIfc(_))
        ));
    }
}
