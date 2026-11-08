//! The building model. Types are named after IFC; `.bimble` stores a superset of IFC.

use crate::GlobalId;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// IFC classes supported so far. Names are the IFC entity names.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum IfcClass {
    IfcColumn,
    IfcBeam,
    IfcSlab,
    IfcWall,
    IfcFooting,
}

impl IfcClass {
    pub fn name(self) -> &'static str {
        match self {
            IfcClass::IfcColumn => "IfcColumn",
            IfcClass::IfcBeam => "IfcBeam",
            IfcClass::IfcSlab => "IfcSlab",
            IfcClass::IfcWall => "IfcWall",
            IfcClass::IfcFooting => "IfcFooting",
        }
    }
}

/// Material families. Drives the hatch pattern and colour in the viewer and drawings.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MaterialCategory {
    Concrete,
    Steel,
    Timber,
    Masonry,
    Insulation,
    Earth,
    Glass,
    Other,
}

impl MaterialCategory {
    pub const ALL: [MaterialCategory; 8] = [
        MaterialCategory::Concrete,
        MaterialCategory::Steel,
        MaterialCategory::Timber,
        MaterialCategory::Masonry,
        MaterialCategory::Insulation,
        MaterialCategory::Earth,
        MaterialCategory::Glass,
        MaterialCategory::Other,
    ];

    /// IFC4 IfcMaterial.Category recommended value.
    pub fn ifc_category(self) -> &'static str {
        match self {
            MaterialCategory::Concrete => "concrete",
            MaterialCategory::Steel => "steel",
            MaterialCategory::Timber => "wood",
            MaterialCategory::Masonry => "brick",
            MaterialCategory::Insulation => "insulation",
            MaterialCategory::Earth => "earth",
            MaterialCategory::Glass => "glass",
            MaterialCategory::Other => "",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            MaterialCategory::Concrete => "Concrete",
            MaterialCategory::Steel => "Steel",
            MaterialCategory::Timber => "Timber",
            MaterialCategory::Masonry => "Masonry",
            MaterialCategory::Insulation => "Insulation",
            MaterialCategory::Earth => "Earth",
            MaterialCategory::Glass => "Glass",
            MaterialCategory::Other => "Other",
        }
    }

    /// Best-effort family from a material name as found in IFC files ("C30/37", "GL24h", "S355",
    /// "Béton armé", "LMP_Earth"…). A heuristic: unknown names give `Other`.
    pub fn classify(name: &str) -> MaterialCategory {
        let n = name.to_lowercase();
        let has = |words: &[&str]| words.iter().any(|w| n.contains(w));
        let digits_after = |prefix: &str| {
            n.strip_prefix(prefix)
                .is_some_and(|r| r.chars().next().is_some_and(|c| c.is_ascii_digit()))
        };
        // Concrete strength classes have a slash (C30/37, LC25/28); timber classes don't (C24, GL24h, D30).
        if has(&["concrete", "béton", "beton", "beton"])
            || (n.contains('/') && (digits_after("c") || digits_after("lc")))
        {
            MaterialCategory::Concrete
        } else if has(&[
            "timber", "wood", "bois", "clt", "glulam", "lamellé", "lamelle",
        ]) || digits_after("gl")
            || digits_after("c")
            || digits_after("d")
        {
            MaterialCategory::Timber
        } else if has(&["steel", "acier", "stahl", "metal", "métal"]) || digits_after("s") {
            MaterialCategory::Steel
        } else if has(&[
            "brick",
            "brique",
            "masonry",
            "maçonnerie",
            "maconnerie",
            "block",
            "parpaing",
            "stone",
            "pierre",
        ]) {
            // Before earth: "terre cuite" is fired clay, not soil.
            MaterialCategory::Masonry
        } else if has(&[
            "earth", "terre", "soil", "ground", "terrain", "gravel", "gravier", "molasse",
        ]) {
            MaterialCategory::Earth
        } else if has(&[
            "insulation",
            "isolation",
            "isolant",
            "mineral wool",
            "laine",
            "xps",
            "eps",
        ]) {
            MaterialCategory::Insulation
        } else if has(&["glass", "verre", "glazing", "vitrage"]) {
            MaterialCategory::Glass
        } else {
            MaterialCategory::Other
        }
    }
}

