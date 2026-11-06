//! Read an IFC file into a render-ready `Scene` (display only, no editing yet).
//!
//! Supported body geometry: IfcExtrudedAreaSolid (arbitrary profiles with voids, polylines,
//! indexed poly curves with arcs, rectangle, circle, I-shape), IfcPolygonalFaceSet,
//! IfcTriangulatedFaceSet, IfcFacetedBrep / shell-based surface models, IfcMappedItem.
//! Boolean results are drawn as their first operand (approximation, reported as a warning).
//! Openings are not cut yet. Everything else is skipped and reported.

use crate::step::{self, Entity, StepFile, Value};
use bimble_core::MaterialCategory;
use bimble_geom::boolean::{self, Op, Tri};
use bimble_geom::scene::{cross, dot, norm, sub};
use bimble_geom::{ElementInfo, MeshBuilder, Scene, SceneBuilder};
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

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
    read_bytes(&bytes)
}

/// Read an IFC file's content (e.g. picked in a browser). Same size cap as `read_file`.
pub fn read_bytes(bytes: &[u8]) -> Result<Scene, ReadError> {
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return Err(ReadError::TooLarge(bytes.len() as u64));
    }
    read_str(&String::from_utf8_lossy(bytes))
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
        angle_scale: 1.0,
        map_stack: Vec::new(),
    };
    let budget = Budget::new(&limits);
    if skipped > 0 {
        r.warn(format!("{skipped} malformed entity instance(s) skipped"));
    }
    let scale = r.length_scale_to_mm();
    // Curves are tessellated so chords deviate at most 0.5 mm from the true arc.
    r.arc_tol = ARC_TOLERANCE_MM / scale;
    r.angle_scale = r.plane_angle_to_radians();
    let containers = r.containers();
    let voids = r.voids();
    let materials = r.materials();

    let mut ids: Vec<u64> = file
        .entities
        .iter()
        .filter(|(_, e)| {
            !SKIP_CLASSES.contains(&e.name.as_str())
                && file
                    .deref(e.arg(6))
                    .is_some_and(|s| s.name == "IFCPRODUCTDEFINITIONSHAPE")
                && file
                    .deref(e.arg(5))
                    .is_some_and(|p| p.name.ends_with("PLACEMENT"))
        })
        .map(|(id, _)| *id)
        .collect();
    ids.sort_unstable();

    // Elements are independent: mesh them on all cores. Each worker has its own placement cache
    // and warnings; budgets are shared. Results keep file order.
    let (arc_tol, angle_scale) = (r.arc_tol, r.angle_scale);
    let results: Vec<(ElementInfo, MeshBuilder, BTreeMap<String, usize>)> = ids
        .par_iter()
        .map_init(
            || Reader {
                f: &file,
                placements: HashMap::new(),
                warnings: BTreeMap::new(),
                arc_tol,
                angle_scale,
                map_stack: Vec::new(),
            },
            |w, &id| {
                let e = &file.entities[&id];
                let mut mb = MeshBuilder::default();
                {
                    let mut emit = Emit {
                        mb: &mut mb,
                        scale,
                        budget: &budget,
                    };
                    w.product_body(e, &mut emit);
                }
                // Openings (IfcRelVoidsElement): subtract them from their host.
                if let Some(ops) = voids.get(&id).filter(|_| !mb.tris.is_empty()) {
                    let mut cutters: Vec<Vec<Tri>> = Vec::new();
                    for &o in ops {
                        let Some(oe) = file.get(o) else { continue };
                        let mut ob = MeshBuilder::default();
                        let mut emit = Emit {
                            mb: &mut ob,
                            scale,
                            budget: &budget,
                        };
                        w.product_body(oe, &mut emit);
                        if !ob.tris.is_empty() {
                            cutters.push(ob.tris);
                        }
                    }
                    w.cut(&mut mb.tris, &cutters, &class_name(&e.name));
                }
                let class = class_name(&e.name);
                let (material, mut category) = materials
                    .get(&id)
                    .cloned()
                    .unwrap_or((String::new(), MaterialCategory::Other));
                if material.is_empty() && e.name == "IFCGEOGRAPHICELEMENT" {
                    category = MaterialCategory::Earth;
                }
                let info = ElementInfo {
                    global_id: e.arg(0).as_str().unwrap_or("").to_owned(),
                    class,
                    name: e.arg(2).as_str().unwrap_or("").to_owned(),
                    storey: containers.get(&id).cloned().unwrap_or_default(),
                    material,
                    category,
                };
                (info, mb, std::mem::take(&mut w.warnings))
            },
        )
        .collect();

    let mut sb = SceneBuilder::with_storeys(r.storey_levels(scale));
    sb.grids = r.grid_axes(scale);
    for (info, mb, warnings) in results {
        for (k, n) in warnings {
            *r.warnings.entry(k).or_default() += n;
        }
        sb.push(info, mb);
    }
    if budget.exhausted() {
        let tris = limits.max_triangles - budget.triangles_left.load(Ordering::Relaxed);
        let items = limits.max_item_visits - budget.visits_left.load(Ordering::Relaxed);
        r.warn(format!(
            "model too large or nested too deeply: stopped after {tris} triangles / {items} items; the rest is not shown"
        ));
    }
    sb.warnings = r
        .warnings
        .into_iter()
        .map(|(k, n)| if n > 1 { format!("{k} (×{n})") } else { k })
        .collect();
    // Freeing millions of parsed values takes a noticeable time: do it off the caller's path.
    // Freeing a big parsed file takes a while: off the caller's thread where there are threads.
    #[cfg(not(target_arch = "wasm32"))]
    std::thread::spawn(move || drop(file));
    #[cfg(target_arch = "wasm32")]
    drop(file);
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
    /// World point → local coordinates (axes may be scaled but must be orthogonal).
    pub fn local(&self, p: [f64; 3]) -> [f64; 3] {
        let d = sub(p, self.t);
        [self.x, self.y, self.z].map(|a| dot(d, a) / dot(a, a).max(1e-300))
    }
    fn determinant(&self) -> f64 {
        dot(self.x, cross(self.y, self.z))
    }
}

