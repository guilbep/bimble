//! Graphic style: every colour, hatch and lighting parameter of a drawing, as data.
//!
//! A style is a JSON file. It only needs the values that differ from the default, e.g. red edges:
//!
//! ```json
//! { "edges": "#d01010" }
//! ```
//!
//! `bimble style` prints the complete default, which is the reference for every field.
//! Files are untrusted input: values are range-checked and unknown fields are refused, so a typo
//! is reported instead of silently ignored.

use crate::MaterialCategory;
use serde::{Deserialize, Serialize};

/// An sRGB colour, written `"#rrggbb"` in files.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub [u8; 3]);

impl Rgb {
    pub fn to_hex(self) -> String {
        format!("#{:02x}{:02x}{:02x}", self.0[0], self.0[1], self.0[2])
    }
    pub fn parse(s: &str) -> Result<Rgb, String> {
        let h = s
            .strip_prefix('#')
            .filter(|h| h.len() == 6 && h.bytes().all(|b| b.is_ascii_hexdigit()))
            .ok_or_else(|| format!("colour {s:?}: expected \"#rrggbb\""))?;
        let c = |i: usize| u8::from_str_radix(&h[i..i + 2], 16).unwrap();
        Ok(Rgb([c(0), c(2), c(4)]))
    }
    /// 0..1 per channel, still sRGB-encoded.
    pub fn unit(self) -> [f32; 3] {
        self.0.map(|c| c as f32 / 255.0)
    }
}

impl Serialize for Rgb {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_hex())
    }
}

impl<'de> Deserialize<'de> for Rgb {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        Rgb::parse(&s).map_err(serde::de::Error::custom)
    }
}

/// A family of parallel lines, sized in screen pixels (drawings read the same at every zoom).
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Lines {
    /// Direction of the lines, degrees counter-clockwise from horizontal.
    pub angle_deg: f32,
    /// Distance between lines, pixels.
    pub spacing_px: f32,
    /// Line thickness as a fraction of the spacing (0..1).
    pub width: f32,
    /// Dashes along the lines: period in pixels, or absent for continuous lines.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dash_px: Option<f32>,
    /// Fraction of each dash period that is drawn (0..1).
    #[serde(default = "one", skip_serializing_if = "is_one")]
    pub dash_on: f32,
}

/// Random dots on a pixel grid.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Stipple {
    /// Grid cell size, pixels.
    pub cell_px: f32,
    /// Fraction of cells with a dot (0..1).
    pub density: f32,
    /// Ink of a dot relative to a line (0..1).
    pub strength: f32,
}

/// How faces of one material family are drawn.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Fill {
    pub color: Rgb,
    /// Up to two line families (e.g. a cross-hatch). An empty list draws no lines.
    #[serde(with = "two_lines")]
    pub lines: [Option<Lines>; 2],
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stipple: Option<Stipple>,
    /// Colour of the hatch ink. Absent: the fill colour, darkened.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ink: Option<Rgb>,
}

/// One `Fill` per material family.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Families {
    pub concrete: Fill,
    pub steel: Fill,
    pub timber: Fill,
    pub masonry: Fill,
    pub insulation: Fill,
    pub earth: Fill,
    pub glass: Fill,
    pub other: Fill,
}

impl Families {
    pub fn get(&self, c: MaterialCategory) -> &Fill {
        match c {
            MaterialCategory::Concrete => &self.concrete,
            MaterialCategory::Steel => &self.steel,
            MaterialCategory::Timber => &self.timber,
            MaterialCategory::Masonry => &self.masonry,
            MaterialCategory::Insulation => &self.insulation,
            MaterialCategory::Earth => &self.earth,
            MaterialCategory::Glass => &self.glass,
            MaterialCategory::Other => &self.other,
        }
    }
    pub fn get_mut(&mut self, c: MaterialCategory) -> &mut Fill {
        match c {
            MaterialCategory::Concrete => &mut self.concrete,
            MaterialCategory::Steel => &mut self.steel,
            MaterialCategory::Timber => &mut self.timber,
            MaterialCategory::Masonry => &mut self.masonry,
            MaterialCategory::Insulation => &mut self.insulation,
            MaterialCategory::Earth => &mut self.earth,
            MaterialCategory::Glass => &mut self.glass,
            MaterialCategory::Other => &mut self.other,
        }
    }
}

