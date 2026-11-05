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
}

impl MaterialCategory {
    /// IFC4 IfcMaterial.Category recommended value.
    pub fn ifc_category(self) -> &'static str {
        match self {
            MaterialCategory::Concrete => "concrete",
            MaterialCategory::Steel => "steel",
            MaterialCategory::Timber => "wood",
            MaterialCategory::Masonry => "brick",
            MaterialCategory::Insulation => "insulation",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Material {
    pub id: GlobalId,
    /// e.g. "C30/37", "S355", "GL24h"
    pub name: String,
    pub category: MaterialCategory,
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
}

impl Generator {
    pub fn name(&self) -> &'static str {
        match self {
            Generator::RectColumn { .. } => "rect_column",
            Generator::RectBeam { .. } => "rect_beam",
            Generator::Slab { .. } => "slab",
            Generator::Wall { .. } => "wall",
            Generator::PadFooting { .. } => "pad_footing",
        }
    }

    /// Generator schema version, bumped when parameters change meaning.
    pub fn version(&self) -> u32 {
        1
    }

    pub fn profile(&self) -> Profile {
        match self {
            Generator::RectColumn { width, depth, .. }
            | Generator::PadFooting { width, depth, .. } => Profile::Rectangle {
                x_dim: *width,
                y_dim: *depth,
            },
            Generator::RectBeam { width, height, .. } => Profile::Rectangle {
                x_dim: *width,
                y_dim: *height,
            },
            Generator::Slab { outline, .. } => Profile::Polygon(outline.clone()),
            // Wall profile starts at the local origin and runs along +X; centred on its thickness.
            Generator::Wall {
                length, thickness, ..
            } => Profile::Polygon(vec![
                [0.0, -thickness / 2.0],
                [*length, -thickness / 2.0],
                [*length, thickness / 2.0],
                [0.0, thickness / 2.0],
            ]),
        }
    }

    /// Extrusion depth along local +Z.
    pub fn depth(&self) -> f64 {
        match self {
            Generator::RectColumn { height, .. } | Generator::PadFooting { height, .. } => *height,
            Generator::RectBeam { length, .. } => *length,
            Generator::Slab { thickness, .. } => *thickness,
            Generator::Wall { height, .. } => *height,
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
        }
    }
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
}

/// The whole project. BTreeMaps keep serialization order canonical (sorted by GlobalId).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Model {
    pub project_id: GlobalId,
    pub project_name: String,
    pub storeys: BTreeMap<GlobalId, Storey>,
    pub materials: BTreeMap<GlobalId, Material>,
    pub elements: BTreeMap<GlobalId, Element>,
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
        for (k, s) in &self.storeys {
            if *k != s.id || !s.elevation.is_finite() {
                return Err(format!("storey {k}: inconsistent id or elevation"));
            }
        }
        for (k, m) in &self.materials {
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
            let p = &e.placement;
            if p.origin
                .iter()
                .chain(&p.z_axis)
                .chain(&p.x_axis)
                .any(|v| !v.is_finite())
            {
                return Err(ctx("non-finite placement".into()));
            }
            e.generator.validate().map_err(ctx)?;
        }
        Ok(())
    }
}
