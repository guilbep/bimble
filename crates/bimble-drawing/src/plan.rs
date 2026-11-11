//! Storey plans at scale.
//!
//! A plan is a horizontal cut at `cut_above_mm` over the storey level, seen from above:
//! - what the plane cuts: filled, hatched like its material, outlined with the cut pen;
//! - what is seen below the cut, down to the storey level: its top filled lighter than what
//!   is cut, painted from the lowest to the highest element so higher tops hide what is under
//!   them (a painter's hidden-line removal), with its visible edges in thin lines;
//! - edges of the storey's own elements above the cut, up to the next storey: dashed.
//!
//! Around the drawing, as a template says (see `template`): grid axes with bubbles, dimension
//! chains (axis to axis, then overall), slab levels, opening crosses, a legend, the drawing
//! title, a level note and a title block.

use crate::cut::{P2, clip_z, edge_seen_from_above, facing_up_region, section};
use crate::sheet::{Anchor, Hatch, Item, Paper, Pen, Sheet, text_width_mm};
use crate::template::{Axes, PlanTemplate, fill};
use bimble_core::MaterialCategory;
use bimble_core::style::{GraphicStyle, Rgb};
use bimble_geom::Scene;

/// Scales offered when a plan doesn't fit (1:N).
pub const STANDARD_SCALES: [u32; 18] = [
    1, 2, 5, 10, 20, 25, 50, 75, 100, 125, 200, 250, 500, 1000, 1250, 2000, 2500, 5000,
];

/// Sheet margin around the frame, mm.
const MARGIN: f64 = 10.0;
/// Gap between the drawing and the frame or title block, mm.
const PAD: f64 = 5.0;
/// Grid bubble diameter, mm on paper.
const BUBBLE: f64 = 8.0;
/// Distance between dimension chains, and from the drawing to the first one, mm.
const DIM_STEP: f64 = 7.0;
/// Room under the drawing for its title, mm.
const TITLE_BAND: f64 = 11.0;
/// Legend: rows per column, row height and column width, mm.
const LEGEND_ROWS: usize = 4;
const LEGEND_ROW: f64 = 6.0;
const LEGEND_COL: f64 = 62.0;

#[derive(Clone, Debug)]
pub struct PlanOptions {
    pub storey: String,
    /// Height of the cut plane above the storey level, mm.
    pub cut_above_mm: f64,
    /// 1:N, N in 1..=5000.
    pub scale: u32,
    /// None: the smallest paper it fits on.
    pub paper: Option<Paper>,
    pub style: GraphicStyle,
    /// Title block texts.
    pub project: String,
    pub date: String,
    pub template: PlanTemplate,
    /// Draw this view (a plan with its view range, a ceiling plan, a section) instead of
    /// `storey` cut at `cut_above_mm` (#32).
    pub view: Option<bimble_core::view::View>,
    /// Only the drawing, its axes, dimensions and title, on a page just its size: no frame,
    /// title block, legend or scale bar (a view placed on a sheet, #33).
    pub bare: bool,
    /// Sections marked on plans (#24): where they cut and which way they look.
    pub section_marks: Vec<SectionMark>,
}

/// A section's mark on a plan: a line through `origin` (world mm) across `facing`, its ends
/// thick, arrows looking along `facing`, its tag at each end.
#[derive(Clone, Debug, PartialEq)]
pub struct SectionMark {
    pub tag: String,
    pub origin: [f64; 2],
    pub facing: bimble_core::view::Facing,
    /// The section is drawn in true north (its facing not turned to project north).
    pub true_north: bool,
}

/// The marks of the section views among `views`: tagged by the last word of their name when it
/// is short ("Coupe A" → "A", "Section 2" → "2"), else A, B, C… in their order.
pub fn section_marks(views: &[bimble_core::view::View]) -> Vec<SectionMark> {
    views
        .iter()
        .filter_map(|v| match v.kind {
            bimble_core::view::ViewKind::Section { origin, facing, .. } => {
                Some((v, origin, facing))
            }
            _ => None,
        })
        .enumerate()
        .map(|(i, (v, origin, facing))| {
            let last = v.name.split_whitespace().last().unwrap_or("");
            let last = last.split('-').next().unwrap_or(last);
            let tag = if !last.is_empty() && last.chars().count() <= 3 && last != v.name {
                last.to_string()
            } else {
                letter_tag(i)
            };
            SectionMark {
                tag,
                origin,
                facing,
                true_north: v.true_north,
            }
        })
        .collect()
}

impl Default for PlanOptions {
    fn default() -> Self {
        PlanOptions {
            storey: String::new(),
            cut_above_mm: 1200.0,
            scale: 100,
            paper: None,
            style: GraphicStyle::default(),
            project: String::new(),
            date: String::new(),
            template: PlanTemplate::default(),
            view: None,
            bare: false,
            section_marks: Vec::new(),
        }
    }
}

/// What `plan` produced, for reporting.
#[derive(Clone, Debug, PartialEq)]
pub struct PlanInfo {
    pub paper: Paper,
    pub portrait: bool,
    pub cut_elements: usize,
    /// Elements seen below the cut whose top is filled.
    pub seen_elements: usize,
    pub projection_lines: usize,
    pub above_lines: usize,
    /// Grid axes drawn, and whether they came from the model (else from the columns).
    pub axes: usize,
    pub model_axes: bool,
    /// Elements left out because of their class (template `hidden_classes`).
    pub hidden_elements: usize,
    /// The legend's entries (drawn on the sheet unless `bare`).
    pub legend: Vec<(String, Swatch)>,
    /// From world mm to paper mm and back.
    pub frame: crate::viewdraw::ViewFrame,
}

/// A legend entry's swatch.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Swatch {
    Cut(MaterialCategory),
    Seen(MaterialCategory),
    Above,
}

fn legend_height(n: usize) -> f64 {
    if n == 0 {
        0.0
    } else {
        10.0 + n.min(LEGEND_ROWS) as f64 * LEGEND_ROW + PAD
    }
}

fn legend_width(n: usize) -> f64 {
    n.div_ceil(LEGEND_ROWS) as f64 * LEGEND_COL + 4.0
}

/// A kind of line on a drawing (#42).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Cut,
    Seen,
    Above,
    Hatch,
    Axis,
    Dimension,
    Frame,
}

/// The pen of a kind of line at 1:`scale`: its line type's colour, dashes and thickness (model
/// pens by scale; annotation pens for axes, dimensions and frames). Styles without that line
/// type fall back to their `*_pen_mm` fields.
pub fn role_pen(style: &GraphicStyle, role: Role, scale: u32) -> Pen {
    let p = &style.paper;
    let name = match role {
        Role::Cut => &p.lines.cut,
        Role::Seen => &p.lines.seen,
        Role::Above => &p.lines.above,
        Role::Hatch => &p.lines.hatch,
        Role::Axis => &p.lines.axis,
        Role::Dimension => &p.lines.dimension,
        Role::Frame => &p.lines.frame,
    };
    let annotation = matches!(role, Role::Axis | Role::Dimension | Role::Frame);
    if let Some(lt) = p.line_type(name) {
        let w = if annotation {
            p.pens.annotation_mm(lt.pen)
        } else {
            p.pens.model_mm(lt.pen, scale)
        };
        if let Some(w) = w {
            return Pen {
                width_mm: w as f64,
                color: lt.color,
                dash: lt.dash_mm.iter().map(|&d| d as f64).collect(),
            };
        }
    }
    let (w, dash): (f32, Vec<f64>) = match role {
        Role::Cut => (p.cut_pen_mm, Vec::new()),
        Role::Seen => (p.projection_pen_mm, Vec::new()),
        Role::Above => (
            p.above_pen_mm,
            p.above_dash_mm.iter().map(|&d| d as f64).collect(),
        ),
        Role::Hatch => (p.hatch_pen_mm, Vec::new()),
        Role::Axis => (
            p.axis_pen_mm,
            p.axis_dash_mm.iter().map(|&d| d as f64).collect(),
        ),
        Role::Dimension => (p.annotation_pen_mm, Vec::new()),
        Role::Frame => (p.frame_pen_mm, Vec::new()),
    };
    Pen {
        width_mm: w as f64,
        color: style.edges,
        dash,
    }
}

/// Hatch lines of a material family on paper.
fn hatches_for(style: &GraphicStyle, c: MaterialCategory, scale: u32) -> Vec<Hatch> {
    let paper_style = &style.paper;
    let mm_per_px = paper_style.hatch_mm_per_px as f64;
    let fam = style.families.get(c);
    let hatch_ink = fam
        .ink
        .unwrap_or_else(|| darken(fam.color, style.hatch_darken));
    fam.lines
        .iter()
        .flatten()
        .map(|l| Hatch {
            angle_deg: l.angle_deg as f64,
            spacing_mm: l.spacing_px as f64 * mm_per_px,
            offset_mm: 0.0,
            pen: Pen {
                width_mm: role_pen(style, Role::Hatch, scale).width_mm,
                color: hatch_ink,
                dash: l
                    .dash_px
                    .map(|d| {
                        let d = d as f64 * mm_per_px;
                        vec![d * l.dash_on as f64, d * (1.0 - l.dash_on as f64)]
                    })
                    .unwrap_or_default(),
            },
        })
        .collect()
}

/// The fill of what is seen below the cut: the family colour, lighter.
fn seen_fill_for(style: &GraphicStyle, c: MaterialCategory) -> Rgb {
    lighten(style.families.get(c).color, style.paper.seen_lighten)
}

/// The lines of a hatch type (#44), its width from the model pens at the scale.
fn hatch_type_lines(
    style: &GraphicStyle,
    h: &bimble_core::hatches::HatchType,
    scale: u32,
) -> Vec<Hatch> {
    let width = style.paper.pens.model_mm(h.pen, scale).map_or_else(
        || role_pen(style, Role::Hatch, scale).width_mm,
        |w| w as f64,
    );
    h.lines
        .iter()
        .map(|l| Hatch {
            angle_deg: l.angle_deg as f64,
            spacing_mm: l.spacing_mm as f64,
            offset_mm: l.offset_mm as f64,
            pen: Pen {
                width_mm: width,
                color: h.color,
                dash: l.dash_mm.iter().map(|&d| d as f64).collect(),
            },
        })
        .collect()
}

/// Fill and hatches of a material family where it is cut: its hatch type if it has one, else
/// its own colour and pattern.
pub(crate) fn cut_look(
    style: &GraphicStyle,
    c: MaterialCategory,
    scale: u32,
) -> (Option<Rgb>, Vec<Hatch>) {
    match style.paper.material_hatch(c, true) {
        Some(h) => (
            // No fill: white paper, hiding what lies under it.
            Some(h.fill.unwrap_or(Rgb([255, 255, 255]))),
            hatch_type_lines(style, h, scale),
        ),
        None => (
            Some(style.families.get(c).color),
            hatches_for(style, c, scale),
        ),
    }
}

/// Fill and hatches of a material family seen from above: its hatch type if it has one, else
/// its colour, lighter.
pub(crate) fn seen_look(
    style: &GraphicStyle,
    c: MaterialCategory,
    scale: u32,
) -> (Option<Rgb>, Vec<Hatch>) {
    match style.paper.material_hatch(c, false) {
        Some(h) => (
            // No fill: white paper, hiding what lies under it.
            Some(h.fill.unwrap_or(Rgb([255, 255, 255]))),
            hatch_type_lines(style, h, scale),
        ),
        None => (Some(seen_fill_for(style, c)), Vec::new()),
    }
}

