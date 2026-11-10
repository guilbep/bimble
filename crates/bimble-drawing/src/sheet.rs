//! A sheet of paper with vector content, in mm, y pointing down (like SVG), written as SVG or
//! PDF. Plans (and later sections, rebar drawings) are built as a `Sheet`, so both formats get
//! the same drawing.

use bimble_core::style::{Rgb, SheetFont};
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
    /// Dash pattern, mm: dash, gap, dash, gap… (e.g. dash-dot: long dash, gap, dot, gap).
    /// Empty: continuous.
    pub dash: Vec<f64>,
}

/// Parallel hatch lines inside an area.
#[derive(Clone, Debug, PartialEq)]
pub struct Hatch {
    /// Line direction, degrees counter-clockwise from horizontal (on paper, y up).
    pub angle_deg: f64,
    pub spacing_mm: f64,
    /// Shift of the lines across their direction, mm.
    pub offset_mm: f64,
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
    /// Text on a baseline through `pos`; `anchor` says which point of the baseline `pos` is.
    Text {
        pos: [f64; 2],
        size_mm: f64,
        text: String,
        color: Rgb,
        anchor: Anchor,
        /// Baseline direction, degrees counter-clockwise from horizontal (on paper).
        angle_deg: f64,
        bold: bool,
    },
}

/// Which point of a text's baseline its position is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Anchor {
    #[default]
    Start,
    Middle,
    End,
}

impl Item {
    /// Horizontal, regular text starting at `pos`.
    pub fn text(pos: [f64; 2], size_mm: f64, text: impl Into<String>, color: Rgb) -> Item {
        Item::Text {
            pos,
            size_mm,
            text: text.into(),
            color,
            anchor: Anchor::Start,
            angle_deg: 0.0,
            bold: false,
        }
    }
}

/// Advance widths of Helvetica for ASCII 0x20..=0x7e, 1/1000 em (Adobe AFM).
const HELVETICA: [u16; 95] = [
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, // ' '../
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, // 0..9
    278, 278, 584, 584, 584, 556, 1015, // :..@
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, // A..M
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, // N..Z
    278, 278, 278, 469, 556, 333, // [..`
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, // a..m
    556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, // n..z
    334, 260, 334, 584, // {..~
];

/// Width of `text` in `font`, mm: Roboto's own advances; Helvetica's approximately (its AFM
/// widths for ASCII, bold ≈ 6 % wider).
pub fn text_width_mm(text: &str, size_mm: f64, bold: bool, font: SheetFont) -> f64 {
    if font == SheetFont::Roboto {
        return crate::fonts::Face::roboto(bold).width_em(text) * size_mm;
    }
    let units: u32 = text
        .chars()
        .map(|c| match c as u32 {
            v @ 0x20..=0x7e => HELVETICA[(v - 0x20) as usize] as u32,
            _ => 556,
        })
        .sum();
    units as f64 / 1000.0 * size_mm * if bold { 1.06 } else { 1.0 }
}

/// Where a text starts (left end of its baseline) and its baseline direction (y down).
fn text_start(
    pos: [f64; 2],
    size_mm: f64,
    text: &str,
    anchor: Anchor,
    angle_deg: f64,
    bold: bool,
    font: SheetFont,
) -> ([f64; 2], [f64; 2]) {
    let a = angle_deg.to_radians();
    let u = [a.cos(), -a.sin()];
    let back = match anchor {
        Anchor::Start => 0.0,
        Anchor::Middle => text_width_mm(text, size_mm, bold, font) / 2.0,
        Anchor::End => text_width_mm(text, size_mm, bold, font),
    };
    ([pos[0] - u[0] * back, pos[1] - u[1] * back], u)
}