#[cfg(test)]
mod material_tests {
    use super::MaterialCategory::{self, *};

    #[test]
    fn classify_real_names() {
        let cases = [
            ("C30/37", Concrete),
            ("C30/37(P)", Concrete),
            ("C80/95 (P)", Concrete),
            ("Concrete_T4", Concrete),
            ("Béton armé", Concrete),
            ("GL24h", Timber),
            ("C24 (CLT)", Timber),
            ("Toit de base:LM_SLAB_TIMBER_CLT_240", Timber),
            ("S355", Steel),
            ("LMP_Earth", Earth),
            ("Laine minérale", Insulation),
            ("Brique terre cuite", Masonry),
            ("Mystery", Other),
        ];
        for (name, want) in cases {
            assert_eq!(MaterialCategory::classify(name), want, "{name}");
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Material {
    pub id: GlobalId,
    /// e.g. "C30/37", "S355", "GL24h"
    pub name: String,
    pub category: MaterialCategory,
    /// Density, modulus, strength and their source (ADR 0031). Absent when unknown, so
    /// older models keep their hash.
    #[serde(
        default,
        skip_serializing_if = "crate::library::MaterialProps::is_empty"
    )]
    pub props: crate::library::MaterialProps,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Storey {
    pub id: GlobalId,
    pub name: String,
    /// Elevation of the storey in mm.
    pub elevation: f64,
}

/// Local frame of an element, relative to its storey. Units: mm.
/// `z_axis` is the extrusion direction; `x_axis` is the profile's X direction.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Placement {
    pub origin: [f64; 3],
    pub z_axis: [f64; 3],
    pub x_axis: [f64; 3],
}

impl Placement {
    /// Placement axes must be finite, unit length and perpendicular. Files are untrusted input:
    /// a skewed or scaled frame would draw differently here than in other IFC tools.
    pub fn validate(&self) -> Result<(), String> {
        let all = self.origin.iter().chain(&self.z_axis).chain(&self.x_axis);
        if all.into_iter().any(|v| !v.is_finite()) {
            return Err("non-finite placement".into());
        }
        let dot = |a: [f64; 3], b: [f64; 3]| a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        const TOL: f64 = 1e-6;
        if (dot(self.z_axis, self.z_axis) - 1.0).abs() > TOL
            || (dot(self.x_axis, self.x_axis) - 1.0).abs() > TOL
        {
            return Err("placement axes must be unit vectors".into());
        }
        if dot(self.z_axis, self.x_axis).abs() > TOL {
            return Err("placement x_axis must be perpendicular to z_axis".into());
        }
        Ok(())
    }

    pub fn at(x: f64, y: f64, z: f64) -> Self {
        Placement {
            origin: [x, y, z],
            z_axis: [0.0, 0.0, 1.0],
            x_axis: [1.0, 0.0, 0.0],
        }
    }

