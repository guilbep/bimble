//! Storey plans at scale.
//!
//! A plan is a horizontal cut at `cut_above_mm` over the storey level, seen from above:
//! - what the plane cuts: filled, hatched like its material, outlined with the cut pen;
//! - visible edges between the storey level and the cut: thin lines (projection);
//! - edges of the storey's own elements above the cut, up to the next storey: dashed.
//!
//! No hidden-line removal yet: projected edges under other projected elements still show (cut
//! areas are drawn over them).

use crate::cut::{P2, clip_z, section};
use crate::sheet::{Hatch, Item, Paper, Pen, Sheet};
use bimble_core::MaterialCategory;
use bimble_core::style::{GraphicStyle, Rgb};
use bimble_geom::Scene;

/// Scales offered when a plan doesn't fit (1:N).
pub const STANDARD_SCALES: [u32; 18] = [
    1, 2, 5, 10, 20, 25, 50, 75, 100, 125, 200, 250, 500, 1000, 1250, 2000, 2500, 5000,
];

/// Sheet margin around the frame, mm.
const MARGIN: f64 = 10.0;
/// Height of the title block band at the bottom of the frame, mm.
const TITLE_H: f64 = 32.0;
/// Gap between the drawing and the frame or title block, mm.
const PAD: f64 = 5.0;

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
}

/// What `plan` produced, for reporting.
#[derive(Clone, Debug, PartialEq)]
pub struct PlanInfo {
    pub paper: Paper,
    pub portrait: bool,
    pub cut_elements: usize,
    pub projection_lines: usize,
    pub above_lines: usize,
}

fn darken(c: Rgb, k: f32) -> Rgb {
    Rgb(c.0.map(|v| (v as f32 * (1.0 - k)).round() as u8))
}

/// The smallest standard scale at or above `n` (a coarser drawing).
fn next_standard_scale(n: f64) -> Option<u32> {
    STANDARD_SCALES.into_iter().find(|&s| s as f64 >= n)
}

