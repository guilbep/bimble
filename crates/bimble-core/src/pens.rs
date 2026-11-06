//! Line weights and line types on paper (#42), like Revit's *Épaisseur des lignes* and line
//! styles: 16 pens whose thickness depends on the drawing's scale (model lines), 16 for
//! perspective views and 16 for annotations (independent of scale); named line types (colour,
//! dash pattern, pen); and which line type each kind of line on a drawing uses.
//!
//! All of it is part of the graphic style (`paper`), configurable and checked like the rest.

use crate::style::Rgb;
use serde::{Deserialize, Serialize};

/// Number of pens in each table.
pub const PENS: usize = 16;
/// Most scales in the model table.
pub const MAX_SCALES: usize = 24;
/// Most line types.
pub const MAX_LINE_TYPES: usize = 500;

/// Pen thicknesses, mm on paper.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PenTable {
    /// Scales (1:N) of the model table's columns, increasing.
    pub scales: Vec<u32>,
    /// Model lines: for each pen 1..16, one thickness per scale.
    pub model: Vec<Vec<f32>>,
    /// Perspective (3D) views: one thickness per pen.
    pub perspective: Vec<f32>,
    /// Annotations (dimensions, texts, grids, frames): one thickness per pen, any scale.
    pub annotation: Vec<f32>,
}

impl Default for PenTable {
    /// Revit's metric defaults (model table as in the office's settings).
    fn default() -> Self {
        let model: [[f32; 7]; PENS] = [
            [0.10, 0.10, 0.10, 0.10, 0.10, 0.10, 0.10],
            [0.20, 0.20, 0.20, 0.20, 0.20, 0.10, 0.10],
            [0.30, 0.25, 0.35, 0.35, 0.25, 0.18, 0.15],
            [0.70, 0.70, 0.50, 0.50, 0.35, 0.25, 0.18],
            [1.00, 1.00, 0.70, 0.70, 0.50, 0.35, 0.25],
            [1.40, 1.40, 1.00, 1.00, 0.70, 0.50, 0.35],
            [2.00, 2.00, 1.40, 1.40, 1.00, 0.70, 0.50],
            [2.80, 2.80, 2.00, 2.00, 1.40, 1.00, 0.70],
            [4.00, 4.00, 2.80, 2.80, 2.00, 1.40, 1.00],
            [5.00, 5.00, 4.00, 4.00, 2.80, 2.00, 1.40],
            [6.00, 6.00, 5.00, 5.00, 4.00, 2.80, 2.00],
            [7.00, 7.00, 6.00, 6.00, 5.00, 4.00, 2.80],
            [8.00, 8.00, 7.00, 7.00, 6.00, 5.00, 4.00],
            [9.00, 9.00, 8.00, 8.00, 7.00, 6.00, 5.00],
            [9.00, 9.00, 9.00, 9.00, 8.00, 7.00, 6.00],
            [0.05, 0.05, 0.18, 0.18, 0.10, 0.10, 0.10],
        ];
        let annotation = vec![
            0.10, 0.18, 0.25, 0.35, 0.50, 0.70, 1.00, 1.40, 2.00, 2.80, 4.00, 5.00, 6.00, 7.00,
            8.00, 10.00,
        ];
        PenTable {
            scales: vec![5, 10, 20, 50, 100, 200, 500],
            model: model.iter().map(|r| r.to_vec()).collect(),
            perspective: annotation.clone(),
            annotation,
        }
    }
}

impl PenTable {
    /// Thickness of model pen `pen` (1..16) at 1:`scale`: the column of the nearest listed
    /// scale (in ratio: 1:75 takes 1:100 rather than 1:50 only when nearer). None: no such pen.
    pub fn model_mm(&self, pen: u8, scale: u32) -> Option<f32> {
        let row = self.model.get((pen as usize).checked_sub(1)?)?;
        let s = (scale.max(1)) as f64;
        let col = self
            .scales
            .iter()
            .enumerate()
            .min_by(|a, b| {
                let d = |x: u32| (s.ln() - (x.max(1) as f64).ln()).abs();
                d(*a.1).total_cmp(&d(*b.1))
            })
            .map(|(i, _)| i)?;
        row.get(col).copied()
    }

    pub fn annotation_mm(&self, pen: u8) -> Option<f32> {
        self.annotation.get((pen as usize).checked_sub(1)?).copied()
    }