#[derive(Clone, Debug, PartialEq)]
pub struct Sheet {
    pub width_mm: f64,
    pub height_mm: f64,
    pub title: String,
    pub items: Vec<Item>,
    /// The font of its text (the style's).
    pub font: SheetFont,
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
    if !p.dash.is_empty() {
        let d: Vec<String> = p.dash.iter().map(|x| format!("{x:.3}")).collect();
        let _ = write!(s, r#" stroke-dasharray="{}""#, d.join(" "));
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
    let off = if h.offset_mm.is_finite() {
        h.offset_mm
    } else {
        0.0
    };
    let first = ((n0 - off) / h.spacing_mm).ceil() as i64;
    let last = ((n1 - off) / h.spacing_mm).floor() as i64;
    if last - first > 100_000 {
        return Vec::new();
    }
    (first..=last)
        .map(|k| {
            let s = k as f64 * h.spacing_mm + off;
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
                    anchor,
                    angle_deg,
                    bold,
                } => {
                    // Positioned from the computed start so SVG and PDF place text alike.
                    let (p, _) =
                        text_start(*pos, *size_mm, text, *anchor, *angle_deg, *bold, self.font);
                    let rot = if *angle_deg != 0.0 {
                        format!(
                            r#" transform="rotate({:.3} {:.3} {:.3})""#,
                            -angle_deg, p[0], p[1]
                        )
                    } else {
                        String::new()
                    };
                    let weight = if *bold { r#" font-weight="bold""# } else { "" };
                    let family = match self.font {
                        SheetFont::Roboto => "Roboto, Helvetica, Arial, sans-serif",
                        SheetFont::Helvetica => "Helvetica, Arial, sans-serif",
                    };
                    let _ = writeln!(
                        s,
                        r#"<text x="{:.3}" y="{:.3}" font-family="{family}" font-size="{:.3}"{weight} fill="{}"{rot}>{}</text>"#,
                        p[0],
                        p[1],
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
        let (catalog, pages, page, content_id, font, info, bold_font) = (
            Ref::new(1),
            Ref::new(2),
            Ref::new(3),
            Ref::new(4),
            Ref::new(5),
            Ref::new(6),
            Ref::new(7),
        );
        let mut pdf = Pdf::new();
        pdf.catalog(catalog).pages(pages);
        pdf.pages(pages).kids([page]).count(1);
        {
            let mut p = pdf.page(page);
            p.media_box(Rect::new(0.0, 0.0, w as f32, h as f32))
                .parent(pages)
                .contents(content_id);
            p.resources()
                .fonts()
                .pair(Name(b"F1"), font)
                .pair(Name(b"F2"), bold_font);
            p.finish();
        }
        // Roboto: each weight embedded (ids from 8 on), its text as glyph ids.
        let mut next = 8;
        let mut used: [std::collections::BTreeMap<u16, char>; 2] = Default::default();
        if self.font == SheetFont::Roboto {
            for item in &self.items {
                if let Item::Text { text, bold, .. } = item {
                    let face = crate::fonts::Face::roboto(*bold);
                    for ch in text.chars() {
                        used[*bold as usize].entry(face.glyph(ch)).or_insert(ch);
                    }
                }
            }
            for (id, bold) in [(font, false), (bold_font, true)] {
                embed_font(
                    &mut pdf,
                    id,
                    crate::fonts::Face::roboto(bold),
                    &used[bold as usize],
                    &mut next,
                );
            }
        } else {
            pdf.type1_font(font)
                .base_font(Name(b"Helvetica"))
                .encoding_predefined(Name(b"WinAnsiEncoding"));
            pdf.type1_font(bold_font)
                .base_font(Name(b"Helvetica-Bold"))
                .encoding_predefined(Name(b"WinAnsiEncoding"));
        }
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
            c.set_dash_pattern(p.dash.iter().map(|&x| x as f32), 0.0);
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
                    anchor,
                    angle_deg,
                    bold,
                } => {
                    let [r, g, b] = rgb(*color);
                    c.set_fill_rgb(r, g, b);
                    c.begin_text();
                    c.set_font(Name(if *bold { b"F2" } else { b"F1" }), 1.0);
                    let (p, u) =
                        text_start(*pos, *size_mm, text, *anchor, *angle_deg, *bold, self.font);
                    // Glyph x along the baseline, glyph y "up" on paper (y is down here).
                    let s = *size_mm;
                    let m = [s * u[0], s * u[1], s * u[1], -s * u[0], p[0], p[1]];
                    c.set_text_matrix(m.map(|v| v as f32));
                    let bytes = match self.font {
                        SheetFont::Roboto => {
                            let face = crate::fonts::Face::roboto(*bold);
                            text.chars()
                                .flat_map(|ch| face.glyph(ch).to_be_bytes())
                                .collect()
                        }
                        SheetFont::Helvetica => win_ansi(text),
                    };
                    c.show(Str(&bytes));
                    c.end_text();
                }
            }
        }
        pdf.stream(content_id, &c.finish());
        pdf.finish()
    }
}

/// Embed `face` as font `id`: a Type 0 font over the whole file (compressed), its text as
/// 2-byte glyph ids, with the widths and the characters (for search and copy) of the glyphs
/// `used`. A face no text uses is still declared (the page names it), without its file.
fn embed_font(
    pdf: &mut pdf_writer::Pdf,
    id: pdf_writer::Ref,
    face: &crate::fonts::Face,
    used: &std::collections::BTreeMap<u16, char>,
    next: &mut i32,
) {
    use pdf_writer::types::{CidFontType, FontFlags, SystemInfo, UnicodeCmap};
    use pdf_writer::{Filter, Finish, Name, Rect, Ref, Str};
    let mut new = || {
        *next += 1;
        Ref::new(*next - 1)
    };
    let (cid, descriptor, cmap, file) = (new(), new(), new(), new());
    let name = Name(face.name.as_bytes());
    let info = SystemInfo {
        registry: Str(b"Adobe"),
        ordering: Str(b"Identity"),
        supplement: 0,
    };
    pdf.type0_font(id)
        .base_font(name)
        .encoding_predefined(Name(b"Identity-H"))
        .descendant_font(cid)
        .to_unicode(cmap);
    let mut c = pdf.cid_font(cid);
    c.subtype(CidFontType::Type2)
        .base_font(name)
        .system_info(info)
        .font_descriptor(descriptor)
        .default_width(0.0)
        .cid_to_gid_map_predefined(Name(b"Identity"));
    let advances = face.advances(used.keys().copied());
    let mut w = c.widths();
    for (g, a) in used.keys().zip(advances) {
        w.consecutive(*g, [a]);
    }
    w.finish();
    c.finish();
    let [x0, y0, x1, y1] = face.bbox;
    let mut d = pdf.font_descriptor(descriptor);
    d.name(name)
        .flags(FontFlags::NON_SYMBOLIC)
        .bbox(Rect::new(x0, y0, x1, y1))
        .italic_angle(0.0)
        .ascent(face.ascent)
        .descent(face.descent)
        .cap_height(face.cap_height)
        .stem_v(80.0);
    if !used.is_empty() {
        d.font_file2(file);
    }
    d.finish();
    let mut u = UnicodeCmap::new(Name(b"Bimble-UTF16"), info);
    for (g, ch) in used {
        u.pair(*g, *ch);
    }
    pdf.cmap(cmap, &u.finish());
    if !used.is_empty() {
        let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
        let _ = std::io::Write::write_all(&mut z, face.data);
        let packed = z.finish().unwrap_or_default();
        pdf.stream(file, &packed)
            .filter(Filter::FlateDecode)
            .pair(Name(b"Length1"), face.data.len() as i32);
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

/// A sheet rasterized to RGBA pixels (from its SVG, so it matches the exported drawing).
/// Fonts for drawing sheets, loaded once: the system's and Bimble's own (Roboto).
fn svg_options() -> resvg::usvg::Options<'static> {
    use resvg::usvg;
    use std::sync::{Arc, OnceLock};
    static DB: OnceLock<Arc<usvg::fontdb::Database>> = OnceLock::new();
    let db = DB.get_or_init(|| {
        let mut db = usvg::fontdb::Database::new();
        db.load_system_fonts();
        let none = db.is_empty();
        // Ours, so sheets in Roboto look the same everywhere; and the only font where there
        // are none (a browser, a bare container).
        db.load_font_data(crate::fonts::ROBOTO_REGULAR.to_vec());
        db.load_font_data(crate::fonts::ROBOTO_BOLD.to_vec());
        if none {
            db.set_sans_serif_family("Roboto");
        }
        Arc::new(db)
    });
    usvg::Options {
        fontdb: db.clone(),
        ..Default::default()
    }
}

/// A sheet ready to be drawn in pieces.
pub struct Prepared {
    tree: resvg::usvg::Tree,
    svg_px_per_mm: f64,
    pub width_mm: f64,
    pub height_mm: f64,
}

impl Prepared {
    /// A `w` × `h` pixel image of the sheet at `px_per_mm`, its top-left corner at `origin_mm`
    /// on the sheet. Around the paper: grey.
    pub fn render(
        &self,
        px_per_mm: f64,
        origin_mm: [f64; 2],
        w: u32,
        h: u32,
    ) -> Result<Raster, String> {
        use resvg::tiny_skia;
        if !(px_per_mm.is_finite() && px_per_mm > 0.0) {
            return Err("bad zoom".into());
        }
        if w as u64 * h as u64 > 200_000_000 {
            return Err(format!("{w} × {h} pixels is too large"));
        }
        let mut pixmap =
            tiny_skia::Pixmap::new(w.max(1), h.max(1)).ok_or("cannot allocate the image")?;
        pixmap.fill(tiny_skia::Color::from_rgba8(200, 200, 200, 255));
        let (x0, y0) = (
            (-origin_mm[0] * px_per_mm) as f32,
            (-origin_mm[1] * px_per_mm) as f32,
        );
        if let Some(r) = tiny_skia::Rect::from_xywh(
            x0,
            y0,
            (self.width_mm * px_per_mm) as f32,
            (self.height_mm * px_per_mm) as f32,
        ) {
            let mut paint = tiny_skia::Paint::default();
            paint.set_color(tiny_skia::Color::WHITE);
            pixmap.fill_rect(r, &paint, tiny_skia::Transform::identity(), None);
        }
        let s = (px_per_mm / self.svg_px_per_mm) as f32;
        resvg::render(
            &self.tree,
            tiny_skia::Transform::from_row(s, 0.0, 0.0, s, x0, y0),
            &mut pixmap.as_mut(),
        );
        // The background is opaque, so premultiplied and straight alpha are the same.
        Ok(Raster {
            width: w.max(1),
            height: h.max(1),
            rgba: pixmap.take(),
        })
    }
}

pub struct Raster {
    pub width: u32,
    pub height: u32,
    /// Straight (non-premultiplied) RGBA, row by row.
    pub rgba: Vec<u8>,
}

impl Sheet {
    /// Rasterize at `px_per_mm` (e.g. 3.94 for 100 dpi). Text uses the system fonts.
    pub fn rasterize(&self, px_per_mm: f64) -> Result<Raster, String> {
        let p = self.prepare()?;
        let (w, h) = (
            (self.width_mm * px_per_mm).round().max(1.0) as u32,
            (self.height_mm * px_per_mm).round().max(1.0) as u32,
        );
        p.render(px_per_mm, [0.0, 0.0], w, h)
    }

    /// The sheet parsed once, to draw parts of it at any zoom (the layout view, #34).
    pub fn prepare(&self) -> Result<Prepared, String> {
        use resvg::usvg;
        let opt = svg_options();
        let tree = usvg::Tree::from_str(&self.to_svg(), &opt).map_err(|e| e.to_string())?;
        // usvg sizes are in px at 96 dpi; the sheet's are mm.
        let svg_px_per_mm = tree.size().width() as f64 / self.width_mm;
        Ok(Prepared {
            tree,
            svg_px_per_mm,
            width_mm: self.width_mm,
            height_mm: self.height_mm,
        })
    }
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
            font: SheetFont::Helvetica,
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
                        offset_mm: 0.0,
                        pen: Pen {
                            width_mm: 0.13,
                            color: black,
                            dash: Vec::new(),
                        },
                    }],
                    outline: Some(Pen {
                        width_mm: 0.5,
                        color: black,
                        dash: Vec::new(),
                    }),
                },
                Item::Lines {
                    lines: vec![vec![[0.0, 100.0], [200.0, 100.0]]],
                    pen: Pen {
                        width_mm: 0.13,
                        color: black,
                        dash: vec![2.0, 1.0],
                    },
                },
                Item::text([10.0, 200.0], 3.5, "Plan N01 – 1:100", black),
                Item::Text {
                    pos: [100.0, 100.0],
                    size_mm: 2.5,
                    text: "362".into(),
                    color: black,
                    anchor: Anchor::Middle,
                    angle_deg: 90.0,
                    bold: true,
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
            offset_mm: 0.0,
            pen: Pen {
                width_mm: 0.1,
                color: Rgb([0, 0, 0]),
                dash: Vec::new(),
            },
        };
        // Vertical lines every 5 mm across 100 mm: x = 0, 5, ..., 100.
        let l = hatch_lines(&loops, &h);
        assert_eq!(l.len(), 21);
        for seg in &l {
            assert!((seg[0][0] - seg[1][0]).abs() < 1e-9, "vertical: {seg:?}");
        }
        // Shifted by 1 mm across: x = 1, 6, ..., 96.
        let l = hatch_lines(
            &loops,
            &Hatch {
                offset_mm: 1.0,
                ..h
            },
        );
        assert_eq!(l.len(), 20);
        assert!(
            l.iter()
                .all(|s| (s[0][0].rem_euclid(5.0) - 1.0).abs() < 1e-9)
        );
    }

    #[test]
    fn rotated_and_centred_text_is_placed_the_same_in_svg_and_pdf() {
        let s = sample().to_svg();
        // "362" is 3 × 556/1000 × 2.5 × 1.06 mm wide; centred and turned 90° it starts below pos.
        let w = text_width_mm("362", 2.5, true, SheetFont::Helvetica);
        assert!((w - 4.4202).abs() < 1e-9, "{w}");
        let y = 100.0 + w / 2.0;
        assert!(
            s.contains(&format!(
                r##"x="100.000" y="{y:.3}" font-family="Helvetica, Arial, sans-serif" font-size="2.500" font-weight="bold" fill="#000000" transform="rotate(-90.000 100.000 {y:.3})""##
            )),
            "{s}"
        );
        let pdf = String::from_utf8_lossy(&sample().to_pdf()).into_owned();
        assert!(pdf.contains("/Helvetica-Bold"));
    }

    /// In Roboto (the default, issue #53), text is measured with Roboto's widths, named in the
    /// SVG, and embedded in the PDF: a valid PDF whose text reads back, accents included.
    #[test]
    fn roboto_text_is_measured_named_and_embedded() {
        let mut sheet = Sheet {
            font: SheetFont::Roboto,
            ..sample()
        };
        let label = "Façade – Niveau 2, épaisseur 250";
        sheet
            .items
            .push(Item::text([20.0, 150.0], 3.5, label, Rgb([0, 0, 0])));
        let w = text_width_mm("362", 2.5, true, SheetFont::Roboto);
        let roboto = crate::fonts::Face::roboto(true).width_em("362") * 2.5;
        assert!(
            (w - roboto).abs() < 1e-9 && (w - 4.4202).abs() > 0.01,
            "{w}"
        );
        let y = 100.0 + w / 2.0;
        let svg = sheet.to_svg();
        assert!(
            svg.contains(&format!(
                r##"x="100.000" y="{y:.3}" font-family="Roboto, Helvetica, Arial, sans-serif""##
            )),
            "{svg}"
        );
        let pdf = sheet.to_pdf();
        let text = String::from_utf8_lossy(&pdf);
        assert!(text.contains("/Roboto-Bold") && text.contains("/Identity-H"));
        assert!(text.contains("/FontFile2") && !text.contains("/Helvetica"));
        // A full font file per weight used, compressed: well under the two files' 700 KB.
        assert!(pdf.len() < 500_000, "{} bytes", pdf.len());
        let doc = lopdf::Document::load_mem(&pdf).unwrap();
        let read = doc.extract_text(&[1]).unwrap();
        for item in &sheet.items {
            if let Item::Text { text, .. } = item {
                assert!(read.contains(text.as_str()), "{text:?} not in {read:?}");
            }
        }
    }

    #[test]
    fn non_latin_text_does_not_break_the_pdf() {
        assert_eq!(win_ansi("é – ✓"), vec![0xe9, b' ', 0x96, b' ', b'?']);
    }
}
