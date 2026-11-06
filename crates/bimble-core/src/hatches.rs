//! Hatch types (#44): named fill patterns drawn on paper, and which one each material family
//! uses where it is cut and where it is seen.
//!
//! A hatch type is a drafting pattern: its spacing is in mm on paper, the same at every scale.
//! It has an optional solid fill under its lines, a colour and a pen number (its width comes from
//! the model pens at the drawing's scale). A material without a hatch type keeps the pattern of
//! its family (the screen pattern, scaled to paper).

use crate::MaterialCategory;
use crate::style::Rgb;
use serde::{Deserialize, Serialize};

/// At most this many hatch types, and this many line families in one.
pub const MAX_HATCH_TYPES: usize = 500;
pub const MAX_HATCH_LINES: usize = 16;

/// One family of parallel lines.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HatchLine {
    /// Direction, degrees counter-clockwise from horizontal.
    pub angle_deg: f32,
    /// Distance between lines, mm on paper.
    pub spacing_mm: f32,
    /// Shift across the lines, mm on paper (to group lines: earth is three families 0.5 mm apart).
    #[serde(default, skip_serializing_if = "is_zero")]
    pub offset_mm: f32,
    /// Dash, gap, dash, gap… in mm on paper. Empty: continuous.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub dash_mm: Vec<f32>,
}

fn is_zero(x: &f32) -> bool {
    *x == 0.0
}

/// A named hatch: an optional solid fill, then line families in one colour and pen.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HatchType {
    pub name: String,
    /// Solid fill under the lines. Absent: white, like the paper (it still hides what is under).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fill: Option<Rgb>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub lines: Vec<HatchLine>,
    pub color: Rgb,
    /// Model pen number (1..=16) of its lines.
    pub pen: u8,
}

/// Which hatch type a material family uses where it is cut and where it is seen from above.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MaterialHatch {
    pub category: MaterialCategory,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cut: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seen: Option<String>,
}

fn line(angle_deg: f32, spacing_mm: f32) -> HatchLine {
    HatchLine {
        angle_deg,
        spacing_mm,
        offset_mm: 0.0,
        dash_mm: Vec::new(),
    }
}

fn hatch(name: &str, fill: Option<Rgb>, lines: Vec<HatchLine>, color: Rgb) -> HatchType {
    HatchType {
        name: name.into(),
        fill,
        lines,
        color,
        pen: 1,
    }
}

const BLACK: Rgb = Rgb([0, 0, 0]);

/// The office list, without its prefix: one cut and one seen pattern per material, earth,
/// drainage, insulation, diagonals in five colours, solid fills.
pub fn default_hatch_types() -> Vec<HatchType> {
    let mut v = vec![
        hatch("concrete cut", None, vec![line(45.0, 1.5)], BLACK),
        hatch(
            "concrete seen",
            Some(Rgb([235, 235, 235])),
            Vec::new(),
            BLACK,
        ),
        hatch(
            "masonry cut",
            None,
            vec![line(45.0, 1.0), line(135.0, 3.0)],
            BLACK,
        ),
        hatch("masonry seen", None, vec![line(45.0, 3.0)], BLACK),
        hatch("steel cut", Some(BLACK), Vec::new(), BLACK),
        hatch(
            "timber cut",
            None,
            vec![line(45.0, 2.0), line(135.0, 2.0)],
            BLACK,
        ),
        hatch("timber seen", None, vec![line(0.0, 2.0)], BLACK),
        hatch(
            "earth",
            None,
            [0.0f32, 0.5, 1.0]
                .into_iter()
                .map(|o| HatchLine {
                    offset_mm: o,
                    ..line(45.0, 4.0)
                })
                .collect(),
            BLACK,
        ),
        hatch(
            "drainage",
            None,
            vec![
                HatchLine {
                    dash_mm: vec![0.2, 1.0],
                    ..line(0.0, 1.2)
                },
                HatchLine {
                    offset_mm: 0.6,
                    dash_mm: vec![0.2, 1.0],
                    ..line(0.0, 1.2)
                },
            ],
            BLACK,
        ),
        hatch(
            "insulation",
            None,
            vec![
                HatchLine {
                    dash_mm: vec![1.0, 1.0],
                    ..line(45.0, 3.0)
                },
                HatchLine {
                    dash_mm: vec![1.0, 1.0],
                    ..line(135.0, 3.0)
                },
            ],
            BLACK,
        ),
    ];
    for (c, rgb) in [
        ("black", BLACK),
        ("red", Rgb([230, 40, 30])),
        ("green", Rgb([40, 160, 40])),
        ("brown", Rgb([190, 110, 40])),
        ("blue", Rgb([30, 110, 240])),
    ] {
        v.push(hatch(
            &format!("diagonals {c}"),
            None,
            vec![line(45.0, 2.0)],
            rgb,
        ));
    }
    for (c, rgb) in [
        ("black", BLACK),
        ("grey", Rgb([190, 190, 190])),
        ("light grey", Rgb([225, 225, 225])),
    ] {
        v.push(hatch(&format!("solid {c}"), Some(rgb), Vec::new(), BLACK));
    }
    v
}