/// Everything that decides how a drawing looks.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GraphicStyle {
    pub background: Rgb,
    /// Colour of edges (outlines and creases).
    pub edges: Rgb,
    /// Fill of selected elements (drawn without hatch).
    pub selection: Rgb,
    /// Edges of the element under the cursor (what a click would select).
    pub hover: Rgb,
    /// Dimension lines and their text (measuring).
    pub dimensions: Rgb,
    /// A linked (reference) model: its faces and its edges.
    pub reference: Rgb,
    pub reference_edges: Rgb,
    /// Comparing with a linked model: our elements it doesn't have, and its elements we don't.
    pub only_here: Rgb,
    pub only_in_reference: Rgb,
    /// Overall hatch intensity, 0 (none) .. 1.
    pub hatch_strength: f32,
    /// How much darker hatch ink is than the fill when a family sets no `ink` (0..1).
    pub hatch_darken: f32,
    /// Share of light that does not depend on the face orientation (0..1). 1 = flat colours.
    pub ambient: f32,
    /// Brightness of surfaces cut by the clipping box, relative to their fill (0..1).
    pub cut_shade: f32,
    pub families: Families,
    /// Drawings on paper (plans, sections): pens and hatch sizes in mm on the sheet.
    pub paper: PaperStyle,
}

/// Line weights and sizes for drawings on paper, in mm on the printed sheet.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PaperStyle {
    /// Outline of what the cut plane cuts (walls, columns, slabs).
    pub cut_pen_mm: f32,
    /// Visible edges below the cut plane.
    pub projection_pen_mm: f32,
    /// Edges above the cut plane (beams over), dashed.
    pub above_pen_mm: f32,
    /// Dash and gap length of lines above the cut plane.
    pub above_dash_mm: [f32; 2],
    /// Hatch lines.
    pub hatch_pen_mm: f32,
    /// Hatch spacing on paper per screen pixel of the family's `spacing_px`.
    pub hatch_mm_per_px: f32,
    /// Sheet frame and title block lines.
    pub frame_pen_mm: f32,
    /// Text height in the title block.
    pub text_mm: f32,
    /// Dimension lines (and grid axes in styles written before `axis_pen_mm`).
    pub annotation_pen_mm: f32,
    /// Grid axes: pen, and dash pattern (dash, gap, dot, gap…; any even number of lengths), the
    /// dash-dot of axes on office drawings.
    #[serde(default = "axis_pen")]
    pub axis_pen_mm: f32,
    #[serde(default = "axis_dash")]
    pub axis_dash_mm: [f32; 4],
    /// What is seen below the cut is filled with the family colour mixed with this much white.
    pub seen_lighten: f32,
    /// Pen thicknesses per pen number (and per scale for model lines), #42.
    #[serde(default)]
    pub pens: crate::pens::PenTable,
    /// Named line types: colour, dash pattern, pen.
    #[serde(default = "crate::pens::default_line_types")]
    pub line_types: Vec<crate::pens::LineType>,
    /// The line type of each kind of line (cut, seen, above, hatch, axis, dimension, frame).
    /// When set, they replace the `*_pen_mm` and dash fields above.
    #[serde(default)]
    pub lines: crate::pens::LineRoles,
}

impl PaperStyle {
    /// A line type by name.
    pub fn line_type(&self, name: &str) -> Option<&crate::pens::LineType> {
        self.line_types.iter().find(|t| t.name == name)
    }
}

fn axis_pen() -> f32 {
    0.13
}
fn axis_dash() -> [f32; 4] {
    [12.0, 1.5, 0.5, 1.5]
}
fn one() -> f32 {
    1.0
}
fn is_one(x: &f32) -> bool {
    *x == 1.0
}