/// The legend box, its bottom on `bottom` (paper mm), its left at `x0`.
pub(crate) fn draw_legend(
    items: &mut Vec<Item>,
    x0: f64,
    bottom: f64,
    legend: &[(String, Swatch)],
    tpl: &PlanTemplate,
    style: &GraphicStyle,
) {
    if legend.is_empty() {
        return;
    }
    let paper_style = &style.paper;
    let ink = style.edges;
    let t = paper_style.text_mm as f64;
    // Swatches as at 1:100.
    let proj_pen = role_pen(style, Role::Seen, 100);
    let (y0, x1, y1) = (
        bottom - legend_height(legend.len()) + PAD,
        x0 + legend_width(legend.len()),
        bottom,
    );
    items.push(Item::Lines {
        lines: vec![vec![[x0, y0], [x1, y0], [x1, y1], [x0, y1]]],
        pen: proj_pen.clone(),
    });
    items.push(Item::Text {
        pos: [x0 + 3.0, y0 + 5.5],
        size_mm: t,
        text: tpl.legend.title.clone(),
        color: ink,
        anchor: Anchor::Start,
        angle_deg: 0.0,
        bold: true,
    });
    for (i, (label, sw)) in legend.iter().enumerate() {
        let (col, row) = (i / LEGEND_ROWS, i % LEGEND_ROWS);
        let sx = x0 + 3.0 + col as f64 * LEGEND_COL;
        let sy = y0 + 9.0 + row as f64 * LEGEND_ROW;
        let rect = vec![vec![
            [sx, sy],
            [sx + 10.0, sy],
            [sx + 10.0, sy + 4.0],
            [sx, sy + 4.0],
        ]];
        match sw {
            Swatch::Cut(c) => {
                let (fill, hatches) = cut_look(style, *c, 100);
                items.push(Item::Area {
                    loops: rect,
                    fill,
                    hatches,
                    outline: Some(role_pen(style, Role::Cut, 100)),
                })
            }
            Swatch::Seen(c) => {
                let (fill, hatches) = seen_look(style, *c, 100);
                items.push(Item::Area {
                    loops: rect,
                    fill,
                    hatches,
                    outline: Some(proj_pen.clone()),
                })
            }
            Swatch::Above => {
                items.push(Item::Lines {
                    lines: vec![vec![[sx, sy + 2.0], [sx + 10.0, sy + 2.0]]],
                    pen: role_pen(style, Role::Above, 100),
                });
            }
        }
        items.push(Item::text(
            [sx + 13.0, sy + 3.3],
            t * 0.9,
            label.clone(),
            ink,
        ));
    }
}

fn darken(c: Rgb, k: f32) -> Rgb {
    Rgb(c.0.map(|v| (v as f32 * (1.0 - k)).round() as u8))
}

fn lighten(c: Rgb, k: f32) -> Rgb {
    Rgb(c
        .0
        .map(|v| (v as f32 + (255.0 - v as f32) * k).round() as u8))
}

/// The smallest standard scale at or above `n` (a coarser drawing).
fn next_standard_scale(n: f64) -> Option<u32> {
    STANDARD_SCALES.into_iter().find(|&s| s as f64 >= n)
}

/// What the plane cuts out of one element.
struct Cut {
    cat: MaterialCategory,
    class: String,
    material: String,
    loops: Vec<Vec<P2>>,
    open: Vec<Vec<P2>>,
}

/// An element entirely below the cut: its top outline and its visible edges.
struct Seen {
    cat: MaterialCategory,
    class: String,
    top_z: f64,
    top: Vec<Vec<P2>>,
    edges: Vec<[P2; 2]>,
}

/// A grid axis in scene mm.
struct Axis {
    tag: String,
    a: P2,
    b: P2,
}

/// Bounding box of points; `None` when empty.
/// The lines, ticks and value of one dimension on paper: points `pa`, `pb`, its line from `qa` to
/// `qb` along `u` (from `viewdraw::dimension_on_paper`).
#[allow(clippy::too_many_arguments)]
fn dimension_items(
    (pa, pb): (P2, P2),
    (u, qa, qb): (P2, P2, P2),
    pen: &Pen,
    value: String,
    size: f64,
    color: Rgb,
) -> [Item; 3] {
    let mut tick_pen = pen.clone();
    tick_pen.width_mm *= 2.0;
    tick_pen.dash.clear();
    let n = [u[1], -u[0]];
    let mut lines = Vec::new();
    // Extension lines from just off the points to just past the dimension line.
    for (p, q) in [(pa, qa), (pb, qb)] {
        let e = [q[0] - p[0], q[1] - p[1]];
        let el = e[0].hypot(e[1]);
        let e = if el > 1e-9 { [e[0] / el, e[1] / el] } else { n };
        lines.push(vec![
            [p[0] + e[0], p[1] + e[1]],
            [q[0] + e[0] * 1.5, q[1] + e[1] * 1.5],
        ]);
    }
    lines.push(vec![qa, qb]);
    let ticks = [qa, qb]
        .iter()
        .map(|q| {
            let s = 0.9;
            let t = [(u[0] - n[0]) * s, (u[1] - n[1]) * s];
            vec![[q[0] - t[0], q[1] - t[1]], [q[0] + t[0], q[1] + t[1]]]
        })
        .collect();
    let mut angle = -u[1].atan2(u[0]).to_degrees();
    if !(-90.0..=90.0).contains(&angle) {
        angle += if angle > 0.0 { -180.0 } else { 180.0 };
    }
    let mid = [(qa[0] + qb[0]) / 2.0, (qa[1] + qb[1]) / 2.0];
    // The value just above its line, upright.
    let r = angle.to_radians();
    let pos = [mid[0] - r.sin() * 0.8, mid[1] - r.cos() * 0.8];
    [
        Item::Lines {
            lines,
            pen: pen.clone(),
        },
        Item::Lines {
            lines: ticks,
            pen: tick_pen,
        },
        Item::Text {
            pos,
            size_mm: size,
            text: value,
            color,
            anchor: Anchor::Middle,
            angle_deg: angle,
            bold: false,
        },
    ]
}

/// Classes whose cut section gets a label (#24).
const LABELLED: [&str; 4] = ["IfcColumn", "IfcBeam", "IfcMember", "IfcPile"];

/// A length as written in a section label: whole cm, or one decimal.
pub(crate) fn cm(mm: f64) -> String {
    let c = mm / 10.0;
    if (c - c.round()).abs() < 0.05 {
        format!("{}", c.round() as i64)
    } else {
        format!("{c:.1}")
    }
}

/// "30/30", or for timber with a material name "GL24h 24x46".
pub(crate) fn section_text(cat: MaterialCategory, material: &str, b: f64, h: f64) -> String {
    let m = material.trim();
    if cat == MaterialCategory::Timber && !m.is_empty() && m.chars().count() <= 40 {
        format!("{m} {}x{}", cm(b), cm(h))
    } else {
        format!("{}/{}", cm(b), cm(h))
    }
}

/// The label of one cut section: a rectangle "b/h", a circle "Ø d". Others: none.
fn cut_label(c: &Cut) -> Option<String> {
    if !LABELLED.iter().any(|k| c.class.eq_ignore_ascii_case(k)) || c.loops.len() != 1 {
        return None;
    }
    let l = &c.loops[0];
    let (a, b) = bounds(l.iter())?;
    let (w, h) = (b[0] - a[0], b[1] - a[1]);
    if w < 1.0 || h < 1.0 {
        return None;
    }
    let area = polygon_area(l).abs();
    if (w * h - area).abs() <= 0.01 * w * h {
        return Some(section_text(c.cat, &c.material, w, h));
    }
    let r = (w + h) / 4.0;
    if l.len() >= 12
        && (w - h).abs() <= 0.02 * w
        && (std::f64::consts::PI * r * r - area).abs() <= 0.03 * area
    {
        return Some(format!("Ø{}", cm(2.0 * r)));
    }
    None
}

/// A beam above the cut: its label, the ends of its axis and its half width (scene mm).
struct BeamLabel {
    text: String,
    a: P2,
    b: P2,
    half: f64,
}

/// The label of a beam from its mesh: its direction is that of its longest level edge, its width
/// the extent across it, its height the extent in z.
fn beam_label(m: &bimble_geom::Mesh, cat: MaterialCategory, material: &str) -> Option<BeamLabel> {
    let mut dir = None;
    let mut best = 0.0;
    for e in &m.edges {
        let (p, q) = (e[0].map(f64::from), e[1].map(f64::from));
        let (dx, dy, dz) = (q[0] - p[0], q[1] - p[1], q[2] - p[2]);
        let len = dx.hypot(dy);
        if len > best && dz.abs() < 1.0 {
            best = len;
            dir = Some([dx / len, dy / len]);
        }
    }
    let d = dir?;
    let n = [-d[1], d[0]];
    let (mut s0, mut s1, mut t0, mut t1, mut z0, mut z1) = (
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
    );
    for p in &m.positions {
        let p = p.map(f64::from);
        let (s, t) = (p[0] * d[0] + p[1] * d[1], p[0] * n[0] + p[1] * n[1]);
        (s0, s1, t0, t1) = (s0.min(s), s1.max(s), t0.min(t), t1.max(t));
        (z0, z1) = (z0.min(p[2]), z1.max(p[2]));
    }
    let (w, h, len) = (t1 - t0, z1 - z0, s1 - s0);
    if !(w >= 1.0 && h >= 1.0 && len > 2.0 * w) {
        return None;
    }
    let tm = (t0 + t1) / 2.0;
    let at = |s: f64| [d[0] * s + n[0] * tm, d[1] * s + n[1] * tm];
    Some(BeamLabel {
        text: section_text(cat, material, w, h),
        a: at(s0),
        b: at(s1),
        half: w / 2.0,
    })
}

fn bounds<'a>(pts: impl Iterator<Item = &'a P2>) -> Option<(P2, P2)> {
    let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
    for p in pts {
        for k in 0..2 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    (lo[0] <= hi[0]).then_some((lo, hi))
}

fn polygon_area(l: &[P2]) -> f64 {
    let n = l.len();
    (0..n)
        .map(|i| {
            let (p, q) = (l[i], l[(i + 1) % n]);
            p[0] * q[1] - q[0] * p[1]
        })
        .sum::<f64>()
        / 2.0
}

fn inside(p: P2, l: &[P2]) -> bool {
    let n = l.len();
    let mut c = false;
    for i in 0..n {
        let (a, b) = (l[i], l[(i + 1) % n]);
        if (a[1] > p[1]) != (b[1] > p[1])
            && p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0])
        {
            c = !c;
        }
    }
    c
}

/// Spreadsheet-like tags: A..Z, AA, AB, …
pub(crate) fn letter_tag(mut i: usize) -> String {
    let mut s = Vec::new();
    loop {
        s.push(b'A' + (i % 26) as u8);
        if i < 26 {
            break;
        }
        i = i / 26 - 1;
    }
    s.reverse();
    String::from_utf8(s).unwrap_or_default()
}

/// Positions shared by at least two of `vals` (within `tol`), averaged, sorted.
pub(crate) fn clusters(mut vals: Vec<f64>, tol: f64) -> Vec<f64> {
    vals.sort_by(f64::total_cmp);
    let mut out = Vec::new();
    let mut i = 0;
    while i < vals.len() {
        let mut j = i + 1;
        while j < vals.len() && vals[j] - vals[j - 1] <= tol {
            j += 1;
        }
        if j - i >= 2 {
            out.push(vals[i..j].iter().sum::<f64>() / (j - i) as f64);
        }
        i = j;
    }
    out
}

/// A closed polyline approximating a circle.
fn circle(c: P2, r: f64) -> Vec<P2> {
    (0..=36)
        .map(|i| {
            let a = i as f64 / 36.0 * std::f64::consts::TAU;
            [c[0] + r * a.cos(), c[1] + r * a.sin()]
        })
        .collect()
}

/// A storey plan the classic way: `storey` (by name) cut `cut_above_mm` over its level, seen
/// down to the level, with the storey's elements above the cut up to the next storey.
fn storey_setup<'a>(
    scene: &'a Scene,
    storey: &str,
    cut_above_mm: f64,
) -> Result<crate::viewdraw::ViewSetup<'a>, String> {
    let storeys: Vec<String> = scene
        .storeys
        .iter()
        .map(|s| format!("{} ({:+.3} m)", s.name, s.elevation_mm / 1000.0))
        .collect();
    let idx = scene
        .storeys
        .iter()
        .position(|s| s.name == storey)
        .or_else(|| {
            scene
                .storeys
                .iter()
                .position(|s| s.name.eq_ignore_ascii_case(storey))
        })
        .ok_or_else(|| {
            if storeys.is_empty() {
                "this model has no storeys".to_string()
            } else {
                format!("no storey {storey:?}. Storeys: {}", storeys.join(", "))
            }
        })?;
    let st = &scene.storeys[idx];
    let oz = scene.origin_mm[2];
    let level = st.elevation_mm - oz;
    let next = scene.storeys[idx + 1..]
        .iter()
        .map(|s| s.elevation_mm - oz)
        .find(|&z| z > level + 1.0)
        .unwrap_or(f64::INFINITY);
    // In project north (ADR 0045): turned by the building's angle, when it has one.
    let turn = crate::viewdraw::Turn::Plan(scene.georef.plan_turn(false));
    Ok(crate::viewdraw::ViewSetup {
        scene: if turn == crate::viewdraw::Turn::Plan(0.0) {
            std::borrow::Cow::Borrowed(scene)
        } else {
            // Only what lies between this storey and the next: the rest isn't drawn.
            std::borrow::Cow::Owned(crate::viewdraw::map_scene(
                scene,
                &|e| crate::viewdraw::within_heights(e, oz, level + oz, next + oz),
                turn,
                false,
                true,
            ))
        },
        level,
        cut: level + cut_above_mm,
        next,
        storey: Some(st.name.clone()),
        storey_name: st.name.clone(),
        storey_elevation_mm: st.elevation_mm,
        plan_down: true,
        axes: true,
        turn,
    })
}