/// Checks hatch types and which materials use them.
pub fn validate_hatches(types: &[HatchType], uses: &[MaterialHatch]) -> Result<(), String> {
    if types.len() > MAX_HATCH_TYPES {
        return Err(format!("at most {MAX_HATCH_TYPES} hatch types"));
    }
    let mut names = std::collections::BTreeSet::new();
    for t in types {
        if t.name.trim().is_empty() || !names.insert(t.name.as_str()) {
            return Err(format!("hatch type {:?}: a unique name is needed", t.name));
        }
        if !(1..=16).contains(&t.pen) {
            return Err(format!("hatch type {}: pen 1 to 16", t.name));
        }
        if t.lines.len() > MAX_HATCH_LINES {
            return Err(format!(
                "hatch type {}: at most {MAX_HATCH_LINES} line families",
                t.name
            ));
        }
        for l in &t.lines {
            let ok = l.angle_deg.is_finite()
                && l.offset_mm.is_finite()
                && l.spacing_mm.is_finite()
                // Finer than 0.1 mm on paper is a solid fill, and costs a line per 0.1 mm.
                && (0.1..=1000.0).contains(&l.spacing_mm)
                && l.dash_mm.len() % 2 == 0
                && l.dash_mm.len() <= 16
                && l.dash_mm.iter().all(|d| d.is_finite() && *d >= 0.0)
                && (l.dash_mm.is_empty() || l.dash_mm.iter().sum::<f32>() > 0.0);
            if !ok {
                return Err(format!(
                    "hatch type {}: spacing 0.1 to 1000 mm, finite angle and offset, dashes in pairs",
                    t.name
                ));
            }
        }
    }
    let mut seen = std::collections::BTreeSet::new();
    for u in uses {
        if !seen.insert(u.category) {
            return Err(format!("{}: hatches given twice", u.category.label()));
        }
        for n in [&u.cut, &u.seen].into_iter().flatten() {
            if !names.contains(n.as_str()) {
                return Err(format!("{}: no hatch type named {n:?}", u.category.label()));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_valid_and_cover_the_list() {
        let v = default_hatch_types();
        validate_hatches(&v, &[]).unwrap();
        for n in [
            "earth",
            "drainage",
            "insulation",
            "diagonals blue",
            "solid grey",
            "concrete cut",
        ] {
            assert!(v.iter().any(|t| t.name == n), "{n}");
        }
    }

    #[test]
    fn bad_hatches_are_refused() {
        let mut v = default_hatch_types();
        v[0].lines[0].spacing_mm = 0.0;
        assert!(validate_hatches(&v, &[]).is_err());
        let v = default_hatch_types();
        let uses = vec![MaterialHatch {
            category: MaterialCategory::Concrete,
            cut: Some("nope".into()),
            seen: None,
        }];
        assert!(validate_hatches(&v, &uses).is_err());
        let uses = vec![
            MaterialHatch {
                category: MaterialCategory::Concrete,
                cut: None,
                seen: None,
            };
            2
        ];
        assert!(validate_hatches(&v, &uses).is_err());
    }
}
