//! Schedules (ADR 0029): tables of elements and their quantities, like Revit's nomenclatures.
//!
//! A schedule is data: which elements (IFC classes, a material family), which columns, how
//! rows are grouped and sorted, whether totals are shown. The values are measured from the
//! geometry when the schedule is drawn (see `bimble_drawing::schedule`), so a schedule works the
//! same on a `.bimble` model and on any IFC.

use crate::MaterialCategory;
use serde::{Deserialize, Serialize};

/// A column of a schedule.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Field {
    /// The storey the element is on.
    Level,
    /// Where it stands on the grid ("B-2"): the axis crossing nearest its centre in plan.
    Location,
    Name,
    /// The IFC class.
    Class,
    Material,
    /// Its section: "30/30", "Ø30", "GL24h 24x46" (as on plans).
    Section,
    /// Along its axis, m (a column's height, a beam's or wall's length).
    Length,
    /// Across its axis, cm (a wall's thickness, a beam's width).
    Width,
    /// Vertical extent, cm (a beam's depth, a slab's thickness, a wall's height).
    Height,
    /// m²: a wall's face (length × height), a slab's or footing's plan area.
    Area,
    /// m³, from the closed mesh.
    Volume,
    GlobalId,
}

impl Field {
    pub const ALL: [Field; 12] = [
        Field::Level,
        Field::Location,
        Field::Name,
        Field::Class,
        Field::Material,
        Field::Section,
        Field::Length,
        Field::Width,
        Field::Height,
        Field::Area,
        Field::Volume,
        Field::GlobalId,
    ];

    /// The column's title, with its unit.
    pub fn title(self) -> &'static str {
        match self {
            Field::Level => "Level",
            Field::Location => "Location",
            Field::Name => "Name",
            Field::Class => "IFC class",
            Field::Material => "Material",
            Field::Section => "Section",
            Field::Length => "Length (m)",
            Field::Width => "Width (cm)",
            Field::Height => "Height (cm)",
            Field::Area => "Area (m²)",
            Field::Volume => "Volume (m³)",
            Field::GlobalId => "GlobalId",
        }
    }

    /// Summed in the totals.
    pub fn summed(self) -> bool {
        matches!(self, Field::Length | Field::Area | Field::Volume)
    }
}

/// What a schedule lists and shows.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ScheduleDef {
    pub name: String,
    /// The IFC classes listed (case-insensitive); empty: every class.
    pub classes: Vec<String>,
    /// Only elements of this material family; none: every family.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub family: Option<MaterialCategory>,
    pub fields: Vec<Field>,
    /// Rows grouped by level, lowest first, a subtotal under each.
    #[serde(default)]
    pub group_by_level: bool,
    /// A total line at the end.
    #[serde(default)]
    pub totals: bool,
}

impl ScheduleDef {
    /// Whether an element of this class and family belongs in the schedule.
    pub fn lists(&self, class: &str, family: MaterialCategory) -> bool {
        (self.classes.is_empty() || self.classes.iter().any(|c| c.eq_ignore_ascii_case(class)))
            && self.family.is_none_or(|f| f == family)
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.name.trim().is_empty() {
            return Err("a schedule needs a name".into());
        }
        if self.fields.is_empty() || self.fields.len() > 32 {
            return Err(format!("schedule {}: 1 to 32 columns", self.name));
        }
        if self.classes.len() > 64 {
            return Err(format!("schedule {}: at most 64 classes", self.name));
        }
        Ok(())
    }
}

fn def(
    name: &str,
    classes: &[&str],
    family: Option<MaterialCategory>,
    fields: &[Field],
) -> ScheduleDef {
    ScheduleDef {
        name: name.into(),
        classes: classes.iter().map(|c| c.to_string()).collect(),
        family,
        fields: fields.to_vec(),
        group_by_level: true,
        totals: true,
    }
}

/// The built-in schedules, one per kind of structural element and material, as an engineering
/// office keeps them: columns, beams, slabs, walls, footings, in concrete, steel, timber.
pub fn default_schedules() -> Vec<ScheduleDef> {
    use Field::*;
    use MaterialCategory::*;
    let col = [Level, Location, Name, Material, Section, Length, Volume];
    let beam = [
        Level, Name, Material, Section, Length, Width, Height, Volume,
    ];
    vec![
        def("Concrete columns", &["IfcColumn"], Some(Concrete), &col),
        def("Steel columns", &["IfcColumn"], Some(Steel), &col),
        def("Timber columns", &["IfcColumn"], Some(Timber), &col),
        def("Concrete beams", &["IfcBeam"], Some(Concrete), &beam),
        def("Steel beams", &["IfcBeam"], Some(Steel), &beam),
        def("Timber beams", &["IfcBeam"], Some(Timber), &beam),
        def(
            "Slabs",
            &["IfcSlab"],
            None,
            &[Level, Name, Material, Height, Area, Volume],
        ),
        def(
            "Walls",
            &["IfcWall", "IfcWallStandardCase"],
            None,
            &[Level, Name, Material, Width, Length, Height, Area, Volume],
        ),
        def(
            "Footings",
            &["IfcFooting"],
            None,
            &[Level, Location, Name, Material, Section, Height, Volume],
        ),
        def(
            "All elements",
            &[],
            None,
            &[Level, Name, Class, Material, Volume],
        ),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_valid_and_named_once() {
        let v = default_schedules();
        let mut names: Vec<&str> = v.iter().map(|d| d.name.as_str()).collect();
        for d in &v {
            d.validate().unwrap();
        }
        names.sort();
        names.dedup();
        assert_eq!(names.len(), v.len());
    }

    #[test]
    fn a_schedule_lists_its_classes_and_family() {
        let v = default_schedules();
        let steel_beams = v.iter().find(|d| d.name == "Steel beams").unwrap();
        assert!(steel_beams.lists("IFCBEAM", MaterialCategory::Steel));
        assert!(!steel_beams.lists("IfcBeam", MaterialCategory::Concrete));
        assert!(!steel_beams.lists("IfcColumn", MaterialCategory::Steel));
        let all = v.iter().find(|d| d.name == "All elements").unwrap();
        assert!(all.lists("IfcAnything", MaterialCategory::Other));
    }
}
