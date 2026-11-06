//! What goes on a plan sheet besides the model, and in which words: title block, legend,
//! drawing title, dimension unit, hidden classes. A template is a JSON file; every key is
//! optional and falls back to the built-in English template (`PlanTemplate::default()`).
//!
//! Texts may contain placeholders, replaced when the sheet is drawn:
//! `{project}`, `{storey}`, `{level}` (storey level, m, signed), `{scale}` (`1:100`), `{n}` (`100`),
//! `{paper}`, `{date}`, `{bimble}` (version), `{view}` (the view drawn, if any), `{drawing}` (the
//! view's name, else "Plan <storey>"), and any key of `fields` (`{phase}`, `{client}`, …).
//! Unknown placeholders are left as they are, so a typo shows on the sheet.

use bimble_core::style::Rgb;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct PlanTemplate {
    /// Drawing title under the plan, e.g. "Plan {storey}, éch 1 : {n}".
    pub title: String,
    /// Note above the title block, e.g. the storey level. Empty: none.
    pub level_note: String,
    /// Unit of dimension texts.
    pub dimension_unit: DimUnit,
    /// IFC classes left out of the drawing (case-insensitive), e.g. reinforcement.
    pub hidden_classes: Vec<String>,
    /// Fill what is seen below the cut (lighter than what is cut), painted from low to high.
    pub seen_fills: bool,
    /// Structural grid axes with tag bubbles.
    pub axes: Axes,
    /// Dimension chains along the grid axes and overall dimensions, top and left.
    pub dimensions: bool,
    /// Top-of-slab levels on the slabs seen below the cut.
    pub slab_levels: bool,
    /// Diagonal cross in rectangular openings of the slabs seen below the cut.
    pub opening_crosses: bool,
    pub legend: Legend,
    pub title_block: TitleBlock,
    /// Values for placeholders, e.g. {"phase": "Tender", "client": "…"}.
    pub fields: BTreeMap<String, String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DimUnit {
    Mm,
    Cm,
    M,
}

impl DimUnit {
    /// A length (mm) as written in a dimension: mm and cm as integers when whole, else one
    /// decimal (cm) or none (mm); m with two decimals.
    pub fn format(self, mm: f64) -> String {
        // `+ 0.0` turns -0 into 0.
        match self {
            DimUnit::Mm => format!("{}", mm.round() + 0.0),
            DimUnit::Cm => {
                let cm = mm.round() / 10.0 + 0.0;
                if cm == cm.round() {
                    format!("{cm}")
                } else {
                    format!("{cm:.1}")
                }
            }
            DimUnit::M => format!("{:.2}", (mm / 10.0).round() / 100.0 + 0.0),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Axes {
    Off,
    /// The model's IfcGrid axes only.
    Model,
    /// The model's grid axes, or, when it has none, axes through the columns cut by the plane.
    ModelOrColumns,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Legend {
    pub show: bool,
    pub title: String,
    /// Label of a family's cut and seen swatches; `{family}` is the family's name.
    pub cut: String,
    pub seen: String,
    /// Label of the dashed lines above the cut. Empty: not listed.
    pub above: String,
    /// Family names (Concrete, Steel, Timber, Masonry, Insulation, Earth, Glass, Other) in the
    /// sheet's language. Missing ones keep their English name.
    pub families: BTreeMap<String, String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct TitleBlock {
    /// Band height at the bottom of the frame, mm (10..=120).
    pub height_mm: f64,
    /// Columns left to right, each with one or more stacked entries.
    pub columns: Vec<Column>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Default)]
#[serde(default, deny_unknown_fields)]
pub struct Column {
    /// None: the columns without a width share what is left.
    pub width_mm: Option<f64>,
    pub entries: Vec<Entry>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Entry {
    /// Caption, in bold.
    pub label: String,
    /// Text under the caption; "\n" starts a new line.
    pub value: String,
    /// Colour of the value (e.g. a phase in red). None: the ink colour.
    pub color: Option<Rgb>,
    /// Value text size relative to the template's text size.
    #[serde(skip_serializing_if = "is_one")]
    pub size: f64,
}

fn is_one(v: &f64) -> bool {
    *v == 1.0
}

impl Default for Entry {
    fn default() -> Self {
        entry("", "")
    }
}

fn entry(label: &str, value: &str) -> Entry {
    Entry {
        label: label.into(),
        value: value.into(),
        color: None,
        size: 1.0,
    }
}

impl Default for Legend {
    fn default() -> Self {
        Legend {
            show: true,
            title: "Legend".into(),
            cut: "{family}, cut".into(),
            seen: "{family}, seen".into(),
            above: "Above the cut".into(),
            families: BTreeMap::new(),
        }
    }
}

impl Default for TitleBlock {
    fn default() -> Self {
        let col = |w: Option<f64>, e: Vec<Entry>| Column {
            width_mm: w,
            entries: e,
        };
        TitleBlock {
            height_mm: 28.0,
            columns: vec![
                col(None, vec![entry("PROJECT", "{project}")]),
                col(Some(60.0), vec![entry("DRAWING", "{drawing}")]),
                col(
                    Some(30.0),
                    vec![entry("SCALE", "{scale}"), entry("FORMAT", "{paper}")],
                ),
                col(
                    Some(30.0),
                    vec![entry("DATE", "{date}"), entry("REVISION", "")],
                ),
                col(Some(24.0), vec![entry("DRAWN", ""), entry("CHECKED", "")]),
                col(Some(40.0), vec![entry("DRAWING N°", "{drawing_no}")]),
            ],
        }
    }
}

impl Default for PlanTemplate {
    fn default() -> Self {
        PlanTemplate {
            title: "Plan {storey}, {scale}".into(),
            level_note: "{storey}: {level} m".into(),
            dimension_unit: DimUnit::Cm,
            hidden_classes: [
                "IfcReinforcingBar",
                "IfcReinforcingMesh",
                "IfcTendon",
                "IfcTendonAnchor",
                "IfcSpace",
                "IfcOpeningElement",
                "IfcAnnotation",
                "IfcGeographicElement",
            ]
            .map(String::from)
            .to_vec(),
            seen_fills: true,
            axes: Axes::ModelOrColumns,
            dimensions: true,
            slab_levels: true,
            opening_crosses: true,
            legend: Legend::default(),
            title_block: TitleBlock::default(),
            fields: BTreeMap::new(),
        }
    }
}

impl PlanTemplate {
    /// Parse a template file and check its sizes.
    pub fn from_json(s: &str) -> Result<PlanTemplate, String> {
        let t: PlanTemplate = serde_json::from_str(s).map_err(|e| e.to_string())?;
        t.check()?;
        Ok(t)
    }

    /// Read a template file (at most 1 MB).
    pub fn load(path: &std::path::Path) -> Result<PlanTemplate, String> {
        let err = |e: String| format!("{}: {e}", path.display());
        let meta = std::fs::metadata(path).map_err(|e| err(e.to_string()))?;
        if meta.len() > 1_000_000 {
            return Err(err("larger than 1 MB".into()));
        }
        let text = std::fs::read_to_string(path).map_err(|e| err(e.to_string()))?;
        PlanTemplate::from_json(&text).map_err(err)
    }

    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).unwrap_or_default()
    }

    pub fn check(&self) -> Result<(), String> {
        let tb = &self.title_block;
        if !(10.0..=120.0).contains(&tb.height_mm) {
            return Err("title_block.height_mm must be between 10 and 120".into());
        }
        if tb.columns.len() > 40 || tb.columns.iter().any(|c| c.entries.len() > 8) {
            return Err("title block: at most 40 columns of 8 entries".into());
        }
        for c in &tb.columns {
            if let Some(w) = c.width_mm
                && !(w.is_finite() && (1.0..=1000.0).contains(&w))
            {
                return Err("title block column widths must be between 1 and 1000 mm".into());
            }
            for e in &c.entries {
                if !(e.size.is_finite() && (0.2..=5.0).contains(&e.size)) {
                    return Err("title block entry size must be between 0.2 and 5".into());
                }
            }
        }
        Ok(())
    }

    /// Family name in the template's language.
    pub fn family_name(&self, english: &str) -> String {
        self.legend
            .families
            .get(english)
            .cloned()
            .unwrap_or_else(|| english.to_owned())
    }

    pub fn hides(&self, class: &str) -> bool {
        self.hidden_classes
            .iter()
            .any(|c| c.eq_ignore_ascii_case(class))
    }
}

/// Replace `{key}` placeholders from `values` (built-ins first, then `fields`).
pub fn fill(text: &str, values: &[(&str, String)], fields: &BTreeMap<String, String>) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(i) = rest.find('{') {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        match after.find('}') {
            Some(j) => {
                let key = &after[..j];
                let v = values
                    .iter()
                    .find(|(k, _)| *k == key)
                    .map(|(_, v)| v.as_str())
                    .or_else(|| fields.get(key).map(String::as_str));
                match v {
                    Some(v) => out.push_str(v),
                    None => {
                        out.push('{');
                        out.push_str(key);
                        out.push('}');
                    }
                }
                rest = &after[j + 1..];
            }
            None => {
                out.push_str(&rest[i..]);
                rest = "";
            }
        }
    }
    out.push_str(rest);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_template_round_trips_and_partial_files_fall_back_to_it() {
        let d = PlanTemplate::default();
        assert_eq!(PlanTemplate::from_json(&d.to_json()).unwrap(), d);
        let t = PlanTemplate::from_json(
            r#"{"title": "Plan {storey}, éch 1 : {n}", "legend": {"families": {"Concrete": "Béton"}}}"#,
        )
        .unwrap();
        assert_eq!(t.title, "Plan {storey}, éch 1 : {n}");
        assert_eq!(t.legend.cut, d.legend.cut);
        assert_eq!(t.family_name("Concrete"), "Béton");
        assert_eq!(t.family_name("Steel"), "Steel");
        assert!(t.hides("ifcreinforcingbar"));
    }

    #[test]
    fn bad_templates_are_rejected_with_a_reason() {
        assert!(
            PlanTemplate::from_json(r#"{"titel": "x"}"#)
                .unwrap_err()
                .contains("titel")
        );
        assert!(
            PlanTemplate::from_json(r#"{"title_block": {"height_mm": 1e9}}"#)
                .unwrap_err()
                .contains("height_mm")
        );
        assert!(
            PlanTemplate::from_json(
                r#"{"title_block": {"columns": [{"width_mm": -3, "entries": []}]}}"#
            )
            .is_err()
        );
    }

    #[test]
    fn placeholders_are_filled_and_unknown_ones_kept() {
        let mut f = BTreeMap::new();
        f.insert("phase".to_string(), "Tender".to_string());
        let v = [("storey", "N01".to_string()), ("n", "100".to_string())];
        assert_eq!(
            fill("Plan {storey}, 1 : {n} – {phase} {nope} {", &v, &f),
            "Plan N01, 1 : 100 – Tender {nope} {"
        );
    }

    #[test]
    fn dimension_texts() {
        assert_eq!(DimUnit::Cm.format(3620.0), "362");
        assert_eq!(DimUnit::Cm.format(125.0), "12.5");
        assert_eq!(DimUnit::Cm.format(119.96), "12");
        assert_eq!(DimUnit::Mm.format(3620.4), "3620");
        assert_eq!(DimUnit::M.format(3620.0), "3.62");
        assert_eq!(DimUnit::Cm.format(-0.2), "0");
    }
}