/// `lines` is a JSON list of 0..2 families; stored as a fixed pair so the style stays `Copy`.
mod two_lines {
    use super::Lines;
    use serde::{Deserialize, Deserializer, Serialize, Serializer};
    pub fn serialize<S: Serializer>(v: &[Option<Lines>; 2], s: S) -> Result<S::Ok, S::Error> {
        v.iter().flatten().collect::<Vec<_>>().serialize(s)
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<[Option<Lines>; 2], D::Error> {
        let v = Vec::<Lines>::deserialize(d)?;
        if v.len() > 2 {
            return Err(serde::de::Error::custom(
                "at most two line families per fill",
            ));
        }
        Ok([v.first().copied(), v.get(1).copied()])
    }
}

const fn rgb(r: u8, g: u8, b: u8) -> Rgb {
    Rgb([r, g, b])
}

const fn lines(angle_deg: f32, spacing_px: f32, width: f32) -> Option<Lines> {
    Some(Lines {
        angle_deg,
        spacing_px,
        width,
        dash_px: None,
        dash_on: 1.0,
    })
}

/// A fill without hatch.
pub const fn plain(color: Rgb) -> Fill {
    Fill {
        color,
        lines: [None, None],
        stipple: None,
        ink: None,
    }
}

impl Default for GraphicStyle {
    /// The built-in drawing style: muted fills, near-black edges, one hatch per family.
    fn default() -> Self {
        // Spacings are the distances between lines measured across them (pixels), so a line
        // family at 45° with spacing 9/√2 repeats every 9 px along x.
        let s2 = std::f32::consts::SQRT_2;
        GraphicStyle {
            background: rgb(247, 247, 245),
            edges: rgb(26, 26, 28),
            selection: rgb(232, 126, 4),
            hover: rgb(30, 144, 255),
            dimensions: rgb(20, 110, 220),
            reference: rgb(190, 214, 238),
            reference_edges: rgb(52, 110, 180),
            only_here: rgb(224, 75, 58),
            only_in_reference: rgb(46, 160, 90),
            hatch_strength: 0.5,
            hatch_darken: 0.45,
            ambient: 0.62,
            cut_shade: 0.72,
            paper: PaperStyle {
                cut_pen_mm: 0.5,
                projection_pen_mm: 0.18,
                above_pen_mm: 0.13,
                above_dash_mm: [2.0, 1.0],
                hatch_pen_mm: 0.13,
                hatch_mm_per_px: 0.35,
                frame_pen_mm: 0.5,
                text_mm: 2.5,
                annotation_pen_mm: 0.13,
                axis_pen_mm: axis_pen(),
                axis_dash_mm: axis_dash(),
                seen_lighten: 0.6,
                pens: Default::default(),
                line_types: crate::pens::default_line_types(),
                lines: Default::default(),
            },
            families: Families {
                concrete: Fill {
                    stipple: Some(Stipple {
                        cell_px: 0.625,
                        density: 0.07,
                        strength: 0.8,
                    }),
                    ..plain(rgb(214, 214, 210))
                },
                steel: Fill {
                    lines: [lines(135.0, 5.0 / s2, 0.12), None],
                    ..plain(rgb(150, 170, 196))
                },
                timber: Fill {
                    lines: [lines(45.0, 9.0 / s2, 0.1), None],
                    ..plain(rgb(226, 196, 150))
                },
                masonry: Fill {
                    lines: [lines(45.0, 12.0 / s2, 0.08), lines(135.0, 24.0 / s2, 0.05)],
                    ..plain(rgb(206, 150, 128))
                },
                insulation: Fill {
                    lines: [
                        Some(Lines {
                            angle_deg: 0.0,
                            spacing_px: 8.0,
                            width: 0.12,
                            dash_px: Some(10.0),
                            dash_on: 0.6,
                        }),
                        None,
                    ],
                    ..plain(rgb(236, 220, 120))
                },
                earth: Fill {
                    lines: [lines(45.0, 10.0 / s2, 0.08), lines(135.0, 10.0 / s2, 0.08)],
                    ..plain(rgb(176, 160, 132))
                },
                glass: plain(rgb(176, 214, 226)),
                other: plain(rgb(238, 238, 236)),
            },
        }
    }
}

/// Largest style file accepted.
pub const MAX_STYLE_BYTES: usize = 1 << 20;

impl GraphicStyle {
    /// Reads a style file: the values it gives, on top of the default.
    pub fn from_json(text: &str) -> Result<GraphicStyle, String> {
        if text.len() > MAX_STYLE_BYTES {
            return Err(format!("style file larger than {MAX_STYLE_BYTES} bytes"));
        }
        let user: serde_json::Value = serde_json::from_str(text).map_err(|e| e.to_string())?;
        if !user.is_object() {
            return Err("a style file is a JSON object".into());
        }
        let mut merged = serde_json::to_value(GraphicStyle::default()).unwrap();
        merge(&mut merged, user);
        let s: GraphicStyle = serde_json::from_value(merged).map_err(|e| e.to_string())?;
        s.validate()?;
        Ok(s)
    }