    pub fn perspective_mm(&self, pen: u8) -> Option<f32> {
        self.perspective.get((pen as usize).checked_sub(1)?).copied()
    }

    /// Add a scale column (thicknesses copied from the nearest one); keeps the scales sorted.
    pub fn add_scale(&mut self, scale: u32) {
        if self.scales.contains(&scale) || self.scales.len() >= MAX_SCALES {
            return;
        }
        let copy: Vec<f32> = (1..=PENS as u8)
            .map(|p| self.model_mm(p, scale).unwrap_or(0.1))
            .collect();
        let at = self.scales.partition_point(|s| *s < scale);
        self.scales.insert(at, scale);
        for (row, v) in self.model.iter_mut().zip(copy) {
            row.insert(at, v);
        }
    }

    pub fn remove_scale(&mut self, scale: u32) {
        if self.scales.len() <= 1 {
            return;
        }
        if let Some(i) = self.scales.iter().position(|s| *s == scale) {
            self.scales.remove(i);
            for row in &mut self.model {
                if i < row.len() {
                    row.remove(i);
                }
            }
        }
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.scales.is_empty() || self.scales.len() > MAX_SCALES {
            return Err(format!("pens.scales: 1 to {MAX_SCALES} scales"));
        }
        if self.scales.windows(2).any(|w| w[0] >= w[1]) || self.scales.iter().any(|s| *s == 0)
        {
            return Err("pens.scales: increasing scales 1:N, N ≥ 1".into());
        }
        if self.model.len() != PENS || self.model.iter().any(|r| r.len() != self.scales.len()) {
            return Err(format!(
                "pens.model: {PENS} rows of {} thicknesses (one per scale)",
                self.scales.len()
            ));
        }
        if self.perspective.len() != PENS || self.annotation.len() != PENS {
            return Err(format!("pens.perspective and pens.annotation: {PENS} thicknesses each"));
        }
        let ok = |x: &f32| x.is_finite() && (0.01..=20.0).contains(x);
        if !self
            .model
            .iter()
            .flatten()
            .chain(&self.perspective)
            .chain(&self.annotation)
            .all(ok)
        {
            return Err("pens: thicknesses must be 0.01 to 20 mm".into());
        }
        Ok(())
    }
}

/// Which table a line type's pen is read from.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PenKind {
    /// Model lines: thickness depends on the drawing's scale.
    #[default]
    Model,
    /// Annotations: the same at every scale.
    Annotation,
}

/// A named line type: colour, dash pattern, pen.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LineType {
    pub name: String,
    pub color: Rgb,
    /// Dash, gap, dash, gap… in mm on paper; empty: continuous.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub dash_mm: Vec<f32>,
    /// Pen number 1..16.
    pub pen: u8,
}

/// The line type of each kind of line on a drawing (by name).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct LineRoles {
    /// Outline of what the cut plane cuts.
    pub cut: String,
    /// Edges seen beyond the cut.
    pub seen: String,
    /// Edges above the cut (beams over), dashed.
    pub above: String,
    /// Hatch lines (their colour stays the family's ink).
    pub hatch: String,
    /// Grid axes.
    pub axis: String,
    /// Dimension lines, ticks and leaders.
    pub dimension: String,
    /// Sheet frame and title block.
    pub frame: String,
}

impl Default for LineRoles {
    fn default() -> Self {
        LineRoles {
            cut: "black_continu_5".into(),
            seen: "black_continu_2".into(),
            above: "black_dash_1".into(),
            hatch: "black_continu_1".into(),
            axis: "black_axes_1".into(),
            dimension: "black_continu_1".into(),
            frame: "black_continu_5".into(),
        }
    }
}

