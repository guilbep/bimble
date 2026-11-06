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

use crate::cut::{P2, clip_z, section};
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
fn letter_tag(mut i: usize) -> String {
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
fn clusters(mut vals: Vec<f64>, tol: f64) -> Vec<f64> {
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
    let storeys: Vec<String> = scene
        .storeys
        .iter()
        .map(|s| format!("{} ({:+.3} m)", s.name, s.elevation_mm / 1000.0))
        .collect();
    let idx = scene
        .storeys
        .iter()
        .position(|s| s.name == o.storey)
        .or_else(|| {
            scene
                .storeys
                .iter()
                .position(|s| s.name.eq_ignore_ascii_case(&o.storey))
        })
        .ok_or_else(|| {
            if storeys.is_empty() {
                "this model has no storeys".to_string()
            } else {
                format!("no storey {:?}. Storeys: {}", o.storey, storeys.join(", "))
            }
        })?;
    let st = &scene.storeys[idx];
    let oz = scene.origin_mm[2];
    let level = st.elevation_mm - oz;
    let cut = level + o.cut_above_mm;
    let next = scene.storeys[idx + 1..]
        .iter()
        .map(|s| s.elevation_mm - oz)
        .find(|&z| z > level + 1.0)
        .unwrap_or(f64::INFINITY);
    let style = &o.style;
    let paper_style = &style.paper;

    // ---- model geometry (scene mm)
    let mut projection: Vec<[P2; 2]> = Vec::new();
    let mut above: Vec<[P2; 2]> = Vec::new();
    let mut cuts: Vec<Cut> = Vec::new();
    let mut seen: Vec<Seen> = Vec::new();
    let mut hidden = 0;
    let long = |s: &[P2; 2]| (s[1][0] - s[0][0]).hypot(s[1][1] - s[0][1]) > 0.01;
    for e in &scene.elements {
        if tpl.hides(&e.class) {
            hidden += 1;
            continue;
        }
        let m = &e.mesh;
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
                    loops: s.loops,
                    open: s.open,
                });
            }
        }
        let edge = |seg: &[[f32; 3]; 2]| (seg[0].map(f64::from), seg[1].map(f64::from));
        if zmax >= level - 1.0 && zmin <= cut {
            let edges: Vec<[P2; 2]> = m
                .edges
                .iter()
                .filter_map(|seg| {
                    let (a, b) = edge(seg);
                    clip_z(a, b, level - 1.0, cut)
                })
                .filter(long)
                .collect();
            if tpl.seen_fills && !is_cut {
                // Its top: a section just under its highest point.
                let top = if zmax - zmin > 2.0 {
                    section(m, zmax - 1.0).loops
                } else {
                    Vec::new()
                };
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
        if e.storey == st.name && zmax > cut {
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

    let Some((blo, bhi)) = bounds(
        cuts.iter()
            .flat_map(|c| c.loops.iter().chain(&c.open))
            .flatten()
            .chain(seen.iter().flat_map(|s| s.top.iter().flatten()))
            .chain(seen.iter().flat_map(|s| s.edges.iter().flatten()))
            .chain(projection.iter().flatten())
            .chain(above.iter().flatten()),
    ) else {
        return Err(format!(
            "nothing to draw at storey {} (cut at {:+.3} m)",
            st.name,
            (cut + oz) / 1000.0
        ));
    };

    // ---- grid axes (scene mm)
    let mut axes: Vec<Axis> = Vec::new();
    let mut model_axes = false;
    if tpl.axes != Axes::Off {
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
            let centres: Vec<P2> = cuts
                .iter()
                .filter(|c| c.class.eq_ignore_ascii_case("IfcColumn"))
                .filter_map(|c| bounds(c.loops.iter().flatten()))
                .map(|(l, h)| [(l[0] + h[0]) / 2.0, (l[1] + h[1]) / 2.0])
                .collect();
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
    enum Swatch {
        Cut(MaterialCategory),
        Seen(MaterialCategory),
        Above,
    }
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
    let legend_cols = legend.len().div_ceil(LEGEND_ROWS);
    let legend_h = if legend.is_empty() {
        0.0
    } else {
        10.0 + legend.len().min(LEGEND_ROWS) as f64 * LEGEND_ROW + PAD
    };

    // ---- paper
    let tb_h = tpl.title_block.height_mm;
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
    let room = |w: f64, h: f64| (w - 2.0 * MARGIN, h - 2.0 * MARGIN - tb_h - legend_h);
    let legend_w = legend_cols as f64 * LEGEND_COL + 4.0;
    let fits = |p: Paper, portrait: bool| {
        let (w, h) = p.size();
        let (w, h) = if portrait { (h, w) } else { (w, h) };
        let (rw, rh) = room(w, h);
        need.0 <= rw && need.1 <= rh && legend_w <= rw
    };
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
    let (rw, rh) = room(w, h);
    // Centre the drawing and its decorations in the space above the legend and title block.
    let ox = MARGIN + PAD + left_extra + (rw - need.0) / 2.0;
    let oy = MARGIN + PAD + top_extra + (rh - need.1) / 2.0;
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
    let pen = |width: f32, dash: Option<[f64; 2]>| Pen {
        width_mm: width as f64,
        color: ink,
        dash,
    };
    let proj_pen = pen(paper_style.projection_pen_mm, None);
    let note_pen = pen(paper_style.annotation_pen_mm, None);
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
    let mm_per_px = paper_style.hatch_mm_per_px as f64;
    let hatches_of = |c: MaterialCategory| -> Vec<Hatch> {
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
                pen: Pen {
                    width_mm: paper_style.hatch_pen_mm as f64,
                    color: hatch_ink,
                    dash: l.dash_px.map(|d| {
                        let d = d as f64 * mm_per_px;
                        [d * l.dash_on as f64, d * (1.0 - l.dash_on as f64)]
                    }),
                },
            })
            .collect()
    };
    let seen_fill =
        |c: MaterialCategory| lighten(style.families.get(c).color, paper_style.seen_lighten);

    // ---- items, back to front
    let mut items = Vec::new();
    if !projection.is_empty() {
        items.push(Item::Lines {
            lines: seg_lines(&projection),
            pen: proj_pen.clone(),
        });
    }
    let mut labels = Vec::new();
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
        items.push(Item::Area {
            loops: paper_loops(&s.top),
            fill: Some(seen_fill(s.cat)),
            hatches: Vec::new(),
            outline: Some(proj_pen.clone()),
        });
        let slab = s.class.eq_ignore_ascii_case("IfcSlab");
        if slab && tpl.opening_crosses {
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
                    crosses.push(vec![p, q]);
                    crosses.push(vec![[p[0], q[1]], [q[0], p[1]]]);
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
        let d = paper_style.above_dash_mm;
        items.push(Item::Lines {
            lines: seg_lines(&above),
            pen: pen(paper_style.above_pen_mm, Some([d[0] as f64, d[1] as f64])),
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
            pen: Pen {
                dash: Some([8.0, 1.0]),
                ..note_pen.clone()
            },
        });
    }

    // Fills and hatches of all cut elements first, then all outlines on top: touching elements
    // (a column against a wall) keep both outlines.
    for c in &cuts {
        if c.loops.is_empty() {
            continue;
        }
        items.push(Item::Area {
            loops: paper_loops(&c.loops),
            fill: Some(style.families.get(c.cat).color),
            hatches: hatches_of(c.cat),
            outline: None,
        });
    }
    for c in &cuts {
        if !c.loops.is_empty() {
            items.push(Item::Area {
                loops: paper_loops(&c.loops),
                fill: None,
                hatches: Vec::new(),
                outline: Some(pen(paper_style.cut_pen_mm, None)),
            });
        }
        if !c.open.is_empty() {
            items.push(Item::Lines {
                lines: paper_loops(&c.open),
                pen: pen(paper_style.cut_pen_mm, None),
            });
        }
    }

    // Bubbles over everything.
    for (c, tag) in &bubbles {
        items.push(Item::Area {
            loops: vec![circle(*c, BUBBLE / 2.0)],
            fill: Some(Rgb([255, 255, 255])),
            hatches: Vec::new(),
            outline: Some(pen(paper_style.projection_pen_mm, None)),
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
            let (s, anchor) = if text_width_mm(&value, dim_size, false) + 0.6 <= w[1] - w[0] {
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
            pen: pen(paper_style.projection_pen_mm * 2.0, None),
        });
        for (pos, value, angle, anchor) in dim_texts {
            items.push(text_at(pos, dim_size, value, anchor, angle, false));
        }
    }

    // ---- texts
    let n = o.scale.to_string();
    let values = [
        ("project", o.project.clone()),
        ("storey", st.name.clone()),
        ("level", format!("{:+.2}", st.elevation_mm / 1000.0 + 0.0)),
        ("scale", format!("1:{n}")),
        ("n", n.clone()),
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
    let title = filled(&tpl.title);
    let title_y = oy + model.1 * k + PAD + TITLE_BAND - 4.0;
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
        let tw = text_width_mm(&title, size, true);
        items.push(Item::Lines {
            lines: vec![vec![[x, title_y + 1.0], [x + tw, title_y + 1.0]]],
            pen: pen(paper_style.cut_pen_mm * 0.7, None),
        });
    }
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
    if !legend.is_empty() {
        let (x0, y0) = (MARGIN, tb_top - legend_h + PAD);
        let (x1, y1) = (x0 + legend_w, tb_top);
        items.push(Item::Lines {
            lines: vec![vec![[x0, y0], [x1, y0], [x1, y1], [x0, y1]]],
            pen: proj_pen.clone(),
        });
        items.push(text_at(
            [x0 + 3.0, y0 + 5.5],
            t,
            tpl.legend.title.clone(),
            Anchor::Start,
            0.0,
            true,
        ));
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
                Swatch::Cut(c) => items.push(Item::Area {
                    loops: rect,
                    fill: Some(style.families.get(*c).color),
                    hatches: hatches_of(*c),
                    outline: Some(pen(paper_style.cut_pen_mm, None)),
                }),
                Swatch::Seen(c) => items.push(Item::Area {
                    loops: rect,
                    fill: Some(seen_fill(*c)),
                    hatches: Vec::new(),
                    outline: Some(proj_pen.clone()),
                }),
                Swatch::Above => {
                    let d = paper_style.above_dash_mm;
                    items.push(Item::Lines {
                        lines: vec![vec![[sx, sy + 2.0], [sx + 10.0, sy + 2.0]]],
                        pen: pen(paper_style.above_pen_mm, Some([d[0] as f64, d[1] as f64])),
                    });
                }
            }
            items.push(text([sx + 13.0, sy + 3.3], t * 0.9, label.clone()));
        }
    }

    // ---- frame and title block
    let frame_pen = pen(paper_style.frame_pen_mm, None);
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
    };
    let sheet = Sheet {
        width_mm: w,
        height_mm: h,
        title: if o.project.is_empty() {
            title
        } else {
            format!("{} – {title}", o.project)
        },
        items,
    };
    Ok((sheet, info))
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
}