    pub fn y_axis(&self) -> [f64; 3] {
        let (z, x) = (self.z_axis, self.x_axis);
        [
            z[1] * x[2] - z[2] * x[1],
            z[2] * x[0] - z[0] * x[2],
            z[0] * x[1] - z[1] * x[0],
        ]
    }
}

/// 2D profile in the element's local XY plane. Units: mm.
#[derive(Clone, Debug, PartialEq)]
pub enum Profile {
    /// Centred rectangle (same convention as IfcRectangleProfileDef).
    Rectangle { x_dim: f64, y_dim: f64 },
    /// Closed polygon, counter-clockwise, without repeating the first point.
    Polygon(Vec<[f64; 2]>),
}

/// How an element's geometry is generated. Stored in `.bimble`; exported to IFC as explicit
/// geometry plus a `Bimble_Generator` property set so it can be regenerated.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Generator {
    RectColumn {
        width: f64,
        depth: f64,
        height: f64,
    },
    /// Axis along local Z, so `length` is the extrusion depth. `width` × `height` is the section.
    RectBeam {
        width: f64,
        height: f64,
        length: f64,
    },
    Slab {
        outline: Vec<[f64; 2]>,
        thickness: f64,
    },
    Wall {
        length: f64,
        thickness: f64,
        height: f64,
    },
    PadFooting {
        width: f64,
        depth: f64,
        height: f64,
    },
    /// A non-rectangular extrusion, imported from IFC (ADR 0032). Counter-clockwise outline,
    /// clockwise voids, extruded along local +Z.
    ExtrudedPolygon {
        outline: Vec<[f64; 2]>,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        voids: Vec<Vec<[f64; 2]>>,
        height: f64,
    },
    /// An explicit triangle mesh, imported from IFC (IfcPolygonalFaceSet and friends, ADR 0032).
    /// Editable as properties and placement only, not parametrically.
    Mesh {
        positions: Vec<[f64; 3]>,
        indices: Vec<u32>,
    },
}

/// The extrusion that defines an element's body, when its generator is an extrusion.
#[derive(Clone, Debug, PartialEq)]
pub struct Extrusion {
    pub profile: Profile,
    /// Inner loops of the profile (holes), each clockwise when the outline is counter-clockwise.
    pub voids: Vec<Vec<[f64; 2]>>,
    /// Extrusion depth along local +Z.
    pub depth: f64,
}