pub fn plan(scene: &Scene, o: &PlanOptions) -> Result<(Sheet, PlanInfo), String> {
    if !(1..=5000).contains(&o.scale) {
        return Err(format!(
            "scale 1:{}: must be between 1:1 and 1:5000",
            o.scale
        ));
    }
    if !(o.cut_above_mm.is_finite() && (-100_000.0..=100_000.0).contains(&o.cut_above_mm)) {
        return Err("cut height must be a number of mm within ±100 m".into());
    }
    let tpl = &o.template;
    tpl.check()?;
    // What is drawn: the scene as seen from above, the heights that matter, the storey.
    let setup = match &o.view {
        Some(v) => crate::viewdraw::setup(scene, v)?,
        None => storey_setup(scene, &o.storey, o.cut_above_mm)?,
    };
    let scene = &setup.scene;
    let oz = scene.origin_mm[2];
    let (level, cut, next) = (setup.level, setup.cut, setup.next);
    let style = &o.style;
    let paper_style = &style.paper;

    // ---- model geometry (scene mm)
    let mut projection: Vec<[P2; 2]> = Vec::new();
    let mut above: Vec<[P2; 2]> = Vec::new();
    let mut cuts: Vec<Cut> = Vec::new();
    let mut seen: Vec<Seen> = Vec::new();
    let mut beam_labels: Vec<BeamLabel> = Vec::new();
    let mut hidden = 0;
    let long = |s: &[P2; 2]| (s[1][0] - s[0][0]).hypot(s[1][1] - s[0][1]) > 0.01;
    for e in &scene.elements {
        if tpl.hides(&e.class) {
            hidden += 1;
            continue;
        }
        let world = e.world_mesh();
        let m: &bimble_geom::Mesh = &world;
        let (zmin, zmax) = m
            .positions
            .iter()
            .fold((f64::INFINITY, f64::NEG_INFINITY), |(a, b), p| {
                (a.min(p[2] as f64), b.max(p[2] as f64))
            });
        let is_cut = zmin <= cut && zmax >= cut;
        if is_cut {
            let s = section(m, cut);
            if !s.loops.is_empty() || !s.open.is_empty() {
                cuts.push(Cut {
                    cat: e.category,
                    class: e.class.clone(),
                    material: e.material.clone(),
                    loops: s.loops,
                    open: s.open,
                });
            }
        }
        let edge = |seg: &[[f32; 3]; 2]| (seg[0].map(f64::from), seg[1].map(f64::from));
        if zmax >= level - 1.0 && zmin <= cut {
            // Edges whose two faces turn away are behind the element itself (a stair's
            // underside under its treads): not drawn (#22).
            let edges: Vec<[P2; 2]> = m
                .edges
                .iter()
                .enumerate()
                .filter(|(i, _)| m.edge_normals.get(*i).is_none_or(edge_seen_from_above))
                .filter_map(|(_, seg)| {
                    let (a, b) = edge(seg);
                    clip_z(a, b, level - 1.0, cut)
                })
                .filter(long)
                .collect();
            if tpl.seen_fills && !is_cut {
                // What is seen of it from above: all its faces turned upwards (a sloped slab,
                // a ramp, stair treads, footing steps), not only its highest part (#22).
                let top = facing_up_region(m);
                seen.push(Seen {
                    cat: e.category,
                    class: e.class.clone(),
                    top_z: zmax,
                    top,
                    edges,
                });
            } else {
                projection.extend(edges);
            }
        }
        if tpl.element_labels
            && setup.plan_down
            && zmin > cut
            && zmin < next
            && e.class.eq_ignore_ascii_case("IfcBeam")
            && let Some(l) = beam_label(m, e.category, &e.material)
        {
            beam_labels.push(l);
        }
        if setup.storey.as_ref() == Some(&e.storey) && zmax > cut {
            for seg in &m.edges {
                let (a, b) = edge(seg);
                if let Some(s) = clip_z(a, b, cut, next)
                    && long(&s)
                {
                    above.push(s);
                }
            }
        }
    }
    // Painter's order: low tops first.
    seen.sort_by(|a, b| a.top_z.total_cmp(&b.top_z));

    // The view's crop region (ADR 0046): its geometry clipped to it, in the turned scene (the crop
    // is in the view's plane, world mm: less the turned origin).
    let crop = o.view.as_ref().and_then(|v| v.crop).map(|c| {
        let t = setup.turn.apply(scene.origin_mm);
        crate::crop::Rect {
            lo: [c.min[0] - t[0], c.min[1] - t[1]],
            hi: [c.max[0] - t[0], c.max[1] - t[1]],
        }
    });
    // Columns' centres, for axes when the model has none: counted before the crop, so a crop
    // holding one column of a line still shows the line's axis.
    let column_centres: Vec<P2> = cuts
        .iter()
        .filter(|c| c.class.eq_ignore_ascii_case("IfcColumn"))
        .filter_map(|c| bounds(c.loops.iter().flatten()))
        .map(|(l, h)| [(l[0] + h[0]) / 2.0, (l[1] + h[1]) / 2.0])
        .collect();
    if let Some(r) = &crop {
        for c in &mut cuts {
            c.loops = c.loops.iter().filter_map(|l| r.polygon(l)).collect();
            c.open = c.open.iter().flat_map(|l| r.polyline(l)).collect();
        }
        cuts.retain(|c| !c.loops.is_empty() || !c.open.is_empty());
        for sn in &mut seen {
            sn.top = sn.top.iter().filter_map(|l| r.polygon(l)).collect();
            sn.edges = sn.edges.iter().filter_map(|e| r.segment(*e)).collect();
        }
        seen.retain(|sn| !sn.top.is_empty() || !sn.edges.is_empty());
        projection = projection.iter().filter_map(|e| r.segment(*e)).collect();
        above = above.iter().filter_map(|e| r.segment(*e)).collect();
        beam_labels.retain(|b| r.contains([(b.a[0] + b.b[0]) / 2.0, (b.a[1] + b.b[1]) / 2.0]));
    }
    let Some((blo, bhi)) = bounds(
        cuts.iter()
            .flat_map(|c| c.loops.iter().chain(&c.open))
            .flatten()
            .chain(seen.iter().flat_map(|s| s.top.iter().flatten()))
            .chain(seen.iter().flat_map(|s| s.edges.iter().flatten()))
            .chain(projection.iter().flatten())
            .chain(above.iter().flatten()),
    ) else {
        return Err(match &o.view {
            Some(v) => format!("nothing to draw in view {}", v.name),
            None => format!(
                "nothing to draw at storey {} (cut at {:+.3} m)",
                setup.storey_name,
                (cut + oz) / 1000.0
            ),
        });
    };

    // ---- grid axes (scene mm)
    let mut axes: Vec<Axis> = Vec::new();
    let mut model_axes = false;
    if tpl.axes != Axes::Off && setup.axes {
        let (ox_, oy_) = (scene.origin_mm[0], scene.origin_mm[1]);
        axes = scene
            .grids
            .iter()
            .map(|g| Axis {
                tag: g.tag.clone(),
                a: [g.start_mm[0] - ox_, g.start_mm[1] - oy_],
                b: [g.end_mm[0] - ox_, g.end_mm[1] - oy_],
            })
            .collect();
        model_axes = !axes.is_empty();
        if axes.is_empty() && tpl.axes == Axes::ModelOrColumns {
            let centres = &column_centres;
            let xs = clusters(centres.iter().map(|p| p[0]).collect(), 50.0);
            let ys = clusters(centres.iter().map(|p| p[1]).collect(), 50.0);
            for (i, x) in xs.iter().enumerate() {
                axes.push(Axis {
                    tag: letter_tag(i),
                    a: [*x, blo[1]],
                    b: [*x, bhi[1]],
                });
            }
            for (i, y) in ys.iter().enumerate() {
                axes.push(Axis {
                    tag: (i + 1).to_string(),
                    a: [blo[0], *y],
                    b: [bhi[0], *y],
                });
            }
        }
    }
    // Cropped: the axes within the crop too (the drawing already is: its geometry was clipped).
    if let Some(r) = &crop {
        axes = axes
            .into_iter()
            .filter_map(|a| {
                let [p, q] = r.segment([a.a, a.b])?;
                Some(Axis { a: p, b: q, ..a })
            })
            .collect();
    }
    let (lo, hi) = bounds(
        [blo, bhi]
            .iter()
            .chain(axes.iter().flat_map(|a| [&a.a, &a.b])),
    )
    .unwrap_or((blo, bhi));
    // Axes running up the sheet (bubbles on top) and across (bubbles on the left).
    let vertical = |a: &Axis| (a.b[1] - a.a[1]).abs() >= (a.b[0] - a.a[0]).abs();
    let any_v = axes.iter().any(vertical);
    let any_h = axes.iter().any(|a| !vertical(a));

    // ---- legend entries: (label, swatch fill, hatches?, outline pen, dashed line?)
    let family_label = |c: MaterialCategory, pattern: &str| {
        fill(
            pattern,
            &[("family", tpl.family_name(c.label()))],
            &tpl.fields,
        )
    };
    let mut legend: Vec<(String, Swatch)> = Vec::new();
    if tpl.legend.show {
        for c in MaterialCategory::ALL {
            if cuts.iter().any(|x| x.cat == c && !x.loops.is_empty()) {
                legend.push((family_label(c, &tpl.legend.cut), Swatch::Cut(c)));
            }
        }
        for c in MaterialCategory::ALL {
            if seen.iter().any(|x| x.cat == c && !x.top.is_empty()) {
                legend.push((family_label(c, &tpl.legend.seen), Swatch::Seen(c)));
            }
        }
        if !above.is_empty() && !tpl.legend.above.is_empty() {
            legend.push((tpl.legend.above.clone(), Swatch::Above));
        }
    }
    // A bare drawing's legend goes on its sheet's (PlanInfo::legend).
    let legend_n = if o.bare { 0 } else { legend.len() };
    let legend_h = legend_height(legend_n);

    // ---- paper
    let tb_h = if o.bare {
        0.0
    } else {
        tpl.title_block.height_mm
    };
    let margin = if o.bare { 0.0 } else { MARGIN };
    let chains = |any: bool| {
        if !tpl.dimensions {
            0.0
        } else if any {
            2.0
        } else {
            1.0
        }
    };
    let band =
        |chains: f64, bubbles: bool| chains * DIM_STEP + if bubbles { BUBBLE + 4.0 } else { 0.0 };
    let top_extra = band(chains(any_v), any_v);
    let left_extra = band(chains(any_h), any_h);
    let extra = (left_extra + 2.0 * PAD, top_extra + TITLE_BAND + 2.0 * PAD);
    let model = (hi[0] - lo[0], hi[1] - lo[1]);
    let k = 1.0 / o.scale as f64;
    let need = (model.0 * k + extra.0, model.1 * k + extra.1);
    let room = |w: f64, h: f64| (w - 2.0 * margin, h - 2.0 * margin - tb_h - legend_h);
    let legend_w = legend_width(legend_n);
    let fits = |p: Paper, portrait: bool| {
        let (w, h) = p.size();
        let (w, h) = if portrait { (h, w) } else { (w, h) };
        let (rw, rh) = room(w, h);
        need.0 <= rw && need.1 <= rh && legend_w <= rw
    };
    // A bare drawing's page is just its size.
    let (paper, portrait, w, h) = if o.bare {
        (o.paper.unwrap_or(Paper::A4), false, need.0, need.1)
    } else {
        let candidates: Vec<Paper> = match o.paper {
            Some(p) => vec![p],
            None => Paper::ALL.to_vec(),
        };
        let chosen = candidates
            .iter()
            .flat_map(|&p| [(p, false), (p, true)])
            .find(|&(p, portrait)| fits(p, portrait));
        let Some((paper, portrait)) = chosen else {
            let biggest = *candidates.last().unwrap();
            let (bw, bh) = biggest.size();
            // The scale at which the drawing would fit the biggest paper (either orientation).
            let fit = |w: f64, h: f64| {
                let (rw, rh) = room(w, h);
                (model.0 / (rw - extra.0)).max(model.1 / (rh - extra.1))
            };
            let n = fit(bw, bh).min(fit(bh, bw));
            let hint = match next_standard_scale(n).filter(|_| n > 0.0) {
                Some(s) => format!("1:{s} fits on {}", biggest.name()),
                None => "no scale up to 1:5000 fits".into(),
            };
            return Err(format!(
                "at 1:{} the plan ({:.1} × {:.1} m) needs {:.0} × {:.0} mm and does not fit on {}; {hint}",
                o.scale,
                model.0 / 1000.0,
                model.1 / 1000.0,
                need.0,
                need.1,
                biggest.name()
            ));
        };
        let (w, h) = {
            let (w, h) = paper.size();
            if portrait { (h, w) } else { (w, h) }
        };
        (paper, portrait, w, h)
    };
    let (rw, rh) = room(w, h);
    // Centre the drawing and its decorations in the space above the legend and title block.
    let ox = margin + PAD + left_extra + (rw - need.0) / 2.0;
    let oy = margin + PAD + top_extra + (rh - need.1) / 2.0;
    let to_paper = |p: &P2| [ox + (p[0] - lo[0]) * k, oy + (hi[1] - p[1]) * k];
    let seg_lines = |v: &[[P2; 2]]| -> Vec<Vec<P2>> {
        v.iter()
            .map(|s| vec![to_paper(&s[0]), to_paper(&s[1])])
            .collect()
    };
    let paper_loops = |v: &[Vec<P2>]| -> Vec<Vec<P2>> {
        v.iter().map(|l| l.iter().map(to_paper).collect()).collect()
    };
    let ink = style.edges;
    let line = |role: Role| role_pen(style, role, o.scale);
    let thicker = |role: Role, k: f64| {
        let mut p = line(role);
        p.width_mm *= k;
        p.dash.clear();
        p
    };
    let proj_pen = line(Role::Seen);
    let note_pen = line(Role::Dimension);
    let t = paper_style.text_mm as f64;
    let text = |pos: P2, size: f64, s: String| Item::text(pos, size, s, ink);
    let text_at =
        |pos: P2, size: f64, s: String, anchor: Anchor, angle_deg: f64, bold: bool| Item::Text {
            pos,
            size_mm: size,
            text: s,
            color: ink,
            anchor,
            angle_deg,
            bold,
        };
    let cut_of = |c: MaterialCategory| cut_look(style, c, o.scale);
    let seen_of = |c: MaterialCategory| seen_look(style, c, o.scale);

    // ---- items, back to front
    let mut items = Vec::new();
    if !projection.is_empty() {
        items.push(Item::Lines {
            lines: seg_lines(&projection),
            pen: proj_pen.clone(),
        });
    }
    let mut labels = Vec::new();
    let mut openings: Vec<(P2, P2)> = Vec::new();
    let mut seen_lines = 0;
    for s in &seen {
        if !s.edges.is_empty() {
            seen_lines += s.edges.len();
            items.push(Item::Lines {
                lines: seg_lines(&s.edges),
                pen: proj_pen.clone(),
            });
        }
        if s.top.is_empty() {
            continue;
        }
        let (fill, hatches) = seen_of(s.cat);
        items.push(Item::Area {
            loops: paper_loops(&s.top),
            fill,
            hatches,
            outline: Some(proj_pen.clone()),
        });
        let slab = s.class.eq_ignore_ascii_case("IfcSlab");
        if slab && (tpl.opening_crosses || tpl.opening_dimensions) {
            // Holes of the top that are rectangles: a cross (an opening through the slab).
            let mut crosses = Vec::new();
            for (i, l) in s.top.iter().enumerate() {
                let hole = l.len() >= 3
                    && s.top
                        .iter()
                        .enumerate()
                        .any(|(j, o)| j != i && inside(l[0], o));
                let Some((a, b)) = bounds(l.iter()) else {
                    continue;
                };
                let rect = ((b[0] - a[0]) * (b[1] - a[1]) - polygon_area(l).abs()).abs()
                    <= 0.01 * (b[0] - a[0]) * (b[1] - a[1]);
                if hole && rect {
                    let (p, q) = (to_paper(&a), to_paper(&b));
                    if tpl.opening_crosses {
                        crosses.push(vec![p, q]);
                        crosses.push(vec![[p[0], q[1]], [q[0], p[1]]]);
                    }
                    if tpl.opening_dimensions {
                        openings.push((a, b));
                    }
                }
            }
            if !crosses.is_empty() {
                items.push(Item::Lines {
                    lines: crosses,
                    pen: proj_pen.clone(),
                });
            }
        }
        if slab
            && tpl.slab_levels
            && setup.plan_down
            && let Some((a, b)) = bounds(s.top.iter().flatten())
        {
            let (p, q) = (to_paper(&a), to_paper(&b));
            if (q[0] - p[0]).min(p[1] - q[1]) >= 12.0 {
                labels.push(([(p[0] + q[0]) / 2.0, (p[1] + q[1]) / 2.0], s.top_z));
            }
        }
    }
    for (c, z) in labels {
        // A small triangle pointing down onto the level, its value to the right.
        let r = 1.2;
        items.push(Item::Area {
            loops: vec![vec![
                [c[0] - r, c[1] - r * 1.6],
                [c[0] + r, c[1] - r * 1.6],
                [c[0], c[1]],
            ]],
            fill: Some(ink),
            hatches: Vec::new(),
            outline: None,
        });
        items.push(text(
            [c[0] + r + 0.8, c[1] - 0.4],
            t * 0.8,
            format!("{:+.2}", (z + oz) / 1000.0 + 0.0),
        ));
    }
    if !above.is_empty() {
        items.push(Item::Lines {
            lines: seg_lines(&above),
            pen: line(Role::Above),
        });
    }

    // Grid axes, under what is cut.
    let top_draw = oy;
    let left_draw = ox;
    let bubble_v = top_draw - chains(any_v) * DIM_STEP - 4.0 - BUBBLE / 2.0;
    let bubble_h = left_draw - chains(any_h) * DIM_STEP - 4.0 - BUBBLE / 2.0;
    let mut axis_lines = Vec::new();
    let mut bubbles = Vec::new();
    // Where each axis crosses the top (vertical axes) or left (horizontal) edge, for the chains.
    let mut at_top = Vec::new();
    let mut at_left = Vec::new();
    for ax in &axes {
        let (a, b) = (to_paper(&ax.a), to_paper(&ax.b));
        let v = vertical(ax);
        // Order so `a` is the end the bubble goes to (top or left).
        let (a, b) = if (v && a[1] > b[1]) || (!v && a[0] > b[0]) {
            (b, a)
        } else {
            (a, b)
        };
        let d = [b[0] - a[0], b[1] - a[1]];
        let len = d[0].hypot(d[1]);
        if len < 1e-9 {
            continue;
        }
        let u = [d[0] / len, d[1] / len];
        // Extend `a` along the axis to the bubble's edge.
        let along = if v {
            (bubble_v + BUBBLE / 2.0 - a[1]) / u[1]
        } else {
            (bubble_h + BUBBLE / 2.0 - a[0]) / u[0]
        };
        let a2 = [a[0] + u[0] * along, a[1] + u[1] * along];
        let b2 = [b[0] + u[0] * 3.0, b[1] + u[1] * 3.0];
        axis_lines.push(vec![a2, b2]);
        let c = [a2[0] - u[0] * BUBBLE / 2.0, a2[1] - u[1] * BUBBLE / 2.0];
        bubbles.push((c, ax.tag.clone()));
        // Crossing with the drawing's top / left edge.
        if v && u[1].abs() > 1e-9 {
            let s = (top_draw - a[1]) / u[1];
            at_top.push(a[0] + u[0] * s);
        } else if !v && u[0].abs() > 1e-9 {
            let s = (left_draw - a[0]) / u[0];
            at_left.push(a[1] + u[1] * s);
        }
    }
    if !axis_lines.is_empty() {
        items.push(Item::Lines {
            lines: axis_lines,
            // Dash-dot, the axis line of office drawings.
            pen: line(Role::Axis),
        });
    }

    // Fills and hatches of all cut elements first, then all outlines on top: touching elements
    // (a column against a wall) keep both outlines.
    for c in &cuts {
        if c.loops.is_empty() {
            continue;
        }
        let (fill, hatches) = cut_of(c.cat);
        items.push(Item::Area {
            loops: paper_loops(&c.loops),
            fill,
            hatches,
            outline: None,
        });
    }
    for c in &cuts {
        if !c.loops.is_empty() {
            items.push(Item::Area {
                loops: paper_loops(&c.loops),
                fill: None,
                hatches: Vec::new(),
                outline: Some(line(Role::Cut)),
            });
        }
        if !c.open.is_empty() {
            items.push(Item::Lines {
                lines: paper_loops(&c.open),
                pen: line(Role::Cut),
            });
        }
    }

    // Openings (#24): their size inside, their position from the nearest axis (or, without
    // axes, nothing more), above and left of them.
    for (a, b) in &openings {
        let (p, q) = (to_paper(a), to_paper(b));
        let (w, h) = (b[0] - a[0], b[1] - a[1]);
        let label = format!("{}x{}", cm(w), cm(h));
        let size = t * 0.8;
        let fits = crate::sheet::text_width_mm(&label, size, false, o.style.paper.font) + 1.0
            < q[0] - p[0]
            && size + 1.0 < p[1] - q[1];
        let pos = if fits {
            [(p[0] + q[0]) / 2.0, (p[1] + q[1]) / 2.0 + size * 0.35]
        } else {
            [(p[0] + q[0]) / 2.0, p[1] + size + 0.8]
        };
        items.push(text_at(pos, size, label, Anchor::Middle, 0.0, false));
        let nearest = |v: bool, lo: f64, hi: f64| {
            axes.iter()
                .filter(|ax| vertical(ax) == v)
                .map(|ax| if v { ax.a[0] } else { ax.a[1] })
                .flat_map(|x| [(x, lo), (x, hi)])
                .filter(|(x, e)| (x - e).abs() > 1.0)
                .min_by(|m, n| (m.0 - m.1).abs().total_cmp(&(n.0 - n.1).abs()))
        };
        // Along x, on the top edge: from the nearest vertical axis to the nearer side.
        if let Some((x, e)) = nearest(true, a[0], b[0]) {
            let (l, r) = (x.min(e), x.max(e));
            let (pa, pb) = (to_paper(&[l, b[1]]), to_paper(&[r, b[1]]));
            if let Some((u, qa, qb, len)) = crate::viewdraw::dimension_on_paper(
                pa,
                pb,
                2.5,
                bimble_core::view::DimAxis::Horizontal,
            ) {
                items.extend(dimension_items(
                    (pa, pb),
                    (u, qa, qb),
                    &note_pen,
                    tpl.dimension_unit.format(len / k),
                    t * 0.8,
                    ink,
                ));
            }
        }
        // Along y, on the left edge: from the nearest horizontal axis to the nearer side.
        if let Some((y, e)) = nearest(false, a[1], b[1]) {
            let (lo, hi) = (y.min(e), y.max(e));
            // Upwards on paper: the line to the left.
            let (pa, pb) = (to_paper(&[a[0], lo]), to_paper(&[a[0], hi]));
            if let Some((u, qa, qb, len)) = crate::viewdraw::dimension_on_paper(
                pa,
                pb,
                2.5,
                bimble_core::view::DimAxis::Vertical,
            ) {
                items.extend(dimension_items(
                    (pa, pb),
                    (u, qa, qb),
                    &note_pen,
                    tpl.dimension_unit.format(len / k),
                    t * 0.8,
                    ink,
                ));
            }
        }
    }

    // Section labels (#24): beside what is cut, along the beams above.
    if tpl.element_labels {
        for c in &cuts {
            let Some(label) = cut_label(c) else { continue };
            let Some((_, b)) = bounds(c.loops.iter().flatten()) else {
                continue;
            };
            let q = to_paper(&b);
            items.push(text_at(
                // Above-right of it, off the axes that run through its centre.
                [q[0] + 0.6, q[1] - 0.6],
                t * 0.8,
                label,
                Anchor::Start,
                0.0,
                false,
            ));
        }
        for l in &beam_labels {
            let (pa, pb) = (to_paper(&l.a), to_paper(&l.b));
            let d = [pb[0] - pa[0], pb[1] - pa[1]];
            let len = d[0].hypot(d[1]);
            if len < t * 4.0 {
                continue;
            }
            let u = [d[0] / len, d[1] / len];
            let mut angle = -u[1].atan2(u[0]).to_degrees();
            if !(-90.0..=90.0).contains(&angle) {
                angle += if angle > 0.0 { -180.0 } else { 180.0 };
            }
            // Just beside the beam, on the side above its text.
            let r = angle.to_radians();
            let off = l.half * k + 0.8;
            let mid = [(pa[0] + pb[0]) / 2.0, (pa[1] + pb[1]) / 2.0];
            items.push(text_at(
                [mid[0] - r.sin() * off, mid[1] - r.cos() * off],
                t * 0.8,
                l.text.clone(),
                Anchor::Middle,
                angle,
                false,
            ));
        }
    }

    // Section marks (#24), on plans only.
    if tpl.section_markers && !matches!(setup.turn, crate::viewdraw::Turn::Section(_)) {
        let so = scene.origin_mm;
        let (lo_p, hi_p) = (to_paper(&blo), to_paper(&bhi));
        let (x0, x1) = (lo_p[0].min(hi_p[0]) - 6.0, lo_p[0].max(hi_p[0]) + 6.0);
        let (y0, y1) = (lo_p[1].min(hi_p[1]) - 6.0, lo_p[1].max(hi_p[1]) + 6.0);
        for m in &o.section_marks {
            let at = |x: f64, y: f64| {
                let q = setup.turn.apply([x - so[0], y - so[1], 0.0]);
                to_paper(&[q[0], q[1]])
            };
            let f = m.facing.dir_turned(scene.georef.plan_turn(m.true_north));
            let c = at(m.origin[0], m.origin[1]);
            let ahead = at(m.origin[0] + f[0], m.origin[1] + f[1]);
            let fd = [ahead[0] - c[0], ahead[1] - c[1]];
            let fl = fd[0].hypot(fd[1]);
            if fl < 1e-12 {
                continue;
            }
            let fd = [fd[0] / fl, fd[1] / fl];
            // Across the drawing, perpendicular to where it looks (facings are along the axes).
            let g = [-fd[1], fd[0]];
            let (e0, e1) = if g[0].abs() > 0.5 {
                if !(y0..=y1).contains(&c[1]) {
                    continue;
                }
                ([x0, c[1]], [x1, c[1]])
            } else {
                if !(x0..=x1).contains(&c[0]) {
                    continue;
                }
                ([c[0], y0], [c[0], y1])
            };
            items.push(Item::Lines {
                lines: vec![vec![e0, e1]],
                pen: line(Role::Axis),
            });
            for (e, inward) in [
                (e0, [e1[0] - e0[0], e1[1] - e0[1]]),
                (e1, [e0[0] - e1[0], e0[1] - e1[1]]),
            ] {
                let il = inward[0].hypot(inward[1]).max(1e-9);
                let iw = [inward[0] / il, inward[1] / il];
                let tip = [e[0] + iw[0] * 6.0, e[1] + iw[1] * 6.0];
                items.push(Item::Lines {
                    lines: vec![vec![e, tip]],
                    pen: thicker(Role::Cut, 1.5),
                });
                // The arrow from the end, looking along the section.
                let base = [e[0] + iw[0] * 1.5, e[1] + iw[1] * 1.5];
                let head = [base[0] + fd[0] * 4.0, base[1] + fd[1] * 4.0];
                items.push(Item::Lines {
                    lines: vec![vec![base, head]],
                    pen: line(Role::Seen),
                });
                let (a, b) = (
                    [
                        head[0] - fd[0] * 2.0 + iw[0] * 1.0,
                        head[1] - fd[1] * 2.0 + iw[1] * 1.0,
                    ],
                    [
                        head[0] - fd[0] * 2.0 - iw[0] * 1.0,
                        head[1] - fd[1] * 2.0 - iw[1] * 1.0,
                    ],
                );
                items.push(Item::Area {
                    loops: vec![vec![head, a, b]],
                    fill: Some(ink),
                    hatches: Vec::new(),
                    outline: None,
                });
                // The tag beyond the end of the line.
                let size = t * 1.2;
                let pos = [e[0] - iw[0] * 3.0, e[1] - iw[1] * 3.0 + size * 0.36];
                items.push(text_at(pos, size, m.tag.clone(), Anchor::Middle, 0.0, true));
            }
        }
    }

    // Bubbles over everything.
    for (c, tag) in &bubbles {
        items.push(Item::Area {
            loops: vec![circle(*c, BUBBLE / 2.0)],
            fill: Some(Rgb([255, 255, 255])),
            hatches: Vec::new(),
            outline: Some(line(Role::Seen)),
        });
        let size = if tag.chars().count() > 2 { 3.0 } else { 4.0 };
        items.push(text_at(
            [c[0], c[1] + size * 0.36],
            size,
            tag.clone(),
            Anchor::Middle,
            0.0,
            false,
        ));
    }

    // ---- dimension chains: first along the axes (with the outer faces), then overall.
    let unit = tpl.dimension_unit;
    let (plo, phi) = (to_paper(&[blo[0], bhi[1]]), to_paper(&[bhi[0], blo[1]]));
    let mut dim_lines = Vec::new();
    let mut dim_ticks = Vec::new();
    let mut dim_texts = Vec::new();
    let dim_size = t * 0.9;
    let mut chain = |stops: &mut Vec<f64>, line_at: f64, from: f64, horizontal: bool| {
        stops.sort_by(f64::total_cmp);
        stops.dedup_by(|a, b| (*a - *b).abs() < 0.05);
        if stops.len() < 2 {
            return;
        }
        let pt = |s: f64, q: f64| if horizontal { [s, q] } else { [q, s] };
        dim_lines.push(vec![
            pt(stops[0] - 1.5, line_at),
            pt(stops[stops.len() - 1] + 1.5, line_at),
        ]);
        for &s in stops.iter() {
            // Extension line from near the drawing to just past the dimension line.
            dim_lines.push(vec![pt(s, from - 1.5), pt(s, line_at - 1.5)]);
            // Architect's tick: a short 45° stroke.
            let p = pt(s, line_at);
            dim_ticks.push(vec![[p[0] - 0.9, p[1] + 0.9], [p[0] + 0.9, p[1] - 0.9]]);
        }
        let angle = if horizontal { 0.0 } else { 90.0 };
        for (i, w) in stops.windows(2).enumerate() {
            let value = unit.format((w[1] - w[0]) / k);
            // A text wider than its span goes outside it: before the first span, else after.
            let (s, anchor) = if text_width_mm(&value, dim_size, false, o.style.paper.font) + 0.6
                <= w[1] - w[0]
            {
                ((w[0] + w[1]) / 2.0, Anchor::Middle)
            } else if i == 0 {
                // Vertical texts read upwards, towards smaller s.
                (
                    w[0] - 1.2,
                    if horizontal {
                        Anchor::End
                    } else {
                        Anchor::Start
                    },
                )
            } else {
                (
                    w[1] + 1.2,
                    if horizontal {
                        Anchor::Start
                    } else {
                        Anchor::End
                    },
                )
            };
            dim_texts.push((pt(s, line_at - 0.8), value, angle, anchor));
        }
    };
    if tpl.dimensions {
        let mut line = top_draw - DIM_STEP;
        if any_v {
            let mut s: Vec<f64> = at_top.clone();
            s.extend([plo[0], phi[0]]);
            chain(&mut s, line, top_draw, true);
            line -= DIM_STEP;
        }
        chain(&mut vec![plo[0], phi[0]], line, top_draw, true);
        let mut line = left_draw - DIM_STEP;
        if any_h {
            let mut s: Vec<f64> = at_left.clone();
            s.extend([plo[1], phi[1]]);
            chain(&mut s, line, left_draw, false);
            line -= DIM_STEP;
        }
        chain(&mut vec![plo[1], phi[1]], line, left_draw, false);
    }
    if !dim_lines.is_empty() {
        items.push(Item::Lines {
            lines: dim_lines,
            pen: note_pen.clone(),
        });
        items.push(Item::Lines {
            lines: dim_ticks,
            pen: thicker(Role::Dimension, 2.0),
        });
        for (pos, value, angle, anchor) in dim_texts {
            items.push(text_at(pos, dim_size, value, anchor, angle, false));
        }
    }

    // ---- the view's own dimensions and texts (#33)
    if let Some(v) = &o.view {
        let so = scene.origin_mm;
        let at = |p: &[f64; 3]| {
            let q = setup.turn.apply([p[0] - so[0], p[1] - so[1], p[2] - so[2]]);
            to_paper(&[q[0], q[1]])
        };
        for a in &v.annotations {
            match a {
                bimble_core::view::Annotation::Dimension {
                    a,
                    b,
                    offset_mm,
                    axis,
                    kind,
                    ..
                } => {
                    let dt = kind.as_ref().and_then(|k| paper_style.dim_type(k));
                    let (pa, pb) = (at(&a.at), at(&b.at));
                    let Some((u, qa, qb, len)) =
                        crate::viewdraw::dimension_on_paper(pa, pb, offset_mm * k, *axis)
                    else {
                        continue;
                    };
                    // Its type's line (annotation pen) in its colour, else the dimension line.
                    let mut pen = note_pen.clone();
                    if let Some(dt) = dt {
                        if let Some(lt) = paper_style.line_type(&dt.line_type)
                            && let Some(w) = paper_style.pens.annotation_mm(lt.pen)
                        {
                            pen.width_mm = w as f64;
                            pen.dash = lt.dash_mm.iter().map(|&d| d as f64).collect();
                        }
                        pen.color = dt.color;
                    }
                    let (value, size, color) = match dt {
                        Some(dt) => (dt.format(len / k), dt.text_mm as f64, dt.color),
                        None => (unit.format(len / k), t * 0.9, ink),
                    };
                    items.extend(dimension_items(
                        (pa, pb),
                        (u, qa, qb),
                        &pen,
                        value,
                        size,
                        color,
                    ));
                }
                bimble_core::view::Annotation::Text {
                    text,
                    at: p,
                    size_mm,
                    angle_deg,
                    kind,
                    ..
                } => {
                    let tt = kind.as_ref().and_then(|k| paper_style.text_type(k));
                    let size = tt.map_or(*size_mm, |t| t.size_mm as f64);
                    let color = tt.map_or(ink, |t| t.color);
                    for (i, line) in text.split('\n').enumerate() {
                        let q = at(p);
                        items.push(Item::Text {
                            pos: [q[0], q[1] + i as f64 * size * 1.25],
                            size_mm: size,
                            text: line.to_string(),
                            color,
                            anchor: Anchor::Start,
                            angle_deg: *angle_deg,
                            bold: false,
                        });
                    }
                }
            }
        }
    }

    // ---- texts
    let n = o.scale.to_string();
    let values = [
        ("project", o.project.clone()),
        ("storey", setup.storey_name.clone()),
        (
            "level",
            format!("{:+.2}", setup.storey_elevation_mm / 1000.0 + 0.0),
        ),
        (
            "view",
            o.view.as_ref().map(|v| v.name.clone()).unwrap_or_default(),
        ),
        // What the drawing is: the view's name, else "Plan <storey>".
        (
            "drawing",
            o.view
                .as_ref()
                .map_or_else(|| format!("Plan {}", setup.storey_name), |v| v.name.clone()),
        ),
        ("scale", format!("1:{n}")),
        ("n", n.clone()),
        // A sheet's number; a single plan takes it from the fields (--field drawing_no=…).
        (
            "drawing_no",
            tpl.fields.get("drawing_no").cloned().unwrap_or_default(),
        ),
        (
            "paper",
            format!(
                "{}{}",
                paper.name(),
                if portrait { " portrait" } else { "" }
            ),
        ),
        ("date", o.date.clone()),
        ("bimble", env!("CARGO_PKG_VERSION").to_string()),
    ];
    let filled = |s: &str| fill(s, &values, &tpl.fields);
    // Drawing title, bold and underlined, under the drawing at its left.
    // Ceiling plans and sections are titled by the view's name (the template's title says
    // "plan"); plans keep the template's.
    let title = if o.view.is_some() && !setup.plan_down {
        filled("{view}, {scale}")
    } else {
        filled(&tpl.title)
    };
    let title_y = oy + model.1 * k + PAD + TITLE_BAND - 4.0;
    // A bare drawing's page reaches the end of its title (a small drawing, a long name).
    let mut title_end = 0.0f64;
    if !title.is_empty() {
        let size = t * 2.0;
        let x = ox;
        items.push(text_at(
            [x, title_y],
            size,
            title.clone(),
            Anchor::Start,
            0.0,
            true,
        ));
        let tw = text_width_mm(&title, size, true, o.style.paper.font);
        title_end = x + tw + PAD;
        items.push(Item::Lines {
            lines: vec![vec![[x, title_y + 1.0], [x + tw, title_y + 1.0]]],
            pen: thicker(Role::Cut, 0.7),
        });
    }
    // Scale bar, level note, legend, frame and title block: whole sheets only.
    if !o.bare {
        // Graphic scale bar at the right of the title line: a round length about 50 mm long.
        let target_m = 50.0 / k / 1000.0;
        let mag = 10f64.powf(target_m.log10().floor());
        let len_m = [1.0, 2.0, 5.0, 10.0]
            .into_iter()
            .map(|f| f * mag)
            .rfind(|&l| l <= target_m)
            .unwrap_or(mag);
        let bar = len_m * 1000.0 * k;
        {
            let (bx, by) = (w - MARGIN - PAD - bar, title_y - 2.0);
            for i in 0..4 {
                let (x0, x1) = (bx + bar * i as f64 / 4.0, bx + bar * (i + 1) as f64 / 4.0);
                items.push(Item::Area {
                    loops: vec![vec![[x0, by], [x1, by], [x1, by + 2.0], [x0, by + 2.0]]],
                    fill: Some(if i % 2 == 0 {
                        ink
                    } else {
                        Rgb([255, 255, 255])
                    }),
                    hatches: Vec::new(),
                    outline: Some(proj_pen.clone()),
                });
            }
            items.push(text([bx, by - 1.5], t * 0.8, "0".into()));
            let label = if len_m >= 1.0 {
                format!("{len_m} m")
            } else {
                format!("{} mm", len_m * 1000.0)
            };
            items.push(text_at(
                [bx + bar, by - 1.5],
                t * 0.8,
                label,
                Anchor::End,
                0.0,
                false,
            ));
        }
        // Level note just above the title block, at the right.
        let tb_top = h - MARGIN - tb_h;
        let note = filled(&tpl.level_note);
        if !note.is_empty() {
            items.push(text_at(
                [w - MARGIN - PAD, tb_top - 2.5],
                t * 0.9,
                note,
                Anchor::End,
                0.0,
                false,
            ));
        }

        // ---- legend, above the title block at the left
        draw_legend(&mut items, MARGIN, tb_top, &legend, tpl, style);

        frame_and_title_block(&mut items, w, h, tpl, &filled, ink, style);
    }

    let info = PlanInfo {
        paper,
        portrait,
        cut_elements: cuts.len(),
        seen_elements: seen.iter().filter(|s| !s.top.is_empty()).count(),
        projection_lines: projection.len() + seen_lines,
        above_lines: above.len(),
        axes: bubbles.len(),
        model_axes,
        hidden_elements: hidden,
        legend,
        frame: crate::viewdraw::ViewFrame {
            turn: setup.turn,
            origin: scene.origin_mm,
            x0: lo[0],
            y1: hi[1],
            k,
            ox,
            oy,
            cut,
        },
    };
    let sheet = Sheet {
        width_mm: if o.bare { w.max(title_end) } else { w },
        height_mm: h,
        title: if o.project.is_empty() {
            title
        } else {
            format!("{} – {title}", o.project)
        },
        items,
        font: o.style.paper.font,
    };
    Ok((sheet, info))
}