/// What the plane cuts out of one element: its family, closed loops, open chains.
type Cut = (MaterialCategory, Vec<Vec<P2>>, Vec<Vec<P2>>);

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
    for e in &scene.elements {
        let m = &e.mesh;
        let (zmin, zmax) = m
            .positions
            .iter()
            .fold((f64::INFINITY, f64::NEG_INFINITY), |(a, b), p| {
                (a.min(p[2] as f64), b.max(p[2] as f64))
            });
        if zmin <= cut && zmax >= cut {
            let s = section(m, cut);
            if !s.loops.is_empty() || !s.open.is_empty() {
                cuts.push((e.category, s.loops, s.open));
            }
        }
        let edge = |seg: &[[f32; 3]; 2]| (seg[0].map(f64::from), seg[1].map(f64::from));
        if zmax >= level - 1.0 && zmin <= cut {
            for seg in &m.edges {
                let (a, b) = edge(seg);
                if let Some(s) = clip_z(a, b, level - 1.0, cut) {
                    projection.push(s);
                }
            }
        }
        if e.storey == st.name && zmax > cut {
            for seg in &m.edges {
                let (a, b) = edge(seg);
                if let Some(s) = clip_z(a, b, cut, next) {
                    above.push(s);
                }
            }
        }
    }
    let long = |s: &[P2; 2]| (s[1][0] - s[0][0]).hypot(s[1][1] - s[0][1]) > 0.01;
    projection.retain(long);
    above.retain(long);

    let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
    let mut grow = |p: &P2| {
        for k in 0..2 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    };
    for (_, loops, open) in &cuts {
        loops.iter().chain(open).flatten().for_each(&mut grow);
    }
    projection
        .iter()
        .chain(&above)
        .flatten()
        .for_each(&mut grow);
    if lo[0] > hi[0] {
        return Err(format!(
            "nothing to draw at storey {} (cut at {:+.3} m)",
            st.name,
            (cut + oz) / 1000.0
        ));
    }

    // ---- paper
    let k = 1.0 / o.scale as f64;
    let need = (
        (hi[0] - lo[0]) * k + 2.0 * PAD,
        (hi[1] - lo[1]) * k + 2.0 * PAD,
    );
    let room = |w: f64, h: f64| (w - 2.0 * MARGIN, h - 2.0 * MARGIN - TITLE_H);
    let fits = |p: Paper, portrait: bool| {
        let (w, h) = p.size();
        let (w, h) = if portrait { (h, w) } else { (w, h) };
        let (rw, rh) = room(w, h);
        need.0 <= rw && need.1 <= rh
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
        let (bw, bh) = room(biggest.size().0, biggest.size().1);
        let model = ((hi[0] - lo[0]) / 1000.0, (hi[1] - lo[1]) / 1000.0);
        // The scale at which the drawing would fit the biggest paper (either orientation).
        let fit = |w: f64, h: f64| {
            ((hi[0] - lo[0]) / (w - 2.0 * PAD)).max((hi[1] - lo[1]) / (h - 2.0 * PAD))
        };
        let n = fit(bw, bh).min(fit(bh + TITLE_H, bw - TITLE_H));
        let hint = match next_standard_scale(n) {
            Some(s) => format!("1:{s} fits on {}", biggest.name()),
            None => "no scale up to 1:5000 fits".into(),
        };
        return Err(format!(
            "at 1:{} the plan ({:.1} × {:.1} m) needs {:.0} × {:.0} mm and does not fit on {}; {hint}",
            o.scale,
            model.0,
            model.1,
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
    // Centre the drawing in the space above the title block.
    let ox = MARGIN + (rw - (hi[0] - lo[0]) * k) / 2.0;
    let oy = MARGIN + (rh - (hi[1] - lo[1]) * k) / 2.0;
    let to_paper = |p: &P2| [ox + (p[0] - lo[0]) * k, oy + (hi[1] - p[1]) * k];
    let seg_lines = |v: &[[P2; 2]]| -> Vec<Vec<P2>> {
        v.iter()
            .map(|s| vec![to_paper(&s[0]), to_paper(&s[1])])
            .collect()
    };
    let ink = style.edges;
    let pen = |width: f32, dash: Option<[f64; 2]>| Pen {
        width_mm: width as f64,
        color: ink,
        dash,
    };

    // ---- items, back to front
    let mut items = Vec::new();
    if !projection.is_empty() {
        items.push(Item::Lines {
            lines: seg_lines(&projection),
            pen: pen(paper_style.projection_pen_mm, None),
        });
    }
    if !above.is_empty() {
        let d = paper_style.above_dash_mm;
        items.push(Item::Lines {
            lines: seg_lines(&above),
            pen: pen(paper_style.above_pen_mm, Some([d[0] as f64, d[1] as f64])),
        });
    }
    let mm_per_px = paper_style.hatch_mm_per_px as f64;
    // Fills and hatches of all cut elements first, then all outlines on top: touching elements
    // (a column against a wall) keep both outlines.
    for (cat, loops, _) in &cuts {
        if loops.is_empty() {
            continue;
        }
        let fam = style.families.get(*cat);
        let hatch_ink = fam
            .ink
            .unwrap_or_else(|| darken(fam.color, style.hatch_darken));
        let hatches = fam
            .lines
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
            .collect();
        items.push(Item::Area {
            loops: loops
                .iter()
                .map(|l| l.iter().map(to_paper).collect())
                .collect(),
            fill: Some(fam.color),
            hatches,
            outline: None,
        });
    }
    for (_, loops, open) in &cuts {
        if !loops.is_empty() {
            items.push(Item::Area {
                loops: loops
                    .iter()
                    .map(|l| l.iter().map(to_paper).collect())
                    .collect(),
                fill: None,
                hatches: Vec::new(),
                outline: Some(pen(paper_style.cut_pen_mm, None)),
            });
        }
        if !open.is_empty() {
            items.push(Item::Lines {
                lines: open
                    .iter()
                    .map(|l| l.iter().map(to_paper).collect())
                    .collect(),
                pen: pen(paper_style.cut_pen_mm, None),
            });
        }
    }

    // ---- frame, title block, scale bar
    let frame_pen = pen(paper_style.frame_pen_mm, None);
    let thin = pen(paper_style.projection_pen_mm, None);
    let rect =
        |x0: f64, y0: f64, x1: f64, y1: f64| vec![[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]];
    items.push(Item::Lines {
        lines: vec![rect(MARGIN, MARGIN, w - MARGIN, h - MARGIN)],
        pen: frame_pen.clone(),
    });
    let tb_w = (w - 2.0 * MARGIN).min(180.0);
    let (tx0, ty0, tx1, ty1) = (
        w - MARGIN - tb_w,
        h - MARGIN - TITLE_H,
        w - MARGIN,
        h - MARGIN,
    );
    items.push(Item::Lines {
        lines: vec![
            rect(tx0, ty0, tx1, ty1),
            vec![[tx0, ty0 + 12.0], [tx1, ty0 + 12.0]],
        ],
        pen: frame_pen,
    });
    let t = paper_style.text_mm as f64;
    let text = |x: f64, y: f64, size: f64, s: String| Item::Text {
        pos: [x, y],
        size_mm: size,
        text: s,
        color: ink,
    };
    items.push(text(tx0 + 3.0, ty0 + 8.5, t * 1.6, o.project.clone()));
    items.push(text(
        tx0 + 3.0,
        ty0 + 18.0,
        t,
        format!(
            "Plan {}  ·  cut {:+.2} m ({:+.3} m)",
            st.name,
            o.cut_above_mm / 1000.0,
            (cut + oz) / 1000.0
        ),
    ));
    items.push(text(
        tx0 + 3.0,
        ty0 + 25.0,
        t,
        format!(
            "Scale 1:{}  ·  {}{}  ·  {}",
            o.scale,
            paper.name(),
            if portrait { " portrait" } else { "" },
            o.date
        ),
    ));
    items.push(text(
        tx1 - 32.0,
        ty1 - 2.5,
        t * 0.7,
        format!("Bimble {}", env!("CARGO_PKG_VERSION")),
    ));
    // Graphic scale bar left of the title block: a round length about 50 mm long on paper.
    let target_m = 50.0 / k / 1000.0;
    let mag = 10f64.powf(target_m.log10().floor());
    let len_m = [1.0, 2.0, 5.0, 10.0]
        .into_iter()
        .map(|f| f * mag)
        .rfind(|&l| l <= target_m)
        .unwrap_or(mag);
    let bar = len_m * 1000.0 * k;
    if bar < tx0 - MARGIN - 10.0 {
        let (bx, by) = (MARGIN + 5.0, h - MARGIN - 10.0);
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
                outline: Some(thin.clone()),
            });
        }
        items.push(text(bx, by - 1.5, t * 0.8, "0".into()));
        let label = if len_m >= 1.0 {
            format!("{len_m} m")
        } else {
            format!("{} mm", len_m * 1000.0)
        };
        items.push(text(bx + bar - 2.0, by - 1.5, t * 0.8, label));
    }

    let info = PlanInfo {
        paper,
        portrait,
        cut_elements: cuts.len(),
        projection_lines: projection.len(),
        above_lines: above.len(),
    };
    let sheet = Sheet {
        width_mm: w,
        height_mm: h,
        title: format!("{} – Plan {} – 1:{}", o.project, st.name, o.scale),
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
            cut_above_mm: 1200.0,
            scale,
            paper,
            style: GraphicStyle::default(),
            project: "Demo".into(),
            date: "2026-11-05".into(),
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
        let (_, a) = plan(&scene, &opts(&first, 100, None)).unwrap();
        assert_eq!(a.paper, Paper::A4);
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
}