impl Generator {
    pub fn name(&self) -> &'static str {
        match self {
            Generator::RectColumn { .. } => "rect_column",
            Generator::RectBeam { .. } => "rect_beam",
            Generator::Slab { .. } => "slab",
            Generator::Wall { .. } => "wall",
            Generator::PadFooting { .. } => "pad_footing",
            Generator::ExtrudedPolygon { .. } => "extruded_polygon",
            Generator::Mesh { .. } => "mesh",
        }
    }

    /// Generator schema version, bumped when parameters change meaning.
    pub fn version(&self) -> u32 {
        1
    }

    /// The extrusion defining this generator's body, or `None` for an explicit mesh.
    pub fn extrusion(&self) -> Option<Extrusion> {
        match self {
            Generator::RectColumn {
                width,
                depth,
                height,
            }
            | Generator::PadFooting {
                width,
                depth,
                height,
            } => Some(Extrusion {
                profile: Profile::Rectangle {
                    x_dim: *width,
                    y_dim: *depth,
                },
                voids: Vec::new(),
                depth: *height,
            }),
            Generator::RectBeam {
                width,
                height,
                length,
            } => Some(Extrusion {
                profile: Profile::Rectangle {
                    x_dim: *width,
                    y_dim: *height,
                },
                voids: Vec::new(),
                depth: *length,
            }),
            Generator::Slab { outline, thickness } => Some(Extrusion {
                profile: Profile::Polygon(outline.clone()),
                voids: Vec::new(),
                depth: *thickness,
            }),
            // Wall profile starts at the local origin and runs along +X; centred on its thickness.
            Generator::Wall {
                length,
                thickness,
                height,
            } => Some(Extrusion {
                profile: Profile::Polygon(vec![
                    [0.0, -thickness / 2.0],
                    [*length, -thickness / 2.0],
                    [*length, thickness / 2.0],
                    [0.0, thickness / 2.0],
                ]),
                voids: Vec::new(),
                depth: *height,
            }),
            Generator::ExtrudedPolygon {
                outline,
                voids,
                height,
            } => Some(Extrusion {
                profile: Profile::Polygon(outline.clone()),
                voids: voids.clone(),
                depth: *height,
            }),
            Generator::Mesh { .. } => None,
        }
    }

    /// Check parameters are physically meaningful. Files are untrusted input.
    pub fn validate(&self) -> Result<(), String> {
        let pos = |name: &str, v: f64| {
            if v.is_finite() && v > 0.0 {
                Ok(())
            } else {
                Err(format!("{name} must be > 0, got {v}"))
            }
        };
        match self {
            Generator::RectColumn {
                width,
                depth,
                height,
            }
            | Generator::PadFooting {
                width,
                depth,
                height,
            } => {
                pos("width", *width)?;
                pos("depth", *depth)?;
                pos("height", *height)
            }
            Generator::RectBeam {
                width,
                height,
                length,
            } => {
                pos("width", *width)?;
                pos("height", *height)?;
                pos("length", *length)
            }
            Generator::Wall {
                length,
                thickness,
                height,
            } => {
                pos("length", *length)?;
                pos("thickness", *thickness)?;
                pos("height", *height)
            }
            Generator::Slab { outline, thickness } => {
                pos("thickness", *thickness)?;
                if outline.len() < 3 {
                    return Err("slab outline needs at least 3 points".into());
                }
                if outline.iter().flatten().any(|v| !v.is_finite()) {
                    return Err("slab outline has non-finite coordinates".into());
                }
                let area2: f64 = outline
                    .iter()
                    .zip(outline.iter().cycle().skip(1))
                    .map(|(a, b)| a[0] * b[1] - b[0] * a[1])
                    .sum();
                if area2 <= 0.0 {
                    return Err("slab outline must be counter-clockwise with non-zero area".into());
                }
                Ok(())
            }
            Generator::ExtrudedPolygon {
                outline,
                voids,
                height,
            } => {
                pos("height", *height)?;
                loop_check("outline", outline)?;
                let area2: f64 = outline
                    .iter()
                    .zip(outline.iter().cycle().skip(1))
                    .map(|(a, b)| a[0] * b[1] - b[0] * a[1])
                    .sum();
                if area2 <= 0.0 {
                    return Err("extruded polygon outline must be counter-clockwise".into());
                }
                for (i, v) in voids.iter().enumerate() {
                    loop_check(&format!("void {i}"), v)?;
                }
                Ok(())
            }
            Generator::Mesh { positions, indices } => {
                if positions.iter().flatten().any(|v| !v.is_finite()) {
                    return Err("mesh has non-finite coordinates".into());
                }
                if positions.len() > u32::MAX as usize {
                    return Err("mesh has too many vertices".into());
                }
                if indices.iter().any(|i| (*i as usize) >= positions.len()) {
                    return Err("mesh index out of range".into());
                }
                Ok(())
            }
        }
    }
}

/// A closed loop of at least 3 finite points (used by polygon generators).
fn loop_check(what: &str, loop_: &[[f64; 2]]) -> Result<(), String> {
    if loop_.len() < 3 {
        return Err(format!("{what} needs at least 3 points"));
    }
    if loop_.iter().flatten().any(|v| !v.is_finite()) {
        return Err(format!("{what} has non-finite coordinates"));
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Element {
    pub id: GlobalId,
    pub class: IfcClass,
    pub name: String,
    pub storey: GlobalId,
    pub material: GlobalId,
    pub placement: Placement,
    pub generator: Generator,
    /// Openings cut out of this element (windows in a wall…), in element-local coordinates,
    /// subtracted when the mesh is built (ADR 0032). Absent when there are none, so files
    /// without openings keep their hashes.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub openings: Vec<Opening>,
    /// Property sets beyond the material (ADR 0032): "Pset_Name/Property" → value as text.
    /// Absent when empty.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub properties: BTreeMap<String, String>,
}

/// A solid subtracted from its element: a window or door cut out of a wall, for instance.
/// Data only, never executed.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Opening {
    /// The IfcOpeningElement's GlobalId, kept from import so it stays stable when other openings
    /// are removed. Absent on older files and on openings without one: export then derives it
    /// from the element and the opening's index.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<GlobalId>,
    pub placement: Placement,
    pub generator: Generator,
}

/// The whole project. BTreeMaps keep serialization order canonical (sorted by GlobalId).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Model {
    pub project_id: GlobalId,
    pub project_name: String,
    pub storeys: BTreeMap<GlobalId, Storey>,
    pub materials: BTreeMap<GlobalId, Material>,
    pub elements: BTreeMap<GlobalId, Element>,
    /// Views (ADR 0021). Absent from files that have none, so their hashes don't change.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub views: BTreeMap<GlobalId, crate::view::View>,
    /// Sheets (#33). Absent from files that have none.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub sheets: BTreeMap<GlobalId, crate::sheet::Sheet>,
    /// Project information (ADR 0030): client, architect, number, phase… for title blocks.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub project: BTreeMap<String, String>,
    /// Schedules of this model (ADR 0029): new ones, and built-in ones changed (same name).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub schedules: Vec<crate::schedule::ScheduleDef>,
}