    pub fn load(path: &std::path::Path) -> Result<GraphicStyle, String> {
        let meta = std::fs::metadata(path).map_err(|e| format!("{}: {e}", path.display()))?;
        if meta.len() > MAX_STYLE_BYTES as u64 {
            return Err(format!(
                "{}: larger than {MAX_STYLE_BYTES} bytes",
                path.display()
            ));
        }
        let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
        GraphicStyle::from_json(&text).map_err(|e| format!("{}: {e}", path.display()))
    }

    /// The complete style as pretty JSON (all fields).
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).unwrap() + "\n"
    }

    /// Only the values that differ from the default: what a user would write by hand.
    pub fn to_json_diff(&self) -> String {
        let mine = serde_json::to_value(self).unwrap();
        let base = serde_json::to_value(GraphicStyle::default()).unwrap();
        let d = diff(&mine, &base).unwrap_or(serde_json::json!({}));
        serde_json::to_string_pretty(&d).unwrap() + "\n"
    }

    pub fn validate(&self) -> Result<(), String> {
        let unit = |name: &str, x: f32| {
            if (0.0..=1.0).contains(&x) {
                Ok(())
            } else {
                Err(format!("{name} = {x}: must be between 0 and 1"))
            }
        };
        unit("hatch_strength", self.hatch_strength)?;
        unit("hatch_darken", self.hatch_darken)?;
        unit("ambient", self.ambient)?;
        unit("cut_shade", self.cut_shade)?;
        let p = &self.paper;
        let range = |name: &str, x: f32, lo: f32, hi: f32| {
            if x.is_finite() && (lo..=hi).contains(&x) {
                Ok(())
            } else {
                Err(format!("paper.{name} = {x}: must be between {lo} and {hi}"))
            }
        };
        for (n, x) in [
            ("cut_pen_mm", p.cut_pen_mm),
            ("projection_pen_mm", p.projection_pen_mm),
            ("above_pen_mm", p.above_pen_mm),
            ("hatch_pen_mm", p.hatch_pen_mm),
            ("frame_pen_mm", p.frame_pen_mm),
            ("annotation_pen_mm", p.annotation_pen_mm),
            ("axis_pen_mm", p.axis_pen_mm),
        ] {
            range(n, x, 0.05, 5.0)?;
        }
        range("above_dash_mm[0]", p.above_dash_mm[0], 0.1, 50.0)?;
        range("above_dash_mm[1]", p.above_dash_mm[1], 0.1, 50.0)?;
        for (k, x) in p.axis_dash_mm.iter().enumerate() {
            range(&format!("axis_dash_mm[{k}]"), *x, 0.1, 100.0)?;
        }
        range("hatch_mm_per_px", p.hatch_mm_per_px, 0.05, 10.0)?;
        range("text_mm", p.text_mm, 1.0, 20.0)?;
        range("seen_lighten", p.seen_lighten, 0.0, 1.0)?;
        p.pens.validate()?;
        crate::pens::validate_lines(&p.line_types, &p.lines)?;
        for c in MaterialCategory::ALL {
            let f = self.families.get(c);
            let at = |field: &str| format!("families.{}.{field}", c.label().to_lowercase());
            for l in f.lines.iter().flatten() {
                if !l.angle_deg.is_finite() {
                    return Err(format!("{}: not a number", at("lines.angle_deg")));
                }
                if !(1.0..=1000.0).contains(&l.spacing_px) {
                    return Err(format!(
                        "{} = {}: must be between 1 and 1000 pixels",
                        at("lines.spacing_px"),
                        l.spacing_px
                    ));
                }
                unit(&at("lines.width"), l.width)?;
                unit(&at("lines.dash_on"), l.dash_on)?;
                if let Some(d) = l.dash_px
                    && !(1.0..=1000.0).contains(&d)
                {
                    return Err(format!(
                        "{} = {d}: must be between 1 and 1000 pixels",
                        at("lines.dash_px")
                    ));
                }
            }
            if let Some(s) = f.stipple {
                if !(0.25..=1000.0).contains(&s.cell_px) {
                    return Err(format!(
                        "{} = {}: must be between 0.25 and 1000 pixels",
                        at("stipple.cell_px"),
                        s.cell_px
                    ));
                }
                unit(&at("stipple.density"), s.density)?;
                unit(&at("stipple.strength"), s.strength)?;
            }
        }
        Ok(())
    }
}

