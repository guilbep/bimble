//! A sheet of paper with vector content, in mm, y pointing down (like SVG), written as SVG or
//! PDF. Plans (and later sections, rebar drawings) are built as a `Sheet`, so both formats get
//! the same drawing.

use bimble_core::style::Rgb;
use std::fmt::Write;

/// ISO 216 sizes, landscape (width ≥ height), mm.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Paper {
    A4,
    A3,
    A2,
    A1,
    A0,
}

impl Paper {
    pub const ALL: [Paper; 5] = [Paper::A4, Paper::A3, Paper::A2, Paper::A1, Paper::A0];

    /// Landscape size, mm.
    pub fn size(self) -> (f64, f64) {
        match self {
            Paper::A4 => (297.0, 210.0),
            Paper::A3 => (420.0, 297.0),
            Paper::A2 => (594.0, 420.0),
            Paper::A1 => (841.0, 594.0),
            Paper::A0 => (1189.0, 841.0),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Paper::A4 => "A4",
            Paper::A3 => "A3",
            Paper::A2 => "A2",
            Paper::A1 => "A1",
            Paper::A0 => "A0",
        }
    }

    pub fn parse(s: &str) -> Option<Paper> {
        Paper::ALL
            .into_iter()
            .find(|p| p.name().eq_ignore_ascii_case(s.trim()))
    }
}

/// A line style on paper.
#[derive(Clone, Debug, PartialEq)]
pub struct Pen {
    pub width_mm: f64,
    pub color: Rgb,
    /// Dash and gap lengths, mm. None: continuous.
    pub dash: Option<[f64; 2]>,
}