/// The sheet frame and the template's title block along the bottom, for a `w` × `h` mm sheet.
pub(crate) fn frame_and_title_block(
    items: &mut Vec<Item>,
    w: f64,
    h: f64,
    tpl: &PlanTemplate,
    filled: &dyn Fn(&str) -> String,
    ink: Rgb,
    style: &GraphicStyle,
) {
    let t = style.paper.text_mm as f64;
    let tb_h = tpl.title_block.height_mm;
    let tb_top = h - MARGIN - tb_h;
    let text_at =
        |pos: P2, size: f64, s: String, anchor: Anchor, angle_deg: f64, bold: bool| Item::Text {
            pos,
            size_mm: size,
            text: s,
            color: ink,
            anchor,
            angle_deg,
            bold,
        };
    // ---- frame and title block
    let frame_pen = role_pen(style, Role::Frame, 100);
    let rect =
        |x0: f64, y0: f64, x1: f64, y1: f64| vec![[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]];
    items.push(Item::Lines {
        lines: vec![rect(MARGIN, MARGIN, w - MARGIN, h - MARGIN)],
        pen: frame_pen.clone(),
    });
    let frame_w = w - 2.0 * MARGIN;
    let cols = &tpl.title_block.columns;
    if !cols.is_empty() {
        let fixed: f64 = cols.iter().filter_map(|c| c.width_mm).sum();
        let flexible = cols.iter().filter(|c| c.width_mm.is_none()).count();
        let share = if flexible > 0 {
            ((frame_w - fixed) / flexible as f64).max(20.0)
        } else {
            0.0
        };
        let total = fixed + share * flexible as f64;
        // Squeeze proportionally when the columns are wider than the frame; stretch the last
        // column when they are narrower.
        let squeeze = (frame_w / total).min(1.0);
        let mut lines = vec![vec![[MARGIN, tb_top], [w - MARGIN, tb_top]]];
        let mut x = MARGIN;
        for (i, c) in cols.iter().enumerate() {
            let mut cw = c.width_mm.unwrap_or(share) * squeeze;
            if i + 1 == cols.len() {
                cw = w - MARGIN - x;
            }
            if i > 0 {
                lines.push(vec![[x, tb_top], [x, h - MARGIN]]);
            }
            let eh = tb_h / c.entries.len().max(1) as f64;
            for (j, e) in c.entries.iter().enumerate() {
                let ey = tb_top + j as f64 * eh;
                let mut y = ey + 2.0 + t * 1.1;
                if !e.label.is_empty() {
                    items.push(text_at(
                        [x + 2.5, y],
                        t * 1.1,
                        filled(&e.label),
                        Anchor::Start,
                        0.0,
                        true,
                    ));
                    y += t * 0.5;
                }
                let size = t * e.size;
                for line in filled(&e.value).split('\n') {
                    y += size * 1.25;
                    if y > ey + eh {
                        break;
                    }
                    items.push(Item::Text {
                        pos: [x + 2.5, y],
                        size_mm: size,
                        text: line.to_string(),
                        color: e.color.unwrap_or(ink),
                        anchor: Anchor::Start,
                        angle_deg: 0.0,
                        bold: false,
                    });
                }
            }
            x += cw;
        }
        items.push(Item::Lines {
            lines,
            pen: frame_pen,
        });
    }
    items.push(text_at(
        [w - MARGIN, h - MARGIN + 3.5],
        2.0,
        format!("Bimble {}", env!("CARGO_PKG_VERSION")),
        Anchor::End,
        0.0,
        false,
    ));
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn opts(storey: &str, scale: u32, paper: Option<Paper>) -> PlanOptions {
        PlanOptions {
            storey: storey.into(),
            scale,
            paper,
            project: "Demo".into(),
            date: "2026-11-05".into(),
            ..Default::default()
        }
    }

    fn demo() -> Scene {
        Scene::from_model(&concrete_frame(&FrameSpec::default()))
    }

    /// A view cropped to the left half of the frame (ADR 0046): a narrower page, fewer elements
    /// cut, and nothing filled outside the crop.
    #[test]
    fn a_cropped_view_draws_only_its_crop() {
        let scene = demo();
        let levels = scene.levels();
        let v = bimble_core::view::default_views(&levels)
            .into_iter()
            .find(|v| v.name.starts_with("Plan Niveau 1"))
            .unwrap();
        let draw = |v: &bimble_core::view::View| {
            let mut o = opts("", 100, None);
            o.view = Some(v.clone());
            o.bare = true;
            plan(&scene, &o).unwrap()
        };
        let (whole, wi) = draw(&v);
        // The frame spans x 0..18 m: keep x −1..9 m, all of y.
        let crop = bimble_core::view::Crop {
            min: [-1_000.0, -1e6],
            max: [9_000.0, 1e6],
        };
        let mut cropped = v.clone();
        cropped.crop = Some(crop);
        let (part, pi) = draw(&cropped);
        assert!(
            part.width_mm < whole.width_mm,
            "{} vs {}",
            part.width_mm,
            whole.width_mm
        );
        assert!(
            pi.cut_elements < wi.cut_elements,
            "{} vs {}",
            pi.cut_elements,
            wi.cut_elements
        );
        // The axes crossing the crop, not the others (their columns counted before cropping).
        assert!(
            pi.axes > 0 && pi.axes < wi.axes,
            "{} vs {}",
            pi.axes,
            wi.axes
        );
        let right = pi.frame.to_paper([crop.max[0], 0.0, 0.0])[0];
        for i in &part.items {
            if let Item::Area { loops, .. } = i {
                for p in loops.iter().flatten() {
                    assert!(p[0] <= right + 1e-6, "{p:?} beyond {right}");
                }
            }
        }
    }

    /// A bare drawing (a view on its own: On paper, on a sheet) whose title is longer than the
    /// drawing is wide: its page reaches the title's end. It ran off the page before.
    #[test]
    fn a_bare_page_holds_its_title() {
        let scene = demo();
        let levels = scene.levels();
        let mut v = bimble_core::view::default_views(&levels)
            .into_iter()
            .find(|v| matches!(v.kind, bimble_core::view::ViewKind::Plan { .. }))
            .unwrap();
        v.name = "A plan with a long name, as offices write them: level, zone and phase".into();
        let mut o = opts("", 200, None);
        o.view = Some(v);
        o.bare = true;
        let (sheet, _) = plan(&scene, &o).unwrap();
        let ends: Vec<f64> = sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Lines { lines, .. } => lines.iter().flatten().map(|p| p[0]).reduce(f64::max),
                _ => None,
            })
            .collect();
        let right = ends.into_iter().fold(0.0, f64::max);
        assert!(sheet.width_mm >= right, "{} < {right}", sheet.width_mm);
    }

    /// A material given a hatch type is drawn with it where cut (#44): its spacing in paper mm,
    /// its colour, its fill; the legend swatch follows.
    #[test]
    fn a_material_hatch_type_draws_cut_elements() {
        use bimble_core::hatches::MaterialHatch;
        let scene = demo();
        let first = scene
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone();
        let mut o = opts(&first, 50, Some(Paper::A1));
        o.style.paper.material_hatches = vec![MaterialHatch {
            category: MaterialCategory::Concrete,
            cut: Some("diagonals red".into()),
            seen: None,
        }];
        let red = o.style.paper.hatch_type("diagonals red").unwrap().color;
        let (sheet, _) = plan(&scene, &o).unwrap();
        let hatched: Vec<&Hatch> = sheet
            .items
            .iter()
            .flat_map(|i| match i {
                Item::Area { hatches, .. } => hatches.iter().collect(),
                _ => Vec::new(),
            })
            .filter(|h| h.pen.color == red)
            .collect();
        assert!(hatched.len() >= 12, "{}", hatched.len());
        assert!(
            hatched
                .iter()
                .all(|h| (h.spacing_mm - 2.0).abs() < 1e-9 && (h.angle_deg - 45.0).abs() < 1e-9)
        );
        // Without it, the family's own pattern: no red.
        let (sheet, _) = plan(&scene, &opts(&first, 50, Some(Paper::A1))).unwrap();
        assert!(!sheet.items.iter().any(|i| matches!(i,
            Item::Area { hatches, .. } if hatches.iter().any(|h| h.pen.color == red))));
    }

    /// Section labels (#24): each cut 300 × 300 column reads "30/30" beside it, each beam above
    /// the cut "25/50" along it; the template can turn them off.
    #[test]
    fn columns_and_beams_get_section_labels() {
        let scene = demo();
        let first = scene
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone();
        let count = |sheet: &crate::sheet::Sheet, want: &str| {
            sheet
                .items
                .iter()
                .filter(|i| matches!(i, Item::Text { text, .. } if text == want))
                .count()
        };
        let (sheet, _) = plan(&scene, &opts(&first, 50, Some(Paper::A1))).unwrap();
        assert_eq!(count(&sheet, "30/30"), 12);
        // 3 × 3 beams along x, 4 × 2 along y.
        assert_eq!(count(&sheet, "25/50"), 17);
        let along_y = sheet
            .items
            .iter()
            .filter(|i| {
                matches!(i, Item::Text { text, angle_deg, .. }
                if text == "25/50" && (angle_deg.abs() - 90.0).abs() < 1e-6)
            })
            .count();
        assert_eq!(along_y, 8);
        let mut o = opts(&first, 50, Some(Paper::A1));
        o.template.element_labels = false;
        let (sheet, _) = plan(&scene, &o).unwrap();
        assert_eq!(count(&sheet, "30/30") + count(&sheet, "25/50"), 0);
    }

    #[test]
    fn section_labels_are_written_in_cm_and_timber_carries_its_material() {
        assert_eq!(cm(300.0), "30");
        assert_eq!(cm(245.0), "24.5");
        assert_eq!(
            section_text(MaterialCategory::Timber, "GL24h", 240.0, 460.0),
            "GL24h 24x46"
        );
        assert_eq!(
            section_text(MaterialCategory::Concrete, "C30/37", 300.0, 300.0),
            "30/30"
        );
    }

    #[test]
    fn a_storey_plan_cuts_the_columns_at_scale() {
        let scene = demo();
        let first = scene
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone();
        let (sheet, info) = plan(&scene, &opts(&first, 100, Some(Paper::A3))).unwrap();
        assert_eq!(info.paper, Paper::A3);
        assert_eq!((sheet.width_mm, sheet.height_mm), (420.0, 297.0));
        // The demo frame: columns on a 4 × 3 grid are cut at +1.2 m.
        let cut_areas: Vec<&Item> = sheet
            .items
            .iter()
            .filter(|i| matches!(i, Item::Area { fill: Some(_), .. }))
            .collect();
        assert!(info.cut_elements >= 12, "{info:?}");
        assert!(cut_areas.len() >= 12);
        // A 300 × 300 mm column at 1:100 is a 3 × 3 mm square on paper.
        let sizes: Vec<(f64, f64)> = sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Area {
                    loops,
                    outline: Some(_),
                    fill: None,
                    ..
                } => {
                    let l = &loops[0];
                    let xs = l.iter().map(|p| p[0]);
                    let ys = l.iter().map(|p| p[1]);
                    Some((
                        xs.clone().fold(f64::NEG_INFINITY, f64::max)
                            - xs.fold(f64::INFINITY, f64::min),
                        ys.clone().fold(f64::NEG_INFINITY, f64::max)
                            - ys.fold(f64::INFINITY, f64::min),
                    ))
                }
                _ => None,
            })
            .collect();
        assert!(
            sizes
                .iter()
                .any(|(a, b)| (a - 3.0).abs() < 1e-6 && (b - 3.0).abs() < 1e-6),
            "{sizes:?}"
        );
    }

    #[test]
    fn auto_paper_takes_the_smallest_that_fits_and_errors_suggest_a_scale() {
        let scene = demo();
        let first = scene
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone();
        // With axes, dimensions, legend and title block the demo needs A3 at 1:100 …
        let (_, a) = plan(&scene, &opts(&first, 100, None)).unwrap();
        assert_eq!(a.paper, Paper::A3);
        // … because A4 is too small.
        assert!(plan(&scene, &opts(&first, 100, Some(Paper::A4))).is_err());
        let (_, b) = plan(&scene, &opts(&first, 20, None)).unwrap();
        assert!(b.paper > Paper::A4, "1:20 needs more paper: {b:?}");
        let e = plan(&scene, &opts(&first, 1, Some(Paper::A4))).unwrap_err();
        assert!(
            e.contains("does not fit on A4") && e.contains("fits on A4"),
            "{e}"
        );
    }

    #[test]
    fn bad_input_is_reported() {
        let scene = demo();
        let e = plan(&scene, &opts("nope", 100, None)).unwrap_err();
        assert!(e.contains("no storey") && e.contains("Storeys:"), "{e}");
        assert!(
            plan(&scene, &opts("x", 0, None))
                .unwrap_err()
                .contains("1:5000")
        );
        assert!(
            plan(&scene, &opts("x", 6000, None))
                .unwrap_err()
                .contains("1:5000")
        );
    }

    #[test]
    fn scale_suggestions_are_standard() {
        assert_eq!(next_standard_scale(87.0), Some(100));
        assert_eq!(next_standard_scale(100.0), Some(100));
        assert_eq!(next_standard_scale(6000.0), None);
    }

    /// A scene from boxes: (class, storey, triangles), one storey "L0" at 0 and "L1" at 3 m.
    fn boxes(items: Vec<(&str, Vec<bimble_geom::boolean::Tri>)>) -> Scene {
        use bimble_geom::{ElementInfo, MeshBuilder, SceneBuilder, StoreyLevel};
        let mut b = SceneBuilder::with_storeys(vec![
            StoreyLevel {
                name: "L0".into(),
                elevation_mm: 0.0,
                id: String::new(),
            },
            StoreyLevel {
                name: "L1".into(),
                elevation_mm: 3000.0,
                id: String::new(),
            },
        ]);
        for (i, (class, tris)) in items.into_iter().enumerate() {
            b.push(
                ElementInfo {
                    global_id: format!("id{i}"),
                    class: class.into(),
                    name: format!("e{i}"),
                    storey: "L0".into(),
                    material: "C30/37".into(),
                    category: MaterialCategory::Concrete,
                },
                MeshBuilder { tris },
            );
        }
        b.finish()
    }

    fn texts(sheet: &Sheet) -> Vec<String> {
        sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Text { text, .. } => Some(text.clone()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn model_grid_axes_get_bubbles_and_axis_to_axis_dimensions() {
        use bimble_geom::boolean::cuboid;
        let mut scene = boxes(vec![
            (
                "IfcColumn",
                cuboid([-150.0, -150.0, 0.0], [150.0, 150.0, 3000.0]),
            ),
            (
                "IfcColumn",
                cuboid([5850.0, -150.0, 0.0], [6150.0, 150.0, 3000.0]),
            ),
        ]);
        // Grids are in world mm, like the boxes; the scene itself is rebased to its origin.
        assert_ne!(scene.origin_mm, [0.0; 3]);
        let axis = |tag: &str, a: [f64; 2], b: [f64; 2]| bimble_geom::GridAxis {
            tag: tag.into(),
            start_mm: a,
            end_mm: b,
        };
        scene.grids = vec![
            axis("A", [0.0, -1000.0], [0.0, 1000.0]),
            axis("B", [6000.0, -1000.0], [6000.0, 1000.0]),
            axis("1", [-1000.0, 0.0], [7000.0, 0.0]),
        ];
        let (sheet, info) = plan(&scene, &opts("L0", 100, None)).unwrap();
        assert_eq!((info.axes, info.model_axes), (3, true));
        let t = texts(&sheet);
        for tag in ["A", "B", "1"] {
            assert!(t.iter().any(|x| x == tag), "bubble {tag}: {t:?}");
        }
        // Axis to axis 6 m in cm; overall 6.3 m (column faces); 15 cm from face to axis.
        for d in ["600", "630", "15"] {
            assert!(t.iter().any(|x| x == d), "dimension {d}: {t:?}");
        }
        // Without axes in the template: no bubbles, only the overall dimension.
        let mut no = opts("L0", 100, None);
        no.template.axes = crate::template::Axes::Off;
        let (sheet, info) = plan(&scene, &no).unwrap();
        assert_eq!(info.axes, 0);
        let t = texts(&sheet);
        assert!(!t.iter().any(|x| x == "600") && t.iter().any(|x| x == "630"));
    }

    #[test]
    fn columns_give_axes_when_the_model_has_no_grid() {
        let (sheet, info) = plan(&demo(), &opts(&demo_first(), 100, None)).unwrap();
        // The demo: 4 × 3 columns, so axes A–D and 1–3.
        assert_eq!((info.axes, info.model_axes), (7, false));
        let t = texts(&sheet);
        for tag in ["A", "D", "1", "3"] {
            assert!(t.iter().any(|x| x == tag), "{tag}: {t:?}");
        }
    }

    /// Grid axes are dash-dot (long dash, gap, dot, gap) with the axis pen, in SVG and in PDF.
    #[test]
    fn grid_axes_are_dash_dot() {
        let (sheet, _) = plan(&demo(), &opts(&demo_first(), 100, None)).unwrap();
        let svg = sheet.to_svg();
        assert!(
            svg.contains(r#"stroke-dasharray="12.000 1.500 0.500 1.500""#),
            "no dash-dot in the SVG"
        );
        // PDF content streams are compressed: decompress and look for the dash operator.
        let pdf = lopdf::Document::load_mem(&sheet.to_pdf()).unwrap();
        let page = *pdf.get_pages().values().next().unwrap();
        let content = String::from_utf8_lossy(&pdf.get_page_content(page)).into_owned();
        assert!(
            content.contains("[12 1.5 0.5 1.5] 0 d"),
            "no dash-dot in the PDF content"
        );
    }

    fn demo_first() -> String {
        demo()
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone()
    }

    /// Seen tops are painted low to high, lighter than cut fills; a rectangular hole in a seen
    /// slab gets a cross; hidden classes are left out.
    #[test]
    fn seen_tops_are_painted_low_to_high_with_opening_crosses() {
        use bimble_geom::boolean::{Op, apply, cuboid};
        let slab = apply(
            Op::Difference,
            &cuboid([0.0, 0.0, -200.0], [6000.0, 4000.0, 0.0]),
            &[&cuboid([1000.0, 1000.0, -300.0], [2000.0, 3000.0, 100.0])],
        )
        .unwrap();
        let scene = boxes(vec![
            // Listed high first: the order on paper must not depend on the file order.
            (
                "IfcBeam",
                cuboid([3000.0, 0.0, 0.0], [3300.0, 4000.0, 500.0]),
            ),
            ("IfcSlab", slab),
            (
                "IfcReinforcingBar",
                cuboid([100.0, 100.0, 0.0], [5900.0, 120.0, 20.0]),
            ),
            ("IfcWall", cuboid([0.0, 0.0, 0.0], [6000.0, 200.0, 3000.0])),
        ]);
        let (sheet, info) = plan(&scene, &opts("L0", 50, None)).unwrap();
        assert_eq!(info.hidden_elements, 1);
        assert_eq!(info.seen_elements, 2);
        assert_eq!(info.cut_elements, 1);
        let style = GraphicStyle::default();
        let seen = lighten(
            style.families.get(MaterialCategory::Concrete).color,
            style.paper.seen_lighten,
        );
        // Seen areas in paper order: the slab (top at 0) before the beam (top at 0.5 m).
        let seen_areas: Vec<usize> = sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Area {
                    loops,
                    fill: Some(f),
                    ..
                } if *f == seen => Some(loops.len()),
                _ => None,
            })
            .collect();
        // The first is the slab (outline + hole) and the second the beam … except the legend's
        // swatch, which comes after both.
        assert_eq!(&seen_areas[..2], &[2, 1], "{seen_areas:?}");
        // The hole is 1 × 2 m: a cross of two diagonals, 20 × 40 mm on paper at 1:50.
        let cross = sheet.items.iter().any(|i| match i {
            Item::Lines { lines, .. } => {
                lines.len() == 2
                    && lines.iter().all(|l| {
                        l.len() == 2
                            && ((l[1][0] - l[0][0]).abs() - 20.0).abs() < 1e-6
                            && ((l[1][1] - l[0][1]).abs() - 40.0).abs() < 1e-6
                    })
            }
            _ => false,
        });
        assert!(cross, "no opening cross");
        // The slab's level is written on it.
        assert!(texts(&sheet).iter().any(|t| t == "+0.00"));
        // Without seen fills, nothing is filled lighter.
        let mut flat = opts("L0", 50, None);
        flat.template.seen_fills = false;
        let (sheet, info) = plan(&scene, &flat).unwrap();
        assert_eq!(info.seen_elements, 0);
        assert!(
            !sheet
                .items
                .iter()
                .any(|i| matches!(i, Item::Area { fill: Some(f), .. } if *f == seen))
        );
    }

    /// A rectangular slab opening (#24): its size inside it, and its position from the nearest
    /// axes (here through the columns), in the template's unit.
    #[test]
    fn slab_openings_get_their_size_and_position() {
        use bimble_geom::boolean::{Op, apply, cuboid};
        let slab = apply(
            Op::Difference,
            &cuboid([0.0, 0.0, -200.0], [6000.0, 4000.0, 0.0]),
            &[&cuboid([1000.0, 1000.0, -300.0], [2000.0, 3000.0, 100.0])],
        )
        .unwrap();
        let mut list = vec![("IfcSlab", slab)];
        for (x, y) in [(0.0, 0.0), (6000.0, 0.0), (0.0, 4000.0), (6000.0, 4000.0)] {
            list.push((
                "IfcColumn",
                cuboid([x - 150.0, y - 150.0, 0.0], [x + 150.0, y + 150.0, 3000.0]),
            ));
        }
        let scene = boxes(list);
        let (sheet, _) = plan(&scene, &opts("L0", 50, None)).unwrap();
        let t = texts(&sheet);
        assert!(t.iter().any(|x| x == "100x200"), "{t:?}");
        // 1 m from axis x = 0 to its left side, 1 m from axis y = 0 to its bottom.
        assert_eq!(t.iter().filter(|x| *x == "100").count(), 2, "{t:?}");
        assert_eq!(t.iter().filter(|x| *x == "30/30").count(), 4, "{t:?}");
        let mut o = opts("L0", 50, None);
        o.template.opening_dimensions = false;
        let (sheet, _) = plan(&scene, &o).unwrap();
        assert!(!texts(&sheet).iter().any(|x| x == "100x200"));
    }

    /// A section view is marked on the plans it crosses (#24): a dash-dot line across the
    /// drawing, its tag at both ends, arrows looking its way; not when it misses the drawing.
    #[test]
    fn sections_are_marked_on_plans() {
        use bimble_core::view::{Facing, View, ViewKind};
        let scene = demo();
        let first = scene
            .storeys
            .iter()
            .find(|s| s.elevation_mm >= 0.0)
            .unwrap()
            .name
            .clone();
        let section = |name: &str, y: f64| View {
            id: bimble_core::GlobalId::new(),
            name: name.into(),
            kind: ViewKind::Section {
                origin: [3000.0, y],
                facing: Facing::North,
                depth_mm: 0.0,
            },
            visibility: Default::default(),
            references: Vec::new(),
            annotations: Vec::new(),
            true_north: false,
            scale: 100,
            crop: None,
        };
        let views = vec![section("Coupe S1", 2500.0), section("Far away", 90_000.0)];
        let marks = section_marks(&views);
        assert_eq!(marks[0].tag, "S1");
        assert_eq!(marks[1].tag, "B");
        let triangles = |sheet: &crate::sheet::Sheet| -> Vec<Vec<[f64; 2]>> {
            sheet
                .items
                .iter()
                .filter_map(|i| match i {
                    Item::Area {
                        loops,
                        outline: None,
                        ..
                    } if loops.len() == 1 && loops[0].len() == 3 => Some(loops[0].clone()),
                    _ => None,
                })
                .collect()
        };
        let mut o = opts(&first, 50, Some(Paper::A1));
        let (bare, _) = plan(&scene, &o).unwrap();
        o.section_marks = marks;
        let (sheet, _) = plan(&scene, &o).unwrap();
        let t = texts(&sheet);
        assert_eq!(t.iter().filter(|x| *x == "S1").count(), 2, "{t:?}");
        // The far section adds nothing; S1 two arrows, pointing up the paper (north): the tip,
        // first, above the other two corners.
        let (before, after) = (triangles(&bare), triangles(&sheet));
        assert_eq!(after.len(), before.len() + 2);
        for tri in after.iter().filter(|x| !before.contains(x)) {
            assert!(tri[0][1] < tri[1][1] && tri[0][1] < tri[2][1], "{tri:?}");
        }
        assert_eq!(
            t.iter().filter(|x| *x == "B").count(),
            texts(&bare).iter().filter(|x| *x == "B").count()
        );
        // Off by template: none.
        o.template.section_markers = false;
        let (sheet, _) = plan(&scene, &o).unwrap();
        assert!(!texts(&sheet).iter().any(|x| x == "S1"));
    }

    #[test]
    fn template_texts_fill_the_title_block() {
        let mut o = opts(&demo_first(), 100, Some(Paper::A2));
        o.template = crate::template::PlanTemplate::from_json(
            r#"{"title": "Plan {storey}, éch 1 : {n}",
                "title_block": {"columns": [{"entries": [{"label": "PHASE", "value": "{phase}"}]}]},
                "fields": {"phase": "Tender"}}"#,
        )
        .unwrap();
        let (sheet, _) = plan(&demo(), &o).unwrap();
        let t = texts(&sheet);
        let first = demo_first();
        assert!(t.contains(&format!("Plan {first}, éch 1 : 100")), "{t:?}");
        assert!(t.contains(&"PHASE".to_string()) && t.contains(&"Tender".to_string()));
        assert!(sheet.title.ends_with(&format!("Plan {first}, éch 1 : 100")));
    }

    /// A convex prism: `profile` in the x–z plane, extruded from y0 to y1, faces outwards.
    fn prism(profile: &[[f64; 2]], y0: f64, y1: f64) -> Vec<bimble_geom::boolean::Tri> {
        let n = profile.len();
        let p = |k: usize, y: f64| [profile[k % n][0], y, profile[k % n][1]];
        let mut tris = Vec::new();
        for k in 0..n {
            tris.push([p(k, y0), p(k + 1, y0), p(k + 1, y1)]);
            tris.push([p(k, y0), p(k + 1, y1), p(k, y1)]);
        }
        for k in 1..n - 1 {
            tris.push([p(0, y0), p(k, y0), p(k + 1, y0)]);
            tris.push([p(0, y1), p(k + 1, y1), p(k, y1)]);
        }
        // Turn each triangle outwards (the solid is convex).
        let c = {
            let all: Vec<[f64; 3]> = tris.iter().flatten().copied().collect();
            let s = all
                .iter()
                .fold([0.0; 3], |a, q| [a[0] + q[0], a[1] + q[1], a[2] + q[2]]);
            s.map(|v| v / all.len() as f64)
        };
        for t in &mut tris {
            let (u, v) = (
                [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]],
                [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]],
            );
            let nrm = [
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            ];
            let out = (0..3).map(|k| nrm[k] * (t[0][k] - c[k])).sum::<f64>();
            if out < 0.0 {
                t.swap(1, 2);
            }
        }
        tris
    }

    fn seen_colour() -> bimble_core::style::Rgb {
        let style = GraphicStyle::default();
        lighten(
            style.families.get(MaterialCategory::Concrete).color,
            style.paper.seen_lighten,
        )
    }

    /// Bounding boxes (paper mm) of the areas filled as seen from above (not the legend's
    /// 10 × 4 mm swatch).
    fn seen_boxes(sheet: &Sheet) -> Vec<(P2, P2)> {
        let seen = seen_colour();
        sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Area {
                    loops,
                    fill: Some(f),
                    ..
                } if *f == seen => bounds(loops.iter().flatten()),
                _ => None,
            })
            .filter(|(lo, hi)| hi[0] - lo[0] > 12.0 || hi[1] - lo[1] > 6.0)
            .collect()
    }

    /// A ramp seen below the cut is filled over its whole area, not only near its highest
    /// point (#22): 4 × 2 m at 1:100 is 40 × 20 mm on paper.
    #[test]
    fn a_sloped_slab_below_the_cut_is_filled_over_its_whole_area() {
        let ramp = prism(
            &[[0.0, 0.0], [4000.0, 0.0], [4000.0, 800.0], [0.0, 200.0]],
            0.0,
            2000.0,
        );
        let scene = boxes(vec![("IfcSlab", ramp)]);
        let (sheet, info) = plan(&scene, &opts("L0", 100, None)).unwrap();
        assert_eq!(info.seen_elements, 1);
        let b = seen_boxes(&sheet);
        assert_eq!(b.len(), 1, "{b:?}");
        let (lo, hi) = b[0];
        assert!(
            (hi[0] - lo[0] - 40.0).abs() < 0.1 && (hi[1] - lo[1] - 20.0).abs() < 0.1,
            "seen area {:.2} × {:.2} mm",
            hi[0] - lo[0],
            hi[1] - lo[1]
        );
    }

    /// A downstand beam under a slab (one solid) is hidden by the slab: its edges, which face
    /// away from above, aren't drawn over the slab (#22).
    #[test]
    fn a_downstand_under_its_slab_is_not_drawn() {
        use bimble_geom::boolean::{Op, apply, cuboid};
        let solid = apply(
            Op::Union,
            &cuboid([0.0, 0.0, 200.0], [2000.0, 1000.0, 400.0]),
            &[&cuboid([800.0, 0.0, 0.0], [1200.0, 1000.0, 200.0])],
        )
        .unwrap();
        let scene = boxes(vec![("IfcSlab", solid)]);
        let mut o = opts("L0", 100, None);
        o.template.dimensions = false;
        let (sheet, _) = plan(&scene, &o).unwrap();
        let b = seen_boxes(&sheet);
        assert_eq!(b.len(), 1, "{b:?}");
        let (lo, hi) = b[0];
        // The slab is 20 × 10 mm on paper; lines strictly inside it are the downstand's.
        let inside = |p: &P2| p[0] > lo[0] + 0.5 && p[0] < hi[0] - 0.5;
        let lines_inside: usize = sheet
            .items
            .iter()
            .filter_map(|i| match i {
                Item::Lines { lines, .. } => Some(lines),
                _ => None,
            })
            .flatten()
            .filter(|l| {
                l.iter()
                    .all(|p| inside(p) && p[1] > lo[1] - 0.5 && p[1] < hi[1] + 0.5)
            })
            .count();
        assert_eq!(lines_inside, 0, "the downstand shows through its slab");
    }

    fn view_named(scene: &Scene, name: &str) -> bimble_core::view::View {
        bimble_core::view::default_views(&scene.levels())
            .into_iter()
            .find(|v| v.name == name)
            .unwrap()
    }

    /// A plan view with the default range draws the same sheet as the storey plan it replaces.
    #[test]
    fn a_plan_view_draws_like_the_storey_plan() {
        let scene = demo();
        let first = demo_first();
        let classic = plan(&scene, &opts(&first, 100, None)).unwrap().0;
        let mut o = opts("", 100, None);
        o.view = Some(view_named(&scene, &format!("Plan {first}")));
        let by_view = plan(&scene, &o).unwrap().0;
        assert_eq!(by_view.to_svg(), classic.to_svg());
    }

    /// A ceiling plan (looking up) and a section have paper drawings: cut columns, titles by
    /// the view's name.
    #[test]
    fn ceiling_plans_and_sections_are_drawn() {
        let scene = demo();
        let first = demo_first();
        let mut o = opts("", 100, None);
        o.view = Some(view_named(&scene, &format!("Ceiling {first}")));
        let (sheet, info) = plan(&scene, &o).unwrap();
        // The 12 columns are cut at +2.3 m; the slab above is seen from below.
        assert!(info.cut_elements >= 12, "{info:?}");
        assert!(info.seen_elements >= 1, "{info:?}");
        assert!(
            texts(&sheet)
                .iter()
                .any(|t| t == &format!("Ceiling {first}, 1:100"))
        );

        let section = bimble_core::view::View {
            id: bimble_core::GlobalId::new(),
            name: "Section A".into(),
            kind: bimble_core::view::ViewKind::Section {
                origin: [9000.0, 5000.0],
                facing: bimble_core::view::Facing::North,
                depth_mm: 0.0,
            },
            visibility: Default::default(),
            references: Vec::new(),
            annotations: Vec::new(),
            true_north: false,
            scale: 100,
            crop: None,
        };
        o.view = Some(section);
        let (sheet, info) = plan(&scene, &o).unwrap();
        // Through the middle row: 4 columns, slabs and beams along x are cut.
        assert!(info.cut_elements >= 4, "{info:?}");
        assert_eq!(info.axes, 0);
        assert!(texts(&sheet).iter().any(|t| t == "Section A, 1:100"));
        // A section is taller than wide here? No: 18 m wide, about 7 m high.
        assert!(sheet.width_mm > 0.0);
    }

    /// A view hides what its visibility says, on paper too.
    #[test]
    fn a_view_hides_classes_on_paper() {
        let scene = demo();
        let mut v = view_named(&scene, &format!("Plan {}", demo_first()));
        let all = {
            let mut o = opts("", 100, None);
            o.view = Some(v.clone());
            plan(&scene, &o).unwrap().1
        };
        v.visibility.hidden_classes = vec!["IfcColumn".into()];
        let mut o = opts("", 100, None);
        o.view = Some(v);
        let (_, info) = plan(&scene, &o).unwrap();
        assert!(info.cut_elements < all.cut_elements, "{info:?} vs {all:?}");
    }
}