/// Deep merge: objects are merged key by key, anything else replaces.
fn merge(base: &mut serde_json::Value, top: serde_json::Value) {
    match (base, top) {
        (serde_json::Value::Object(b), serde_json::Value::Object(t)) => {
            for (k, v) in t {
                match b.get_mut(&k) {
                    Some(slot) => merge(slot, v),
                    None => {
                        b.insert(k, v);
                    }
                }
            }
        }
        (b, t) => *b = t,
    }
}

/// The parts of `a` that differ from `b` (objects key by key).
fn diff(a: &serde_json::Value, b: &serde_json::Value) -> Option<serde_json::Value> {
    match (a, b) {
        (serde_json::Value::Object(x), serde_json::Value::Object(y)) => {
            let m: serde_json::Map<_, _> = x
                .iter()
                .filter_map(|(k, v)| match y.get(k) {
                    Some(w) => diff(v, w).map(|d| (k.clone(), d)),
                    None => Some((k.clone(), v.clone())),
                })
                .collect();
            (!m.is_empty()).then_some(serde_json::Value::Object(m))
        }
        _ => (a != b).then(|| a.clone()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_round_trips_through_json() {
        let d = GraphicStyle::default();
        assert_eq!(GraphicStyle::from_json(&d.to_json()).unwrap(), d);
        assert_eq!(d.to_json_diff().trim(), "{}");
        d.validate().unwrap();
    }

    #[test]
    fn partial_file_overrides_only_what_it_says() {
        let s = GraphicStyle::from_json(
            r##"{ "edges": "#d01010", "families": { "concrete": { "color": "#808080" } } }"##,
        )
        .unwrap();
        let d = GraphicStyle::default();
        assert_eq!(s.edges, Rgb([0xd0, 0x10, 0x10]));
        assert_eq!(s.families.concrete.color, Rgb([128, 128, 128]));
        // Everything else, including concrete's stipple, keeps the default.
        assert_eq!(s.families.concrete.stipple, d.families.concrete.stipple);
        assert_eq!(s.families.steel, d.families.steel);
        assert_eq!(s.background, d.background);
        assert_eq!(
            s.to_json_diff(),
            "{\n  \"edges\": \"#d01010\",\n  \"families\": {\n    \"concrete\": {\n      \"color\": \"#808080\"\n    }\n  }\n}\n"
        );
    }

    #[test]
    fn bad_files_are_reported() {
        for (text, needle) in [
            (r#"{ "edges": "red" }"#, "#rrggbb"),
            (r##"{ "egdes": "#ff0000" }"##, "unknown field"),
            (r#"{ "ambient": 2 }"#, "ambient"),
            (
                r#"{ "paper": { "axis_dash_mm": [0, 1.5, 0.5, 1.5] } }"#,
                "axis_dash_mm[0]",
            ),
            (r#"{ "hatch_strength": -1 }"#, "hatch_strength"),
            (r#"[1, 2]"#, "JSON object"),
            (
                r#"{ "families": { "steel": { "lines": [ {"angle_deg": 0, "spacing_px": 0, "width": 0.1} ] } } }"#,
                "spacing_px",
            ),
            (
                r#"{ "families": { "steel": { "lines": [ {"angle_deg": 0, "spacing_px": 5, "width": 0.1}, {"angle_deg": 0, "spacing_px": 5, "width": 0.1}, {"angle_deg": 0, "spacing_px": 5, "width": 0.1} ] } } }"#,
                "at most two",
            ),
        ] {
            let e = GraphicStyle::from_json(text).unwrap_err();
            assert!(e.contains(needle), "{text}: {e}");
        }
        let big = format!("{{\"x\": \"{}\"}}", "a".repeat(MAX_STYLE_BYTES));
        assert!(
            GraphicStyle::from_json(&big)
                .unwrap_err()
                .contains("larger")
        );
    }

    #[test]
    fn colour_parsing() {
        assert_eq!(Rgb::parse("#0aFf10").unwrap(), Rgb([10, 255, 16]));
        for bad in ["0aff10", "#0aff1", "#0aff1g", "#0aff100", ""] {
            assert!(Rgb::parse(bad).is_err(), "{bad}");
        }
    }
}