/// Parallel hatch lines inside an area.
#[derive(Clone, Debug, PartialEq)]
pub struct Hatch {
    /// Line direction, degrees counter-clockwise from horizontal (on paper, y up).
    pub angle_deg: f64,
    pub spacing_mm: f64,
    pub pen: Pen,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Item {
    /// Closed loops filled with the even-odd rule (holes work), then hatched and outlined.
    Area {
        loops: Vec<Vec<[f64; 2]>>,
        fill: Option<Rgb>,
        hatches: Vec<Hatch>,
        outline: Option<Pen>,
    },
    /// Open polylines.
    Lines { lines: Vec<Vec<[f64; 2]>>, pen: Pen },
    /// Text; `pos` is the left end of the baseline.
    Text {
        pos: [f64; 2],
        size_mm: f64,
        text: String,
        color: Rgb,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Sheet {
    pub width_mm: f64,
    pub height_mm: f64,
    pub title: String,
    pub items: Vec<Item>,
}

fn hex(c: Rgb) -> String {
    c.to_hex()
}

fn svg_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn path_d(loops: &[Vec<[f64; 2]>], close: bool) -> String {
    let mut d = String::new();
    for l in loops {
        for (k, p) in l.iter().enumerate() {
            let _ = write!(
                d,
                "{}{:.3} {:.3}",
                if k == 0 { "M" } else { "L" },
                p[0],
                p[1]
            );
        }
        if close {
            d.push('Z');
        }
    }
    d
}

fn pen_attrs(p: &Pen) -> String {
    let mut s = format!(
        r#"stroke="{}" stroke-width="{:.3}" stroke-linejoin="round" stroke-linecap="butt""#,
        hex(p.color),
        p.width_mm
    );
    if let Some([a, b]) = p.dash {
        let _ = write!(s, r#" stroke-dasharray="{a:.3} {b:.3}""#);
    }
    s
}

/// Hatch line segments covering the bounding box of `loops` (to be clipped by them).
fn hatch_lines(loops: &[Vec<[f64; 2]>], h: &Hatch) -> Vec<[[f64; 2]; 2]> {
    let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
    for p in loops.iter().flatten() {
        for k in 0..2 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    // No points (or NaN bounds), or no spacing: nothing to hatch.
    if lo[0].partial_cmp(&hi[0]) != Some(std::cmp::Ordering::Less) && lo[0] != hi[0]
        || h.spacing_mm <= 0.0
    {
        return Vec::new();
    }
    // Paper y points down: an angle counter-clockwise on paper is clockwise in these coordinates.
    let a = h.angle_deg.to_radians();
    let d = [a.cos(), -a.sin()];
    let n = [-d[1], d[0]];
    let corners = [lo, [hi[0], lo[1]], hi, [lo[0], hi[1]]];
    let proj = |p: [f64; 2], v: [f64; 2]| p[0] * v[0] + p[1] * v[1];
    let (mut n0, mut n1, mut d0, mut d1) = (
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
    );
    for c in corners {
        n0 = n0.min(proj(c, n));
        n1 = n1.max(proj(c, n));
        d0 = d0.min(proj(c, d));
        d1 = d1.max(proj(c, d));
    }
    // Lines at multiples of the spacing (aligned across areas, like a printed pattern).
    let first = (n0 / h.spacing_mm).ceil() as i64;
    let last = (n1 / h.spacing_mm).floor() as i64;
    if last - first > 100_000 {
        return Vec::new();
    }
    (first..=last)
        .map(|k| {
            let s = k as f64 * h.spacing_mm;
            let at = |t: f64| [n[0] * s + d[0] * t, n[1] * s + d[1] * t];
            [at(d0), at(d1)]
        })
        .collect()
}

impl Sheet {
    pub fn to_svg(&self) -> String {
        let (w, h) = (self.width_mm, self.height_mm);
        let mut s = String::new();
        let _ = writeln!(
            s,
            r##"<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="{w}mm" height="{h}mm" viewBox="0 0 {w} {h}">
<title>{}</title>
<rect x="0" y="0" width="{w}" height="{h}" fill="#ffffff"/>"##,
            svg_escape(&self.title)
        );
        let mut clip_id = 0;
        for item in &self.items {
            match item {
                Item::Area {
                    loops,
                    fill,
                    hatches,
                    outline,
                } => {
                    let d = path_d(loops, true);
                    if let Some(f) = fill {
                        let _ = writeln!(
                            s,
                            r#"<path d="{d}" fill="{}" fill-rule="evenodd"/>"#,
                            hex(*f)
                        );
                    }
                    if !hatches.is_empty() {
                        clip_id += 1;
                        let _ = writeln!(
                            s,
                            r#"<clipPath id="c{clip_id}"><path d="{d}" clip-rule="evenodd"/></clipPath><g clip-path="url(#c{clip_id})">"#
                        );
                        for hatch in hatches {
                            let lines: Vec<Vec<[f64; 2]>> = hatch_lines(loops, hatch)
                                .iter()
                                .map(|l| l.to_vec())
                                .collect();
                            let _ = writeln!(
                                s,
                                r#"<path d="{}" fill="none" {}/>"#,
                                path_d(&lines, false),
                                pen_attrs(&hatch.pen)
                            );
                        }
                        s.push_str("</g>\n");
                    }
                    if let Some(p) = outline {
                        let _ = writeln!(s, r#"<path d="{d}" fill="none" {}/>"#, pen_attrs(p));
                    }
                }
                Item::Lines { lines, pen } => {
                    let _ = writeln!(
                        s,
                        r#"<path d="{}" fill="none" {}/>"#,
                        path_d(lines, false),
                        pen_attrs(pen)
                    );
                }
                Item::Text {
                    pos,
                    size_mm,
                    text,
                    color,
                } => {
                    let _ = writeln!(
                        s,
                        r#"<text x="{:.3}" y="{:.3}" font-family="Helvetica, Arial, sans-serif" font-size="{:.3}" fill="{}">{}</text>"#,
                        pos[0],
                        pos[1],
                        size_mm,
                        hex(*color),
                        svg_escape(text)
                    );
                }
            }
        }
        s.push_str("</svg>\n");
        s
    }

    pub fn to_pdf(&self) -> Vec<u8> {
        use pdf_writer::{Content, Finish, Name, Pdf, Rect, Ref, Str};
        const PT: f64 = 72.0 / 25.4;
        let (w, h) = (self.width_mm * PT, self.height_mm * PT);
        let (catalog, pages, page, content_id, font, info) = (
            Ref::new(1),
            Ref::new(2),
            Ref::new(3),
            Ref::new(4),
            Ref::new(5),
            Ref::new(6),
        );
        let mut pdf = Pdf::new();
        pdf.catalog(catalog).pages(pages);
        pdf.pages(pages).kids([page]).count(1);
        {
            let mut p = pdf.page(page);
            p.media_box(Rect::new(0.0, 0.0, w as f32, h as f32))
                .parent(pages)
                .contents(content_id);
            p.resources().fonts().pair(Name(b"F1"), font);
            p.finish();
        }
        pdf.type1_font(font)
            .base_font(Name(b"Helvetica"))
            .encoding_predefined(Name(b"WinAnsiEncoding"));
        pdf.document_info(info)
            .title(pdf_writer::TextStr(&self.title))
            .producer(pdf_writer::TextStr(concat!(
                "Bimble ",
                env!("CARGO_PKG_VERSION")
            )));

        let mut c = Content::new();
        // Work in mm with y down, like the sheet: scale to points and flip.
        c.transform([PT as f32, 0.0, 0.0, -PT as f32, 0.0, h as f32]);
        let rgb = |c: Rgb| c.unit();
        let path = |c: &mut Content, loops: &[Vec<[f64; 2]>], close: bool| {
            for l in loops {
                for (k, p) in l.iter().enumerate() {
                    if k == 0 {
                        c.move_to(p[0] as f32, p[1] as f32);
                    } else {
                        c.line_to(p[0] as f32, p[1] as f32);
                    }
                }
                if close {
                    c.close_path();
                }
            }
        };
        let set_pen = |c: &mut Content, p: &Pen| {
            let [r, g, b] = rgb(p.color);
            c.set_stroke_rgb(r, g, b);
            c.set_line_width(p.width_mm as f32);
            match p.dash {
                Some([a, b]) => c.set_dash_pattern([a as f32, b as f32], 0.0),
                None => c.set_dash_pattern([], 0.0),
            };
        };
        for item in &self.items {
            match item {
                Item::Area {
                    loops,
                    fill,
                    hatches,
                    outline,
                } => {
                    if let Some(f) = fill {
                        let [r, g, b] = rgb(*f);
                        c.set_fill_rgb(r, g, b);
                        path(&mut c, loops, true);
                        c.fill_even_odd();
                    }
                    if !hatches.is_empty() {
                        c.save_state();
                        path(&mut c, loops, true);
                        c.clip_even_odd();
                        c.end_path();
                        for hatch in hatches {
                            set_pen(&mut c, &hatch.pen);
                            for [a, b] in hatch_lines(loops, hatch) {
                                c.move_to(a[0] as f32, a[1] as f32);
                                c.line_to(b[0] as f32, b[1] as f32);
                            }
                            c.stroke();
                        }
                        c.restore_state();
                    }
                    if let Some(p) = outline {
                        set_pen(&mut c, p);
                        path(&mut c, loops, true);
                        c.stroke();
                    }
                }
                Item::Lines { lines, pen } => {
                    set_pen(&mut c, pen);
                    path(&mut c, lines, false);
                    c.stroke();
                }
                Item::Text {
                    pos,
                    size_mm,
                    text,
                    color,
                } => {
                    let [r, g, b] = rgb(*color);
                    c.set_fill_rgb(r, g, b);
                    c.begin_text();
                    c.set_font(Name(b"F1"), 1.0);
                    // Undo the y flip for the glyphs.
                    let s = *size_mm as f32;
                    c.set_text_matrix([s, 0.0, 0.0, -s, pos[0] as f32, pos[1] as f32]);
                    c.show(Str(&win_ansi(text)));
                    c.end_text();
                }
            }
        }
        pdf.stream(content_id, &c.finish());
        pdf.finish()
    }
}

/// Text for the PDF's built-in Helvetica (WinAnsi): Latin-1 characters as is, others as '?'.
fn win_ansi(s: &str) -> Vec<u8> {
    s.chars()
        .map(|ch| match ch as u32 {
            0x20..=0x7e | 0xa0..=0xff => ch as u32 as u8,
            0x2013 => 0x96, // en dash
            0x2014 => 0x97, // em dash
            0x2019 => 0x92, // right single quote
            _ => b'?',
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Sheet {
        let black = Rgb([0, 0, 0]);
        Sheet {
            width_mm: 297.0,
            height_mm: 210.0,
            title: "Test <sheet> é".into(),
            items: vec![
                Item::Area {
                    loops: vec![
                        vec![[10.0, 10.0], [60.0, 10.0], [60.0, 60.0], [10.0, 60.0]],
                        vec![[20.0, 20.0], [50.0, 20.0], [50.0, 50.0], [20.0, 50.0]],
                    ],
                    fill: Some(Rgb([214, 214, 210])),
                    hatches: vec![Hatch {
                        angle_deg: 45.0,
                        spacing_mm: 2.0,
                        pen: Pen {
                            width_mm: 0.13,
                            color: black,
                            dash: None,
                        },
                    }],
                    outline: Some(Pen {
                        width_mm: 0.5,
                        color: black,
                        dash: None,
                    }),
                },
                Item::Lines {
                    lines: vec![vec![[0.0, 100.0], [200.0, 100.0]]],
                    pen: Pen {
                        width_mm: 0.13,
                        color: black,
                        dash: Some([2.0, 1.0]),
                    },
                },
                Item::Text {
                    pos: [10.0, 200.0],
                    size_mm: 3.5,
                    text: "Plan N01 – 1:100".into(),
                    color: black,
                },
            ],
        }
    }

    #[test]
    fn svg_has_paper_size_holes_hatches_and_escaped_text() {
        let s = sample().to_svg();
        assert!(s.contains(r#"width="297mm" height="210mm" viewBox="0 0 297 210""#));
        assert!(s.contains(r#"fill-rule="evenodd""#));
        assert!(s.contains("clipPath"));
        assert!(s.contains(r#"stroke-dasharray="2.000 1.000""#));
        assert!(s.contains("Test &lt;sheet&gt; é"));
        assert!(s.contains("Plan N01 – 1:100"));
    }

    #[test]
    fn pdf_is_a_one_page_pdf_of_the_paper_size() {
        let b = sample().to_pdf();
        assert!(b.starts_with(b"%PDF-"));
        let text = String::from_utf8_lossy(&b);
        // 297 × 210 mm in points.
        assert!(
            text.contains("/MediaBox [0 0 841.8898 595.2756]"),
            "{}",
            &text[..text.len().min(600)]
        );
        assert!(text.contains("/Count 1"));
        assert!(b.ends_with(b"%%EOF") || b.ends_with(b"%%EOF\n"));
    }

    #[test]
    fn hatch_lines_cover_the_area_at_the_spacing() {
        let loops = vec![vec![[0.0, 0.0], [100.0, 0.0], [100.0, 10.0], [0.0, 10.0]]];
        let h = Hatch {
            angle_deg: 90.0,
            spacing_mm: 5.0,
            pen: Pen {
                width_mm: 0.1,
                color: Rgb([0, 0, 0]),
                dash: None,
            },
        };
        // Vertical lines every 5 mm across 100 mm: x = 0, 5, ..., 100.
        let l = hatch_lines(&loops, &h);
        assert_eq!(l.len(), 21);
        for seg in &l {
            assert!((seg[0][0] - seg[1][0]).abs() < 1e-9, "vertical: {seg:?}");
        }
    }

    #[test]
    fn non_latin_text_does_not_break_the_pdf() {
        assert_eq!(win_ansi("é – ✓"), vec![0xe9, b' ', 0x96, b' ', b'?']);
    }
}