fn normalize(v: [f64; 3]) -> Option<[f64; 3]> {
    let l = norm(v);
    (l > 1e-12 && l.is_finite()).then(|| v.map(|c| c / l))
}

/// Limits shared by all worker threads: total triangles and representation items visited.
/// Once either runs out, `exhausted` is set and every thread stops adding geometry.
struct Budget {
    triangles_left: AtomicUsize,
    visits_left: AtomicUsize,
    exhausted: AtomicBool,
}

impl Budget {
    fn new(limits: &Limits) -> Self {
        Budget {
            triangles_left: AtomicUsize::new(limits.max_triangles),
            visits_left: AtomicUsize::new(limits.max_item_visits),
            exhausted: AtomicBool::new(false),
        }
    }
    fn exhausted(&self) -> bool {
        self.exhausted.load(Ordering::Relaxed)
    }
    /// Takes one item visit; false (and exhausted) when none are left.
    fn visit(&self) -> bool {
        let ok = !self.exhausted()
            && self
                .visits_left
                .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_sub(1))
                .is_ok();
        if !ok {
            self.exhausted.store(true, Ordering::Relaxed);
        }
        ok
    }
}

struct Emit<'a> {
    mb: &'a mut MeshBuilder,
    scale: f64,
    budget: &'a Budget,
}

impl Emit<'_> {
    /// False once the triangle budget is used up; geometry is then dropped.
    fn has_budget(&mut self) -> bool {
        if self.budget.triangles_left.load(Ordering::Relaxed) == 0 {
            self.budget.exhausted.store(true, Ordering::Relaxed);
        }
        !self.budget.exhausted()
    }
    fn charge(&mut self, before: usize) {
        let used = self.mb.tris.len().saturating_sub(before);
        let _ = self
            .budget
            .triangles_left
            .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| {
                Some(v.saturating_sub(used))
            });
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
        let s = self.scale;
        self.triangle_mm(t.map(|p| p.map(|c| c * s)));
    }
    /// A triangle already in mm (boolean results).
    fn triangle_mm(&mut self, t: [[f64; 3]; 3]) {
        if !self.has_budget() {
            return;
        }
        let before = self.mb.tris.len();
        self.mb.add_triangle(t);
        self.charge(before);
    }
}