impl LineRoles {
    pub fn all(&self) -> [(&'static str, &str); 7] {
        [
            ("cut", &self.cut),
            ("seen", &self.seen),
            ("above", &self.above),
            ("hatch", &self.hatch),
            ("axis", &self.axis),
            ("dimension", &self.dimension),
            ("frame", &self.frame),
        ]
    }
}

/// Dash patterns of the default line types, mm on paper.
pub const DASH: [f32; 2] = [2.0, 1.0];
pub const AXES: [f32; 4] = [12.0, 1.5, 0.5, 1.5];
pub const AXES_DETAIL: [f32; 4] = [3.0, 1.0, 0.5, 1.0];

/// The default line types: colour × pattern × pen, as in the office's list (#42).
pub fn default_line_types() -> Vec<LineType> {
    let colors: [(&str, Rgb); 9] = [
        ("black", Rgb([0, 0, 0])),
        ("red", Rgb([230, 40, 30])),
        ("grey", Rgb([128, 128, 128])),
        ("green", Rgb([40, 160, 40])),
        ("brown", Rgb([150, 90, 40])),
        ("orange", Rgb([240, 130, 40])),
        ("blue", Rgb([30, 110, 240])),
        ("pink", Rgb([230, 30, 230])),
        ("yellow", Rgb([230, 210, 20])),
    ];
    let mut v = Vec::new();
    for (c, rgb) in colors {
        for pen in [1u8, 2, 3, 5] {
            v.push(LineType {
                name: format!("{c}_continu_{pen}"),
                color: rgb,
                dash_mm: Vec::new(),
                pen,
            });
        }
        for pen in [1u8, 2, 3, 5] {
            v.push(LineType {
                name: format!("{c}_dash_{pen}"),
                color: rgb,
                dash_mm: DASH.to_vec(),
                pen,
            });
        }
        for pen in [1u8, 2] {
            v.push(LineType {
                name: format!("{c}_axes_{pen}"),
                color: rgb,
                dash_mm: AXES.to_vec(),
                pen,
            });
        }
        v.push(LineType {
            name: format!("{c}_axes_detail_1"),
            color: rgb,
            dash_mm: AXES_DETAIL.to_vec(),
            pen: 1,
        });
    }
    v
}

/// Checks line types and roles.
pub fn validate_lines(types: &[LineType], roles: &LineRoles) -> Result<(), String> {
    if types.len() > MAX_LINE_TYPES {
        return Err(format!("at most {MAX_LINE_TYPES} line types"));
    }
    let mut names = std::collections::BTreeSet::new();
    for t in types {
        if t.name.trim().is_empty() || t.name.chars().count() > 80 {
            return Err("a line type needs a name (at most 80 characters)".into());
        }
        if !names.insert(t.name.as_str()) {
            return Err(format!("two line types are named {}", t.name));
        }
        if !(1..=PENS as u8).contains(&t.pen) {
            return Err(format!("line type {}: pen must be 1 to {PENS}", t.name));
        }
        if t.dash_mm.len() > 16
            || t.dash_mm.len() % 2 == 1
            || t.dash_mm.iter().any(|d| !(d.is_finite() && (0.05..=100.0).contains(d)))
        {
            return Err(format!(
                "line type {}: dashes are pairs of lengths (dash, gap), 0.05 to 100 mm",
                t.name
            ));
        }
    }
    for (role, name) in roles.all() {
        if !names.contains(name) {
            return Err(format!("lines.{role}: no line type named {name:?}"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn thickness_follows_the_nearest_scale() {
        let t = PenTable::default();
        assert_eq!(t.model_mm(5, 100), Some(0.5));
        assert_eq!(t.model_mm(5, 20), Some(0.7));
        // 1:75 is nearer to 1:100 than to 1:50 in ratio (75/50 = 1.5 > 100/75 = 1.33).
        assert_eq!(t.model_mm(7, 75), Some(1.0));
        assert_eq!(t.model_mm(7, 1000), Some(0.5));
        assert_eq!(t.model_mm(17, 100), None);
        assert_eq!(t.model_mm(0, 100), None);
        assert_eq!(t.annotation_mm(1), Some(0.1));
        t.validate().unwrap();
    }

    #[test]
    fn scales_can_be_added_and_removed() {
        let mut t = PenTable::default();
        t.add_scale(75);
        assert_eq!(t.scales, vec![5, 10, 20, 50, 75, 100, 200, 500]);
        assert_eq!(t.model_mm(7, 75), Some(1.0));
        t.validate().unwrap();
        t.remove_scale(75);
        assert_eq!(t, PenTable::default());
    }

    #[test]
    fn default_line_types_cover_the_roles() {
        let v = default_line_types();
        validate_lines(&v, &LineRoles::default()).unwrap();
        assert!(v.iter().any(|t| t.name == "orange_dash_3"));
        let mut bad = LineRoles::default();
        bad.cut = "nope".into();
        assert!(validate_lines(&v, &bad).unwrap_err().contains("lines.cut"));
    }
}