impl Model {
    pub fn new(name: impl Into<String>) -> Self {
        Model {
            project_id: GlobalId::new(),
            project_name: name.into(),
            ..Default::default()
        }
    }

    /// Referential and parameter integrity. Run after loading any file.
    pub fn validate(&self) -> Result<(), String> {
        crate::project::validate(&self.project)?;
        crate::schedule::validate_all(&self.schedules)?;
        for (k, s) in &self.storeys {
            if *k != s.id || !s.elevation.is_finite() {
                return Err(format!("storey {k}: inconsistent id or elevation"));
            }
        }
        for (k, m) in &self.materials {
            m.props.validate()?;
            if *k != m.id {
                return Err(format!("material {k}: key does not match id"));
            }
        }
        for (k, e) in &self.elements {
            let ctx = |msg: String| format!("{} {k} ({}): {msg}", e.class.name(), e.name);
            if *k != e.id {
                return Err(ctx("key does not match id".into()));
            }
            if !self.storeys.contains_key(&e.storey) {
                return Err(ctx(format!("unknown storey {}", e.storey)));
            }
            if !self.materials.contains_key(&e.material) {
                return Err(ctx(format!("unknown material {}", e.material)));
            }
            e.placement.validate().map_err(ctx)?;
            e.generator.validate().map_err(ctx)?;
            for (i, o) in e.openings.iter().enumerate() {
                let ctx =
                    |msg: String| format!("{} {} (opening {i}): {msg}", e.class.name(), e.name);
                o.placement.validate().map_err(ctx)?;
                o.generator.validate().map_err(ctx)?;
            }
            for k in e.properties.keys() {
                if k.is_empty() {
                    return Err(ctx("empty property key".into()));
                }
            }
        }
        let levels = self.levels();
        for (k, v) in &self.views {
            if *k != v.id {
                return Err(format!("view {k}: key does not match id"));
            }
            v.validate(&levels)?;
        }
        let view_ids = self.view_ids();
        let mut numbers = std::collections::BTreeSet::new();
        for (k, s) in &self.sheets {
            if *k != s.id {
                return Err(format!("sheet {k}: key does not match id"));
            }
            s.validate(|v| view_ids.contains(&v))?;
            if !numbers.insert(s.number.as_str()) {
                return Err(format!("two sheets are numbered {}", s.number));
            }
        }
        Ok(())
    }

    /// Ids of every view of the model: stored ones and the default ones (ADR 0021).
    pub fn view_ids(&self) -> std::collections::BTreeSet<GlobalId> {
        crate::view::effective(&self.views, &self.levels())
            .into_iter()
            .map(|v| v.id)
            .collect()
    }

    /// The storeys as levels for views (key: the storey's GlobalId).
    pub fn levels(&self) -> Vec<crate::view::Level> {
        self.storeys
            .values()
            .map(|s| crate::view::Level {
                key: s.id.to_string(),
                name: s.name.clone(),
                elevation_mm: s.elevation,
            })
            .collect()
    }
}