struct Reader<'a> {
    f: &'a StepFile,
    placements: HashMap<u64, Affine>,
    warnings: BTreeMap<String, usize>,
    /// Arc tessellation tolerance in file units.
    arc_tol: f64,
    /// Plane angle unit → radians (Revit IFC2x3 often writes degrees).
    angle_scale: f64,
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
    /// Storey names and levels (world mm): the z of the storey's placement, or its Elevation
    /// attribute when it has no placement.
    fn storey_levels(&mut self, scale: f64) -> Vec<bimble_geom::StoreyLevel> {
        let f = self.f;
        let mut out = Vec::new();
        for (_, st) in f.by_type("IFCBUILDINGSTOREY") {
            let name = st.arg(2).as_str().unwrap_or("").to_owned();
            let z = match st.arg(5).as_ref() {
                Some(pl) => Some(self.object_placement(pl, 0).t[2]),
                None => st.arg(9).as_f64(),
            };
            if let Some(z) = z.filter(|z| z.is_finite()) {
                out.push(bimble_geom::StoreyLevel {
                    name,
                    elevation_mm: z * scale,
                });
            }
        }
        out
    }

    /// Axes of every IfcGrid, in plan (world mm): the first and last point of each axis curve
    /// (IfcPolyline or IfcIndexedPolyCurve; other curve types are skipped), placed by the grid's
    /// placement. Axes repeated with the same tag and position (one grid per storey) are kept
    /// once.
    fn grid_axes(&mut self, scale: f64) -> Vec<bimble_geom::GridAxis> {
        const MAX_AXES: usize = 10_000;
        let f = self.f;
        let mut out = Vec::new();
        // In file order, so the result doesn't depend on how the file is indexed.
        let mut grids: Vec<_> = f.by_type("IFCGRID").collect();
        grids.sort_by_key(|(id, _)| *id);
        for (_, g) in grids {
            let place = match g.arg(5).as_ref() {
                Some(pl) => self.object_placement(pl, 0),
                None => Affine::IDENTITY,
            };
            for list in [g.arg(7), g.arg(8), g.arg(9)] {
                for ax in list.as_list().unwrap_or(&[]) {
                    if out.len() >= MAX_AXES {
                        return out;
                    }
                    let Some(ax) = self.get(ax) else { continue };
                    let tag = ax.arg(0).as_str().unwrap_or("").to_owned();
                    let Some(curve) = self.get(ax.arg(1)) else {
                        continue;
                    };
                    let pts: Vec<[f64; 3]> = match curve.name.as_str() {
                        "IFCPOLYLINE" => curve
                            .arg(0)
                            .as_list()
                            .unwrap_or(&[])
                            .iter()
                            .filter_map(|p| self.point(p))
                            .collect(),
                        // Points of an IfcCartesianPointList2D/3D; the segments don't matter
                        // for the end points.
                        "IFCINDEXEDPOLYCURVE" => self
                            .get(curve.arg(0))
                            .and_then(|l| l.arg(0).as_list())
                            .map(|coords| {
                                let at = |c: &Value| {
                                    let c = c.as_list()?;
                                    let g = |i: usize| c.get(i).and_then(Value::as_f64);
                                    Some([g(0)?, g(1)?, g(2).unwrap_or(0.0)])
                                };
                                [coords.first(), coords.last()]
                                    .into_iter()
                                    .flatten()
                                    .filter_map(at)
                                    .collect()
                            })
                            .unwrap_or_default(),
                        _ => Vec::new(),
                    };
                    let (Some(a), Some(b)) = (pts.first(), pts.last()) else {
                        continue;
                    };
                    let (a, b) = (place.apply(*a), place.apply(*b));
                    let xy = |p: [f64; 3]| [p[0] * scale, p[1] * scale];
                    let (a, b) = (xy(a), xy(b));
                    // Grids are often repeated per storey: keep each axis once.
                    let dup = out.iter().any(|g: &bimble_geom::GridAxis| {
                        g.tag == tag && g.start_mm == a && g.end_mm == b
                    });
                    if !dup && a != b && a.iter().chain(&b).all(|v| v.is_finite()) {
                        out.push(bimble_geom::GridAxis {
                            tag,
                            start_mm: a,
                            end_mm: b,
                        });
                    }
                }
            }
        }
        out
    }

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
        if !out.budget.visit() {
            return;
        }
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
            "IFCBOOLEANCLIPPINGRESULT" | "IFCBOOLEANRESULT" => self.boolean(e, tf, out, depth),
            "IFCSWEPTDISKSOLID" | "IFCSWEPTDISKSOLIDPOLYGONAL" => self.swept_disk(e, tf, out),
            "IFCSTYLEDITEM" => {}
            other => self.warn(format!("unsupported geometry {other} (skipped)")),
        }
    }

    /// The Body representation of a product, placed in the world, into `out`.
    fn product_body(&mut self, e: &Entity, out: &mut Emit) {
        let (Some(pl), Some(shape)) = (e.arg(5).as_ref(), self.f.deref(e.arg(6))) else {
            return;
        };
        let placement = self.object_placement(pl, 0);
        for rep in shape.arg(2).as_list().unwrap_or(&[]) {
            let Some(rep) = self.f.deref(rep) else {
                continue;
            };
            if !is_body(rep) {
                continue;
            }
            for item in rep.arg(3).as_list().unwrap_or(&[]) {
                if let Some(item_id) = item.as_ref() {
                    self.item(item_id, &placement, out, 0);
                }
            }
        }
    }

    /// Host element id → its openings (IfcRelVoidsElement).
    fn voids(&self) -> HashMap<u64, Vec<u64>> {
        let mut out: HashMap<u64, Vec<u64>> = HashMap::new();
        for (_, rel) in self.f.by_type("IFCRELVOIDSELEMENT") {
            if let (Some(h), Some(o)) = (rel.arg(4).as_ref(), rel.arg(5).as_ref()) {
                out.entry(h).or_default().push(o);
            }
        }
        for v in out.values_mut() {
            v.sort_unstable();
        }
        out
    }

    /// Subtract cutters from `tris` (mm). If the whole set fails (e.g. one opening isn't a closed
    /// solid), retry one cutter at a time and keep the ones that work.
    fn cut(&mut self, tris: &mut Vec<Tri>, cutters: &[Vec<Tri>], what: &str) {
        if cutters.is_empty() {
            return;
        }
        let refs: Vec<&[Tri]> = cutters.iter().map(Vec::as_slice).collect();
        match boolean::apply(Op::Difference, tris, &refs) {
            Ok(r) => *tris = r,
            Err(_) => {
                let mut failed = 0;
                for c in cutters {
                    match boolean::apply(Op::Difference, tris, &[c]) {
                        Ok(r) => *tris = r,
                        Err(_) => failed += 1,
                    }
                }
                if failed > 0 {
                    self.warn(format!(
                        "{what}: {failed} opening(s) not cut (geometry not a closed solid)"
                    ));
                }
            }
        }
    }

    /// Triangles (mm, world) of one representation item, evaluated on its own.
    fn solid_tris(&mut self, id: u64, tf: &Affine, out: &mut Emit, depth: usize) -> Vec<Tri> {
        let mut mb = MeshBuilder::default();
        {
            let mut tmp = Emit {
                mb: &mut mb,
                scale: out.scale,
                budget: out.budget,
            };
            self.item(id, tf, &mut tmp, depth);
        }
        mb.tris
    }

    /// IfcBooleanResult / IfcBooleanClippingResult (Operator, FirstOperand, SecondOperand).
    fn boolean(&mut self, e: &Entity, tf: &Affine, out: &mut Emit, depth: usize) {
        let op = match e.arg(0).as_enum() {
            Some("UNION") => Op::Union,
            Some("INTERSECTION") => Op::Intersection,
            _ => Op::Difference,
        };
        let Some(first) = e.arg(1).as_ref() else {
            return;
        };
        let a = self.solid_tris(first, tf, out, depth + 1);
        if a.is_empty() {
            return;
        }
        let b = match self.get(e.arg(2)) {
            Some(h)
                if matches!(
                    h.name.as_str(),
                    "IFCHALFSPACESOLID" | "IFCBOXEDHALFSPACE" | "IFCPOLYGONALBOUNDEDHALFSPACE"
                ) =>
            {
                self.half_space(h, tf, &a, out.scale)
            }
            Some(_) => self.solid_tris(e.arg(2).as_ref().unwrap(), tf, out, depth + 1),
            None => Vec::new(),
        };
        match boolean::apply(op, &a, &[&b]) {
            Ok(r) => {
                for t in r {
                    out.triangle_mm(t);
                }
            }
            Err(err) => {
                self.warn(format!("boolean cut failed, drawn uncut ({err})"));
                for t in a {
                    out.triangle_mm(t);
                }
            }
        }
    }

    /// A half-space as a finite solid (mm, world), large enough to contain `operand`.
    /// IfcHalfSpaceSolid(BaseSurface, AgreementFlag): with AgreementFlag TRUE the plane normal
    /// points away from the half-space's material, so the material is on the −normal side.
    fn half_space(&mut self, h: &Entity, tf: &Affine, operand: &[Tri], scale: f64) -> Vec<Tri> {
        let Some(plane) = self.get(h.arg(0)).filter(|p| p.name == "IFCPLANE") else {
            self.warn(format!("{}: unsupported base surface (not cut)", h.name));
            return Vec::new();
        };
        let pos = tf.then(&self.axis_placement(plane.arg(0)));
        let material_below = h.arg(1).as_enum() != Some("F");
        // Size from the operand (file units), centred on its projection onto the plane.
        let pts = operand.iter().flatten().map(|p| p.map(|c| c / scale));
        let (mut lo, mut hi) = ([f64::INFINITY; 3], [f64::NEG_INFINITY; 3]);
        for p in pts {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k]);
                hi[k] = hi[k].max(p[k]);
            }
        }
        let l = 2.0 * norm(sub(hi, lo)) + 1.0;
        let c = pos.local([0, 1, 2].map(|k| (lo[k] + hi[k]) / 2.0));
        let rect = vec![
            [c[0] - l, c[1] - l],
            [c[0] + l, c[1] - l],
            [c[0] + l, c[1] + l],
            [c[0] - l, c[1] + l],
        ];
        let (z0, z1) = if material_below {
            (-l + c[2].min(0.0), 0.0)
        } else {
            (0.0, l + c[2].max(0.0))
        };
        let half = prism_tris(&rect, &pos, z0, z1, scale);
        if h.name != "IFCPOLYGONALBOUNDEDHALFSPACE" {
            return half;
        }
        // IfcPolygonalBoundedHalfSpace(.., Position, PolygonalBoundary): also inside the boundary
        // prism (extruded along Position's Z).
        let ppos = tf.then(&self.axis_placement(h.arg(2)));
        let Some(poly) = self.curve(h.arg(3)) else {
            return Vec::new();
        };
        let cz = ppos.local([0, 1, 2].map(|k| (lo[k] + hi[k]) / 2.0))[2];
        let prism = prism_tris(&poly, &ppos, cz - l, cz + l, scale);
        match boolean::apply(Op::Intersection, &half, &[&prism]) {
            Ok(t) => t,
            Err(err) => {
                self.warn(format!("polygonal half-space failed ({err}); not cut"));
                Vec::new()
            }
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
            "IFCCIRCLEHOLLOWPROFILEDEF" => {
                // (ProfileType, ProfileName, Position, Radius, WallThickness)
                let (r, t) = (e.arg(3).as_f64()?, e.arg(4).as_f64()?);
                let ring = |rad: f64| {
                    let n = arc_segments(r, std::f64::consts::TAU, self.arc_tol);
                    (0..n)
                        .map(|i| {
                            let a = i as f64 / n as f64 * std::f64::consts::TAU;
                            [rad * a.cos(), rad * a.sin()]
                        })
                        .collect::<Vec<_>>()
                };
                let mut loops = vec![ring(r)];
                if t > 0.0 && t < r {
                    loops.push(ring(r - t));
                }
                placed(self, loops, e.arg(2))
            }
            "IFCRECTANGLEHOLLOWPROFILEDEF" => {
                // (ProfileType, ProfileName, Position, XDim, YDim, WallThickness, ..fillets ignored)
                let (x, y, t) = (
                    e.arg(3).as_f64()? / 2.0,
                    e.arg(4).as_f64()? / 2.0,
                    e.arg(5).as_f64()?,
                );
                let rect = |a: f64, b: f64| vec![[-a, -b], [a, -b], [a, b], [-a, b]];
                let mut loops = vec![rect(x, y)];
                if t > 0.0 && t < x.min(y) {
                    loops.push(rect(x - t, y - t));
                }
                placed(self, loops, e.arg(2))
            }
            "IFCISHAPEPROFILEDEF" => {
                // Root fillets between web and flanges (FilletRadius); flange edge radii and
                // slopes are ignored.
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
                let r = e.arg(7).as_f64().filter(|r| r.is_finite() && *r > 0.0);
                let c = match r {
                    // Inner corners where the web meets the flanges.
                    Some(r) => fillet(&c, &[3, 4, 9, 10], r, self.arc_tol),
                    None => c,
                };
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
        let mut pts: Vec<[f64; 2]> = self
            .curve3(v, 0)?
            .into_iter()
            .map(|p| [p[0], p[1]])
            .collect();
        if pts.len() > 1 && dist2(pts[0], *pts.last().unwrap()) < 1e-18 {
            pts.pop();
        }
        pts.dedup_by(|a, b| dist2(*a, *b) < 1e-18);
        (pts.len() >= 3).then_some(pts)
    }

    /// Any bounded curve → points along it (2D curves get z = 0). Arcs are tessellated within
    /// `arc_tol`. Consecutive duplicate points are removed.
    fn curve3(&mut self, v: &Value, depth: usize) -> Option<Vec<[f64; 3]>> {
        if depth > 16 {
            self.warn("curve nesting too deep");
            return None;
        }
        let e = self.get(v)?;
        let mut pts: Vec<[f64; 3]> = match e.name.as_str() {
            "IFCPOLYLINE" => e
                .arg(0)
                .as_list()?
                .iter()
                .filter_map(|p| self.point(p))
                .collect(),
            "IFCINDEXEDPOLYCURVE" => {
                let list = self.get(e.arg(0))?.arg(0).as_list()?;
                let coords: Vec<[f64; 3]> = list
                    .iter()
                    .filter_map(|c| {
                        let c = c.as_list()?;
                        Some([
                            c.first()?.as_f64()?,
                            c.get(1)?.as_f64()?,
                            c.get(2).and_then(Value::as_f64).unwrap_or(0.0),
                        ])
                    })
                    .collect();
                let at = |i: &Value| -> Option<[f64; 3]> {
                    coords.get((i.as_i64()? as usize).checked_sub(1)?).copied()
                };
                match e.arg(1).as_list() {
                    None => coords.clone(),
                    Some(segs) => {
                        let mut out: Vec<[f64; 3]> = Vec::new();
                        for s in segs {
                            let Value::Typed(kind, inner) = s else {
                                continue;
                            };
                            let idx = inner.as_list().unwrap_or(&[]);
                            let p: Vec<[f64; 3]> = idx.iter().filter_map(at).collect();
                            if p.len() != idx.len() || p.is_empty() {
                                continue;
                            }
                            if out.is_empty() {
                                out.push(p[0]);
                            }
                            match kind.as_str() {
                                "IFCARCINDEX" if p.len() == 3 => out.extend(
                                    arc3(p[0], p[1], p[2], self.arc_tol).into_iter().skip(1),
                                ),
                                _ => out.extend_from_slice(&p[1..]),
                            }
                        }
                        out
                    }
                }
            }
            "IFCCOMPOSITECURVE" | "IFCCOMPOSITECURVEONSURFACE" => {
                let mut out: Vec<[f64; 3]> = Vec::new();
                for seg in e.arg(0).as_list()? {
                    // IfcCompositeCurveSegment(Transition, SameSense, ParentCurve)
                    let s = self.get(seg)?;
                    let mut p = self.curve3(s.arg(2), depth + 1)?;
                    if s.arg(1).as_enum() == Some("F") {
                        p.reverse();
                    }
                    if let (Some(last), Some(first)) = (out.last(), p.first())
                        && norm(sub(*last, *first)) < 1e-9
                    {
                        p.remove(0);
                    }
                    out.extend(p);
                }
                out
            }
            "IFCTRIMMEDCURVE" => self.trimmed_curve(e, depth)?,
            "IFCCIRCLE" | "IFCELLIPSE" => {
                let pos = self.axis_placement(e.arg(0));
                let r1 = e.arg(1).as_f64()?;
                let r2 = if e.name == "IFCELLIPSE" {
                    e.arg(2).as_f64()?
                } else {
                    r1
                };
                let n = arc_segments(r1.max(r2), std::f64::consts::TAU, self.arc_tol);
                (0..n)
                    .map(|i| {
                        let a = i as f64 / n as f64 * std::f64::consts::TAU;
                        pos.apply([r1 * a.cos(), r2 * a.sin(), 0.0])
                    })
                    .collect()
            }
            other => {
                self.warn(format!("unsupported curve {other} (skipped)"));
                return None;
            }
        };
        pts.dedup_by(|a, b| norm(sub(*a, *b)) < 1e-9);
        (pts.len() >= 2).then_some(pts)
    }

    /// IfcTrimmedCurve(BasisCurve, Trim1, Trim2, SenseAgreement, MasterRepresentation) on a line,
    /// circle or ellipse. Cartesian trims are preferred unless the file says PARAMETER.
    fn trimmed_curve(&mut self, e: &Entity, depth: usize) -> Option<Vec<[f64; 3]>> {
        let basis = self.get(e.arg(0))?;
        let sense = e.arg(3).as_enum() != Some("F");
        let prefer_param = e.arg(4).as_enum() == Some("PARAMETER");
        let trim = |r: &Self, v: &Value| -> (Option<[f64; 3]>, Option<f64>) {
            let (mut pt, mut par) = (None, None);
            for x in v.as_list().unwrap_or(&[]) {
                match x {
                    Value::Typed(n, inner) if n == "IFCPARAMETERVALUE" => par = inner.as_f64(),
                    Value::Ref(_) => pt = r.point(x),
                    _ => {}
                }
            }
            (pt, par)
        };
        let (t1, t2) = (trim(self, e.arg(1)), trim(self, e.arg(2)));
        match basis.name.as_str() {
            "IFCLINE" => {
                let p0 = self.point(basis.arg(0))?;
                let vec = self.get(basis.arg(1))?;
                let d = self.direction(vec.arg(0))?;
                let m = vec.arg(1).as_f64().unwrap_or(1.0);
                let at = |t: f64| {
                    [
                        p0[0] + d[0] * m * t,
                        p0[1] + d[1] * m * t,
                        p0[2] + d[2] * m * t,
                    ]
                };
                let pick = |t: (Option<[f64; 3]>, Option<f64>)| match (t, prefer_param) {
                    ((_, Some(p)), true) | ((None, Some(p)), false) => Some(at(p)),
                    ((Some(q), _), _) => Some(q),
                    _ => None,
                };
                Some(vec![pick(t1)?, pick(t2)?])
            }
            "IFCCIRCLE" | "IFCELLIPSE" => {
                let pos = self.axis_placement(basis.arg(0));
                let r1 = basis.arg(1).as_f64()?;
                let r2 = if basis.name == "IFCELLIPSE" {
                    basis.arg(2).as_f64()?
                } else {
                    r1
                };
                let angle_of = |q: [f64; 3]| {
                    let d = sub(q, pos.t);
                    (dot(d, pos.y) / r2).atan2(dot(d, pos.x) / r1)
                };
                let angle = |t: (Option<[f64; 3]>, Option<f64>)| match (t, prefer_param) {
                    ((_, Some(p)), true) | ((None, Some(p)), false) => Some(p * self.angle_scale),
                    ((Some(q), _), _) => Some(angle_of(q)),
                    _ => None,
                };
                let (a1, a2) = (angle(t1)?, angle(t2)?);
                let tau = std::f64::consts::TAU;
                let sweep = if sense {
                    let s = (a2 - a1).rem_euclid(tau);
                    if s < 1e-12 { tau } else { s }
                } else {
                    let s = (a1 - a2).rem_euclid(tau);
                    -(if s < 1e-12 { tau } else { s })
                };
                let n = arc_segments(r1.max(r2), sweep, self.arc_tol);
                let mut pts: Vec<[f64; 3]> = (0..=n)
                    .map(|i| {
                        let a = a1 + sweep * i as f64 / n as f64;
                        pos.apply([r1 * a.cos(), r2 * a.sin(), 0.0])
                    })
                    .collect();
                // Snap the ends onto Cartesian trims so joined segments meet exactly.
                if let (Some(q), false) = (t1.0, prefer_param) {
                    pts[0] = q;
                }
                if let (Some(q), false) = (t2.0, prefer_param) {
                    pts[n] = q;
                }
                Some(pts)
            }
            _ => {
                self.warn(format!("trimmed {} drawn untrimmed", basis.name));
                self.curve3(e.arg(0), depth + 1)
            }
        }
    }

    /// Plane angle unit from the project units, as a factor to radians.
    fn plane_angle_to_radians(&mut self) -> f64 {
        let f = self.f;
        let assignment = f
            .by_type("IFCPROJECT")
            .next()
            .and_then(|(_, p)| self.get(p.arg(8)));
        for u in assignment.and_then(|a| a.arg(0).as_list()).unwrap_or(&[]) {
            let Some(u) = self.get(u) else { continue };
            if u.arg(1).as_enum() != Some("PLANEANGLEUNIT") {
                continue;
            }
            match u.name.as_str() {
                "IFCSIUNIT" => return 1.0,
                "IFCCONVERSIONBASEDUNIT" => {
                    if let Some(factor) = self.get(u.arg(3)).and_then(|m| m.arg(0).as_f64()) {
                        return factor;
                    }
                }
                _ => {}
            }
        }
        1.0
    }

    /// IfcSweptDiskSolid: a tube of radius `Radius` along `Directrix` (reinforcing bars, cables).
    fn swept_disk(&mut self, e: &Entity, tf: &Affine, out: &mut Emit) {
        let Some(path) = self.curve3(e.arg(0), 0) else {
            return;
        };
        let Some(r) = e.arg(1).as_f64().filter(|r| r.is_finite() && *r > 0.0) else {
            return;
        };
        if e.arg(2).as_f64().is_some_and(|ri| ri > 0.0) {
            self.warn("hollow swept disk drawn solid (inner radius ignored)");
        }
        let path: Vec<[f64; 3]> = path.into_iter().map(|p| tf.apply(p)).collect();
        let n = arc_segments(r, std::f64::consts::TAU, self.arc_tol);
        tube(&path, r, n, out);
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

/// Closed prism (mm): a 2D loop in `pos`'s XY plane, from z0 to z1 along its Z. Outward-facing.
fn prism_tris(loop2d: &[[f64; 2]], pos: &Affine, z0: f64, z1: f64, scale: f64) -> Vec<Tri> {
    let mut l = loop2d.to_vec();
    if signed_area(&l) < 0.0 {
        l.reverse();
    }
    let p = |q: [f64; 2], z: f64| pos.apply([q[0], q[1], z]).map(|c| c * scale);
    let bottom: Vec<[f64; 3]> = l.iter().rev().map(|q| p(*q, z0)).collect();
    let top: Vec<[f64; 3]> = l.iter().map(|q| p(*q, z1)).collect();
    let mut mb = MeshBuilder::default();
    mb.add_polygon(&bottom, &[]);
    mb.add_polygon(&top, &[]);
    let n = l.len();
    for i in 0..n {
        let j = (i + 1) % n;
        let (bi, bj, ti, tj) = (p(l[i], z0), p(l[j], z0), p(l[i], z1), p(l[j], z1));
        mb.add_triangle([bi, bj, tj]);
        mb.add_triangle([bi, tj, ti]);
    }
    let mut tris = mb.tris;
    // A mirroring placement turns the solid inside out.
    if pos.determinant() < 0.0 {
        for t in &mut tris {
            t.swap(1, 2);
        }
    }
    tris
}

/// Arc through three points in 3D, within `tol` of the true arc (via the 2D version in the
/// arc's own plane). Collinear points give the straight polyline.
fn arc3(a: [f64; 3], b: [f64; 3], c: [f64; 3], tol: f64) -> Vec<[f64; 3]> {
    let n = cross(sub(b, a), sub(c, a));
    let (Some(e1), Some(n)) = (normalize(sub(b, a)), normalize(n)) else {
        return vec![a, b, c];
    };
    let e2 = cross(n, e1);
    let to2 = |p: [f64; 3]| {
        let d = sub(p, a);
        [dot(d, e1), dot(d, e2)]
    };
    arc(to2(a), to2(b), to2(c), tol)
        .into_iter()
        .map(|q| {
            [
                a[0] + e1[0] * q[0] + e2[0] * q[1],
                a[1] + e1[1] * q[0] + e2[1] * q[1],
                a[2] + e1[2] * q[0] + e2[2] * q[1],
            ]
        })
        .collect()
}

/// Replace the given (right-angle, concave or convex) polygon corners with arcs of radius `r`.
fn fillet(poly: &[[f64; 2]], corners: &[usize], r: f64, tol: f64) -> Vec<[f64; 2]> {
    let n = poly.len();
    let mut out = Vec::with_capacity(n + corners.len() * 8);
    for (i, &c) in poly.iter().enumerate() {
        if !corners.contains(&i) {
            out.push(c);
            continue;
        }
        let (p, q) = (poly[(i + n - 1) % n], poly[(i + 1) % n]);
        let l1 = ((c[0] - p[0]).powi(2) + (c[1] - p[1]).powi(2)).sqrt();
        let l2 = ((q[0] - c[0]).powi(2) + (q[1] - c[1]).powi(2)).sqrt();
        if r >= l1 || r >= l2 || l1 == 0.0 || l2 == 0.0 {
            out.push(c);
            continue;
        }
        let d1 = [(c[0] - p[0]) / l1, (c[1] - p[1]) / l1];
        let d2 = [(q[0] - c[0]) / l2, (q[1] - c[1]) / l2];
        let t1 = [c[0] - d1[0] * r, c[1] - d1[1] * r];
        let t2 = [c[0] + d2[0] * r, c[1] + d2[1] * r];
        // Centre of a fillet at a right angle; the arc's middle point lies towards the corner.
        let center = [t1[0] + d2[0] * r, t1[1] + d2[1] * r];
        let k = ((c[0] - center[0]).powi(2) + (c[1] - center[1]).powi(2)).sqrt();
        let mid = [
            center[0] + (c[0] - center[0]) / k * r,
            center[1] + (c[1] - center[1]) / k * r,
        ];
        out.extend(arc(t1, mid, t2, tol));
    }
    out
}

/// Tube of radius `r` with `n` sides along a polyline: rings carried along the path without
/// twisting, mitred at corners so the radius stays `r`, with end caps unless the path is closed.
fn tube(path: &[[f64; 3]], r: f64, n: usize, out: &mut Emit) {
    let mut p: Vec<[f64; 3]> = path.to_vec();
    p.dedup_by(|a, b| norm(sub(*a, *b)) < 1e-9);
    if p.len() < 2 {
        return;
    }
    let closed = norm(sub(p[0], *p.last().unwrap())) < 1e-9;
    if closed {
        p.pop();
    }
    let m = p.len();
    let seg = |i: usize| normalize(sub(p[(i + 1) % m], p[i]));
    let mut rings: Vec<Vec<[f64; 3]>> = Vec::with_capacity(m);
    let mut prev_n: Option<[f64; 3]> = None;
    for (i, pi) in p.iter().enumerate() {
        let d_in = if i > 0 || closed {
            seg((i + m - 1) % m)
        } else {
            None
        };
        let d_out = if i + 1 < m || closed { seg(i) } else { None };
        let (Some(t), d_in) = (
            match (d_in, d_out) {
                (Some(a), Some(b)) => {
                    normalize([a[0] + b[0], a[1] + b[1], a[2] + b[2]]).or(Some(b))
                }
                (a, b) => a.or(b),
            },
            d_in,
        ) else {
            return;
        };
        // Frame: carry the previous normal along (parallel transport), so rings don't twist.
        let seed = prev_n.unwrap_or(if t[2].abs() < 0.9 {
            [0.0, 0.0, 1.0]
        } else {
            [1.0, 0.0, 0.0]
        });
        let Some(nrm) = normalize(sub(seed, t.map(|c| c * dot(seed, t)))) else {
            return;
        };
        prev_n = Some(nrm);
        let bin = cross(t, nrm);
        // Mitre: stretch across the bend by 1 / cos(half angle) so the tube keeps radius r.
        let (bend, stretch) = match (d_in, d_out) {
            (Some(a), Some(b)) => {
                let c = dot(a, t).clamp(0.3, 1.0);
                (normalize(cross(t, cross(a, b))), 1.0 / c)
            }
            _ => (None, 1.0),
        };
        let ring = (0..n)
            .map(|j| {
                let a = j as f64 / n as f64 * std::f64::consts::TAU;
                let mut off = [0, 1, 2].map(|k| r * (a.cos() * nrm[k] + a.sin() * bin[k]));
                if let Some(bd) = bend {
                    let extra = dot(off, bd) * (stretch - 1.0);
                    off = [0, 1, 2].map(|k| off[k] + extra * bd[k]);
                }
                [pi[0] + off[0], pi[1] + off[1], pi[2] + off[2]]
            })
            .collect();
        rings.push(ring);
    }
    let pairs = if closed { m } else { m - 1 };
    for i in 0..pairs {
        let (a, b) = (&rings[i], &rings[(i + 1) % m]);
        for j in 0..n {
            let k = (j + 1) % n;
            // Rings turn counter-clockwise around the tangent t. With edges along t and around
            // the ring, (a_j, b_k, b_j) has normal +radial: facing outward.
            out.triangle([a[j], b[k], b[j]]);
            out.triangle([a[j], a[k], b[k]]);
        }
    }
    if !closed {
        let start: Vec<[f64; 3]> = rings[0].iter().rev().copied().collect();
        out.polygon(&start, &[]);
        out.polygon(&rings[m - 1], &[]);
    }
}

/// Segments needed so that chords stay within `tol` of a circle of radius `r` over `sweep` radians.
/// Also at most 15° per segment, so small circles (bolts, drill holes) stay round: with the
/// distance tolerance alone a Ø20 mm hole got 10 segments and lost 6.5 % of its volume.
fn arc_segments(r: f64, sweep: f64, tol: f64) -> usize {
    const MAX_STEP: f64 = std::f64::consts::PI / 12.0;
    let step = if tol > 0.0 && tol < r {
        (2.0 * (1.0 - tol / r).acos()).min(MAX_STEP)
    } else {
        MAX_STEP
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

    /// Small full circles: within ~1.2 % of the exact area (15° segments), like IfcOpenShell.
    fn tube_mesh(path: &[[f64; 3]], r: f64, n: usize) -> bimble_geom::Mesh {
        let mut mb = MeshBuilder::default();
        let budget = Budget::new(&Limits {
            max_triangles: usize::MAX,
            max_item_visits: usize::MAX,
        });
        let mut out = Emit {
            mb: &mut mb,
            scale: 1.0,
            budget: &budget,
        };
        tube(path, r, n, &mut out);
        mb.build([0.0; 3])
    }

    #[test]
    fn straight_tube_is_a_closed_outward_prism() {
        let (r, n, l) = (8.0, 24, 1000.0);
        let m = tube_mesh(&[[0.0, 0.0, 0.0], [l, 0.0, 0.0]], r, n);
        let prism = 0.5 * n as f64 * r * r * (std::f64::consts::TAU / n as f64).sin() * l;
        assert!(m.volume() > 0.0, "inward-facing: {}", m.volume());
        assert!(
            (m.volume() - prism).abs() / prism < 1e-6,
            "{} vs {prism}",
            m.volume()
        );
    }

    #[test]
    fn bent_tube_keeps_its_radius_at_the_corner() {
        // L-shaped bar: 1 m along X, then 1 m along Y. Mitred corner: volume = 2 m of section.
        let (r, n) = (8.0, 24);
        let m = tube_mesh(
            &[[0.0, 0.0, 0.0], [1000.0, 0.0, 0.0], [1000.0, 1000.0, 0.0]],
            r,
            n,
        );
        let section = 0.5 * n as f64 * r * r * (std::f64::consts::TAU / n as f64).sin();
        let want = section * 2000.0;
        assert!(
            (m.volume() - want).abs() / want < 1e-3,
            "{} vs {want}",
            m.volume()
        );
        // Every vertex at the corner ring stays within the corner's miter: none collapses inward.
        let near_corner = m
            .positions
            .iter()
            .filter(|p| (p[0] - 1000.0).abs() < 20.0 && p[1].abs() < 20.0)
            .count();
        assert!(near_corner > 0);
    }

    #[test]
    fn fillet_replaces_corners_with_arcs() {
        let sq = [[0.0, 0.0], [10.0, 0.0], [10.0, 10.0], [0.0, 10.0]];
        let f = fillet(&sq, &[2], 2.0, 0.01);
        assert!(f.len() > 5);
        // Area of a square with one rounded corner: 100 − r² (1 − π/4).
        let area: f64 = f
            .iter()
            .zip(f.iter().cycle().skip(1))
            .map(|(a, b)| a[0] * b[1] - b[0] * a[1])
            .sum::<f64>()
            / 2.0;
        // Exact rounded area, minus what the arc's chords cut off: n · r²/2 · (θ − sin θ).
        let segs = (f.len() - 3 - 1) as f64;
        let th = std::f64::consts::FRAC_PI_2 / segs;
        let want = 100.0 - 4.0 * (1.0 - std::f64::consts::FRAC_PI_4) - segs * 2.0 * (th - th.sin());
        assert!((area - want).abs() < 1e-9, "{area} vs {want}");
    }

    #[test]
    fn small_circles_stay_round() {
        let tol = ARC_TOLERANCE_MM; // file in mm
        for r in [5.0, 10.0, 50.0] {
            let n = arc_segments(r, std::f64::consts::TAU, tol);
            let area = 0.5 * n as f64 * r * r * (std::f64::consts::TAU / n as f64).sin();
            let exact = std::f64::consts::PI * r * r;
            assert!(
                (exact - area) / exact < 0.012,
                "r={r}: {n} segments, {:.2}% short",
                (exact - area) / exact * 100.0
            );
        }
    }

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

    /// IfcGrid axes come out in world mm, moved by the grid's placement, with their tags;
    /// polylines and indexed poly curves; a grid repeated on another storey adds nothing.
    #[test]
    fn grid_axes_are_read_in_world_mm() {
        let m = concrete_frame(&FrameSpec::default());
        let ifc = crate::export(&m, "t.ifc", "t");
        let grid = "#900001=IFCCARTESIANPOINT((1000.,2000.,0.));
#900002=IFCAXIS2PLACEMENT3D(#900001,$,$);
#900003=IFCLOCALPLACEMENT($,#900002);
#900010=IFCCARTESIANPOINT((0.,-500.));
#900011=IFCCARTESIANPOINT((0.,6000.));
#900012=IFCPOLYLINE((#900010,#900011));
#900013=IFCGRIDAXIS('A',#900012,.T.);
#900020=IFCCARTESIANPOINT((-500.,0.));
#900021=IFCCARTESIANPOINT((9000.,0.));
#900022=IFCPOLYLINE((#900020,#900021));
#900023=IFCGRIDAXIS('1',#900022,.T.);
#900024=IFCCIRCLE($,1.);
#900025=IFCGRIDAXIS('skipped',#900024,.T.);
#900026=IFCCARTESIANPOINTLIST2D(((3000.,-500.),(3000.,2000.),(3000.,6000.)),$);
#900027=IFCINDEXEDPOLYCURVE(#900026,$,.F.);
#900028=IFCGRIDAXIS('B',#900027,.T.);
#900030=IFCGRID('3abcdefghijklmnopqrstu',$,'Grid',$,$,#900003,$,(#900013,#900025,#900028),(#900023),$,$);
#900031=IFCGRID('3abcdefghijklmnopqrstv',$,'Same grid, next storey',$,$,#900003,$,(#900013),(#900023),$,$);
ENDSEC;
END-ISO";
        let ifc = ifc.replacen("ENDSEC;\nEND-ISO", grid, 1);
        assert!(ifc.contains("IFCGRID("), "fixture not inserted");
        let s = read_str(&ifc).unwrap();
        assert_eq!(
            s.grids,
            vec![
                bimble_geom::GridAxis {
                    tag: "A".into(),
                    start_mm: [1000.0, 1500.0],
                    end_mm: [1000.0, 8000.0],
                },
                bimble_geom::GridAxis {
                    tag: "B".into(),
                    start_mm: [4000.0, 1500.0],
                    end_mm: [4000.0, 8000.0],
                },
                bimble_geom::GridAxis {
                    tag: "1".into(),
                    start_mm: [500.0, 2000.0],
                    end_mm: [10000.0, 2000.0],
                },
            ]
        );
    }

    #[test]
    fn rejects_non_ifc() {
        assert!(matches!(
            read_str("ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('AP214'));\nENDSEC;\nDATA;\nENDSEC;"),
            Err(ReadError::NotIfc(_))
        ));
    }
}
