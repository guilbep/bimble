//! Reading other offices' plans as reference geometry (ADR 0021): the lines of a DXF or of a
//! vector PDF page, in mm, as polylines. Text, hatches' fills and images are not read: a reference
//! is for checking that geometries match and for measuring.
//!
//! Files are untrusted: sizes, block nesting and point counts are bounded, and nothing panics.

use bimble_core::view::MAX_REFERENCE_POINTS;

pub type Polyline = Vec<[f64; 2]>;

/// What was read, and what to tell the user about it.
#[derive(Debug, Clone, PartialEq)]
pub struct Read {
    pub polylines: Vec<Polyline>,
    pub notes: Vec<String>,
}

const MAX_FILE_BYTES: usize = 256 * 1024 * 1024;
const MAX_DEPTH: usize = 8;
/// Segments per full circle when arcs and curves become lines.
const CIRCLE_SEGMENTS: f64 = 64.0;

/// A reference from a file's bytes, by its name's extension. `pdf_scale` is the plan's scale
/// (1:N) for PDFs: PDF sizes are on paper. DWG is refused with how to convert it.
pub fn read(name: &str, bytes: &[u8], pdf_scale: f64) -> Result<Read, String> {
    if bytes.len() > MAX_FILE_BYTES {
        return Err(format!("{name}: larger than 256 MB"));
    }
    let lower = name.to_ascii_lowercase();
    if lower.ends_with(".dxf") {
        read_dxf(bytes)
    } else if lower.ends_with(".pdf") {
        read_pdf(bytes, pdf_scale)
    } else if lower.ends_with(".dwg") {
        Err(
            "DWG can't be read directly (its libraries are closed or GPL). Convert it to DXF \
             first, for example with the free ODA File Converter or LibreDWG's dwg2dxf, then \
             add the DXF."
                .into(),
        )
    } else {
        Err(format!("{name}: a reference plan must be a .dxf or a .pdf"))
    }
}

struct Out {
    polylines: Vec<Polyline>,
    points: usize,
    truncated: bool,
}

impl Out {
    fn push(&mut self, p: Polyline) {
        if p.len() < 2 || p.iter().any(|q| !(q[0].is_finite() && q[1].is_finite())) {
            return;
        }
        if self.points + p.len() > MAX_REFERENCE_POINTS {
            self.truncated = true;
            return;
        }
        self.points += p.len();
        self.polylines.push(p);
    }
}

/// An affine 2D transform: x' = a x + c y + e, y' = b x + d y + f (PDF's order).
#[derive(Clone, Copy, Debug, PartialEq)]
struct M([f64; 6]);

impl M {
    fn apply(&self, p: [f64; 2]) -> [f64; 2] {
        let m = self.0;
        [
            m[0] * p[0] + m[2] * p[1] + m[4],
            m[1] * p[0] + m[3] * p[1] + m[5],
        ]
    }
    /// `self` applied after `inner`.
    fn after(&self, inner: &M) -> M {
        let (a, b) = (inner.0, self.0);
        M([
            a[0] * b[0] + a[1] * b[2],
            a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4],
            a[4] * b[1] + a[5] * b[3] + b[5],
        ])
    }
}

fn arc(c: [f64; 2], r: f64, a0: f64, a1: f64) -> Polyline {
    // Counter-clockwise from a0 to a1 (radians).
    let mut sweep = a1 - a0;
    while sweep <= 0.0 {
        sweep += std::f64::consts::TAU;
    }
    let n = ((sweep / std::f64::consts::TAU) * CIRCLE_SEGMENTS)
        .ceil()
        .max(2.0) as usize;
    (0..=n)
        .map(|i| {
            let a = a0 + sweep * i as f64 / n as f64;
            [c[0] + r * a.cos(), c[1] + r * a.sin()]
        })
        .collect()
}

// ---------------------------------------------------------------- DXF

/// DXF lines, arcs, circles, ellipses, polylines (with bulges) and splines (through their
/// control points), with block references expanded. Model space only. Units from $INSUNITS
/// (none: millimetres).
pub fn read_dxf(bytes: &[u8]) -> Result<Read, String> {
    use dxf::enums::Units;
    let mut cursor = std::io::Cursor::new(bytes);
    let d = dxf::Drawing::load(&mut cursor).map_err(|e| format!("not a readable DXF: {e}"))?;
    let (k, unit) = match d.header.default_drawing_units {
        Units::Millimeters => (1.0, "mm"),
        Units::Centimeters => (10.0, "cm"),
        Units::Decimeters => (100.0, "dm"),
        Units::Meters => (1000.0, "m"),
        Units::Inches => (25.4, "inches"),
        Units::Feet => (304.8, "feet"),
        Units::Unitless => (1.0, "none (read as mm)"),
        _ => (1.0, "unusual (read as mm)"),
    };
    let blocks: std::collections::HashMap<&str, &dxf::Block> =
        d.blocks().map(|b| (b.name.as_str(), b)).collect();
    let mut out = Out {
        polylines: Vec::new(),
        points: 0,
        truncated: false,
    };
    let scale = M([k, 0.0, 0.0, k, 0.0, 0.0]);
    let mut skipped = 0usize;
    for e in d.entities() {
        if e.common.is_in_paper_space {
            continue;
        }
        dxf_entity(e, &scale, &blocks, 0, &mut out, &mut skipped);
    }
    let mut notes = vec![format!("DXF units: {unit}")];
    if skipped > 0 {
        notes.push(format!(
            "{skipped} entities not drawn (text, hatches, 3D, nesting)"
        ));
    }
    if out.truncated {
        notes.push(format!("stopped at {MAX_REFERENCE_POINTS} points"));
    }
    Ok(Read {
        polylines: out.polylines,
        notes,
    })
}

fn dxf_entity(
    e: &dxf::entities::Entity,
    m: &M,
    blocks: &std::collections::HashMap<&str, &dxf::Block>,
    depth: usize,
    out: &mut Out,
    skipped: &mut usize,
) {
    use dxf::entities::EntityType as T;
    let p2 = |p: &dxf::Point| m.apply([p.x, p.y]);
    let mapped = |pl: Polyline| -> Polyline { pl.into_iter().map(|p| m.apply(p)).collect() };
    match &e.specific {
        T::Line(l) => out.push(vec![p2(&l.p1), p2(&l.p2)]),
        T::Circle(c) => out.push(mapped(arc(
            [c.center.x, c.center.y],
            c.radius,
            0.0,
            std::f64::consts::TAU,
        ))),
        T::Arc(a) => out.push(mapped(arc(
            [a.center.x, a.center.y],
            a.radius,
            a.start_angle.to_radians(),
            a.end_angle.to_radians(),
        ))),
        T::Ellipse(el) => {
            let (c, ax) = (
                [el.center.x, el.center.y],
                [el.major_axis.x, el.major_axis.y],
            );
            let r = el.minor_axis_ratio;
            let mut t1 = el.end_parameter;
            while t1 <= el.start_parameter {
                t1 += std::f64::consts::TAU;
            }
            let n = (((t1 - el.start_parameter) / std::f64::consts::TAU) * CIRCLE_SEGMENTS)
                .ceil()
                .max(2.0) as usize;
            let pl = (0..=n)
                .map(|i| {
                    let t = el.start_parameter + (t1 - el.start_parameter) * i as f64 / n as f64;
                    let (s, co) = t.sin_cos();
                    [
                        c[0] + ax[0] * co - ax[1] * r * s,
                        c[1] + ax[1] * co + ax[0] * r * s,
                    ]
                })
                .collect();
            out.push(mapped(pl));
        }
        T::LwPolyline(p) => {
            let v: Vec<([f64; 2], f64)> =
                p.vertices.iter().map(|v| ([v.x, v.y], v.bulge)).collect();
            out.push(mapped(bulged(&v, p.is_closed())));
        }
        T::Polyline(p) => {
            let v: Vec<([f64; 2], f64)> = p
                .vertices()
                .map(|v| ([v.location.x, v.location.y], v.bulge))
                .collect();
            out.push(mapped(bulged(&v, p.is_closed())));
        }
        T::Spline(s) => {
            let pts = if s.fit_points.len() >= 2 {
                &s.fit_points
            } else {
                &s.control_points
            };
            out.push(pts.iter().map(&p2).collect());
        }
        T::Solid(s) => out.push(
            [
                &s.first_corner,
                &s.second_corner,
                &s.fourth_corner,
                &s.third_corner,
                &s.first_corner,
            ]
            .iter()
            .map(|p| p2(p))
            .collect(),
        ),
        T::Insert(ins) => {
            let Some(b) = blocks.get(ins.name.as_str()) else {
                *skipped += 1;
                return;
            };
            if depth >= MAX_DEPTH {
                *skipped += 1;
                return;
            }
            let (s, c) = ins.rotation.to_radians().sin_cos();
            let (sx, sy) = (ins.x_scale_factor, ins.y_scale_factor);
            let base = [b.base_point.x, b.base_point.y];
            let cols = ins.column_count.clamp(1, 1000) as usize;
            let rows = ins.row_count.clamp(1, 1000) as usize;
            for row in 0..rows {
                for col in 0..cols {
                    // Block coordinates: minus the base point, scaled, rotated, at the location
                    // (plus the array step, in the rotated frame).
                    let dx = col as f64 * ins.column_spacing;
                    let dy = row as f64 * ins.row_spacing;
                    let local = M([
                        sx * c,
                        sx * s,
                        -sy * s,
                        sy * c,
                        ins.location.x + dx * c - dy * s - (base[0] * sx * c - base[1] * sy * s),
                        ins.location.y + dx * s + dy * c - (base[0] * sx * s + base[1] * sy * c),
                    ]);
                    let mm = m.after(&local);
                    for be in &b.entities {
                        dxf_entity(be, &mm, blocks, depth + 1, out, skipped);
                        if out.truncated {
                            return;
                        }
                    }
                }
            }
        }
        _ => *skipped += 1,
    }
}

/// A polyline whose segments may be arcs (DXF bulge: tan of a quarter of the arc's angle).
fn bulged(v: &[([f64; 2], f64)], closed: bool) -> Polyline {
    let mut out = Vec::new();
    let n = v.len();
    if n == 0 {
        return out;
    }
    let segs = if closed { n } else { n - 1 };
    out.push(v[0].0);
    for i in 0..segs {
        let (a, bulge) = v[i];
        let b = v[(i + 1) % n].0;
        if bulge.abs() < 1e-9 || !bulge.is_finite() {
            out.push(b);
            continue;
        }
        let theta = 4.0 * bulge.atan();
        let chord = ((b[0] - a[0]).powi(2) + (b[1] - a[1]).powi(2)).sqrt();
        if chord < 1e-12 {
            continue;
        }
        let r = chord / (2.0 * (theta / 2.0).sin());
        // Centre: from the chord's middle, perpendicular, at r·cos(θ/2).
        let mid = [(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0];
        let h = r * (theta / 2.0).cos();
        let nrm = [-(b[1] - a[1]) / chord, (b[0] - a[0]) / chord];
        let c = [mid[0] + nrm[0] * h, mid[1] + nrm[1] * h];
        let a0 = (a[1] - c[1]).atan2(a[0] - c[0]);
        let steps = ((theta.abs() / std::f64::consts::TAU) * CIRCLE_SEGMENTS)
            .ceil()
            .max(2.0) as usize;
        let rr = r.abs();
        for k in 1..=steps {
            let t = a0 + theta * k as f64 / steps as f64;
            out.push([c[0] + rr * t.cos(), c[1] + rr * t.sin()]);
        }
    }
    out
}

// ---------------------------------------------------------------- PDF

/// The paths of the first page of a vector PDF (stroked and filled outlines alike), in mm at the
/// given scale (1:N): a PDF point is 25.4/72 mm on paper, N times that in the model.
pub fn read_pdf(bytes: &[u8], scale: f64) -> Result<Read, String> {
    if !(scale.is_finite() && scale > 0.0 && scale <= 100_000.0) {
        return Err("the PDF's scale must be 1:N with N between 1 and 100000".into());
    }
    let doc = lopdf::Document::load_mem(bytes).map_err(|e| format!("not a readable PDF: {e}"))?;
    let pages = doc.get_pages();
    let (&n, &page) = pages.iter().next().ok_or("the PDF has no page")?;
    let content = doc
        .get_page_content_with_limit(page, MAX_FILE_BYTES)
        .map_err(|e| format!("page {n}: {e}"))?;
    let resources = doc.get_page_resources(page).ok().and_then(|(d, ids)| {
        d.cloned().or_else(|| {
            ids.first()
                .and_then(|id| doc.get_dictionary(*id).ok().cloned())
        })
    });
    let k = 25.4 / 72.0 * scale;
    let mut out = Out {
        polylines: Vec::new(),
        points: 0,
        truncated: false,
    };
    let mut skipped = 0usize;
    pdf_content(
        &doc,
        &content,
        resources.as_ref(),
        M([k, 0.0, 0.0, k, 0.0, 0.0]),
        0,
        &mut out,
        &mut skipped,
    );
    let mut notes = vec![format!(
        "PDF page {n} of {}, read at 1:{scale}",
        pages.len()
    )];
    if out.polylines.is_empty() {
        notes.push("no vector lines found (a scanned PDF has none)".into());
    }
    if skipped > 0 {
        notes.push(format!(
            "{skipped} drawings not read (too deeply nested or unreadable)"
        ));
    }
    if out.truncated {
        notes.push(format!("stopped at {MAX_REFERENCE_POINTS} points"));
    }
    Ok(Read {
        polylines: out.polylines,
        notes,
    })
}

fn num(o: &lopdf::Object) -> Option<f64> {
    match o {
        lopdf::Object::Integer(i) => Some(*i as f64),
        lopdf::Object::Real(r) => Some(*r as f64),
        _ => None,
    }
}

fn pdf_content(
    doc: &lopdf::Document,
    content: &[u8],
    resources: Option<&lopdf::Dictionary>,
    base: M,
    depth: usize,
    out: &mut Out,
    skipped: &mut usize,
) {
    let Ok(ops) = lopdf::content::Content::decode(content) else {
        *skipped += 1;
        return;
    };
    let mut ctm = base;
    let mut stack: Vec<M> = Vec::new();
    let mut path: Vec<Polyline> = Vec::new();
    let mut cur: Polyline = Vec::new();
    let mut last = [0.0f64; 2];
    let mut start = [0.0f64; 2];
    let flush = |cur: &mut Polyline, path: &mut Vec<Polyline>| {
        if cur.len() >= 2 {
            path.push(std::mem::take(cur));
        } else {
            cur.clear();
        }
    };
    for op in ops.operations {
        let a: Vec<f64> = op.operands.iter().filter_map(num).collect();
        match op.operator.as_str() {
            "q" => {
                if stack.len() < 64 {
                    stack.push(ctm);
                }
            }
            "Q" => ctm = stack.pop().unwrap_or(base),
            "cm" if a.len() == 6 => {
                ctm = ctm.after(&M([a[0], a[1], a[2], a[3], a[4], a[5]]));
            }
            "m" if a.len() == 2 => {
                flush(&mut cur, &mut path);
                last = [a[0], a[1]];
                start = last;
                cur.push(ctm.apply(last));
            }
            "l" if a.len() == 2 => {
                if cur.is_empty() {
                    cur.push(ctm.apply(last));
                }
                last = [a[0], a[1]];
                cur.push(ctm.apply(last));
            }
            "c" | "v" | "y" => {
                let (p1, p2, p3) = match (op.operator.as_str(), a.len()) {
                    ("c", 6) => ([a[0], a[1]], [a[2], a[3]], [a[4], a[5]]),
                    ("v", 4) => (last, [a[0], a[1]], [a[2], a[3]]),
                    ("y", 4) => ([a[0], a[1]], [a[2], a[3]], [a[2], a[3]]),
                    _ => continue,
                };
                if cur.is_empty() {
                    cur.push(ctm.apply(last));
                }
                let p0 = last;
                for i in 1..=12 {
                    let t = i as f64 / 12.0;
                    let u = 1.0 - t;
                    let w = [u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t];
                    let p = [
                        w[0] * p0[0] + w[1] * p1[0] + w[2] * p2[0] + w[3] * p3[0],
                        w[0] * p0[1] + w[1] * p1[1] + w[2] * p2[1] + w[3] * p3[1],
                    ];
                    cur.push(ctm.apply(p));
                }
                last = p3;
            }
            "h" => {
                if !cur.is_empty() {
                    cur.push(ctm.apply(start));
                    last = start;
                }
            }
            "re" if a.len() == 4 => {
                flush(&mut cur, &mut path);
                let (x, y, w, h) = (a[0], a[1], a[2], a[3]);
                path.push(
                    [[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]]
                        .map(|p| ctm.apply(p))
                        .to_vec(),
                );
                last = [x, y];
                start = last;
            }
            // Painting ends the path: stroked or filled, its outline is a line of the plan.
            "S" | "s" | "f" | "F" | "f*" | "B" | "B*" | "b" | "b*" => {
                if matches!(op.operator.as_str(), "s" | "b" | "b*") && !cur.is_empty() {
                    cur.push(ctm.apply(start));
                }
                flush(&mut cur, &mut path);
                for p in path.drain(..) {
                    out.push(p);
                    if out.truncated {
                        return;
                    }
                }
            }
            // The path was only a clip: not drawn.
            "n" => {
                cur.clear();
                path.clear();
            }
            "Do" => {
                let Some(name) = op.operands.first().and_then(|o| o.as_name().ok()) else {
                    continue;
                };
                if depth >= MAX_DEPTH {
                    *skipped += 1;
                    continue;
                }
                let xobj = resources
                    .and_then(|r| r.get_deref(b"XObject", doc).ok())
                    .and_then(|x| x.as_dict().ok())
                    .and_then(|x| x.get_deref(name, doc).ok())
                    .and_then(|x| x.as_stream().ok());
                let Some(stream) = xobj else { continue };
                let is_form = stream
                    .dict
                    .get(b"Subtype")
                    .ok()
                    .and_then(|s| s.as_name().ok())
                    == Some(b"Form".as_slice());
                if !is_form {
                    continue;
                }
                let matrix: Vec<f64> = stream
                    .dict
                    .get(b"Matrix")
                    .ok()
                    .and_then(|m| m.as_array().ok())
                    .map(|m| m.iter().filter_map(num).collect())
                    .unwrap_or_default();
                let m = if matrix.len() == 6 {
                    ctm.after(&M([
                        matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
                    ]))
                } else {
                    ctm
                };
                let inner_res = stream
                    .dict
                    .get_deref(b"Resources", doc)
                    .ok()
                    .and_then(|r| r.as_dict().ok())
                    .or(resources);
                match stream.decompressed_content_with_limit(MAX_FILE_BYTES) {
                    Ok(c) => pdf_content(doc, &c, inner_res, m, depth + 1, out, skipped),
                    Err(_) => {
                        // An uncompressed stream has its bytes as they are.
                        if stream.dict.get(b"Filter").is_err() {
                            pdf_content(doc, &stream.content, inner_res, m, depth + 1, out, skipped)
                        } else {
                            *skipped += 1;
                        }
                    }
                }
                if out.truncated {
                    return;
                }
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn near(a: [f64; 2], b: [f64; 2]) -> bool {
        (a[0] - b[0]).abs() < 1e-6 && (a[1] - b[1]).abs() < 1e-6
    }

    #[test]
    fn dxf_lines_arcs_polylines_and_blocks_in_mm() {
        use dxf::entities::*;
        use dxf::{Block, Drawing, Point};
        let mut d = Drawing::new();
        // LWPOLYLINE and $INSUNITS need a version newer than the default R12.
        d.header.version = dxf::enums::AcadVersion::R2018;
        d.header.default_drawing_units = dxf::enums::Units::Meters;
        d.add_entity(Entity::new(EntityType::Line(Line::new(
            Point::new(0.0, 0.0, 0.0),
            Point::new(2.0, 0.0, 0.0),
        ))));
        let mut lw = LwPolyline::default();
        for (x, y, bulge) in [(0.0, 0.0, 0.0), (1.0, 0.0, 1.0), (1.0, 1.0, 0.0)] {
            lw.vertices.push(dxf::LwPolylineVertex {
                x,
                y,
                bulge,
                ..Default::default()
            });
        }
        d.add_entity(Entity::new(EntityType::LwPolyline(lw)));
        let mut block = Block {
            name: "COL".into(),
            ..Default::default()
        };
        block.entities.push(Entity::new(EntityType::Line(Line::new(
            Point::new(0.0, 0.0, 0.0),
            Point::new(0.3, 0.0, 0.0),
        ))));
        d.add_block(block);
        let ins = Insert {
            name: "COL".into(),
            location: Point::new(5.0, 5.0, 0.0),
            rotation: 90.0,
            ..Default::default()
        };
        d.add_entity(Entity::new(EntityType::Insert(ins)));
        let mut text = Vec::new();
        d.save(&mut text).unwrap();

        let r = read("plan.DXF", &text, 100.0).unwrap();
        assert!(r.notes[0].contains("m"), "{:?}", r.notes);
        assert_eq!(r.polylines.len(), 3, "{:?}", r.polylines);
        // Metres → mm.
        assert!(near(r.polylines[0][1], [2000.0, 0.0]));
        // The bulge 1 makes a half circle from (1000,0) to (1000,1000), bulging east.
        let arc = &r.polylines[1];
        assert!(near(*arc.last().unwrap(), [1000.0, 1000.0]));
        let east = arc.iter().map(|p| p[0]).fold(f64::MIN, f64::max);
        assert!((east - 1500.0).abs() < 5.0, "{east}");
        // The block's 300 mm line, turned 90° at (5 m, 5 m).
        let b = &r.polylines[2];
        assert!(
            near(b[0], [5000.0, 5000.0]) && near(b[1], [5000.0, 5300.0]),
            "{b:?}"
        );
    }

    /// A minimal PDF with one page: a 100 × 50 pt rectangle and a stroked line through cm.
    fn pdf_bytes() -> Vec<u8> {
        let content = b"q 1 0 0 1 10 20 cm 0 0 100 50 re S Q 0 0 m 72 0 l S";
        let mut objects = vec![
            "<< /Type /Catalog /Pages 2 0 R >>".to_string(),
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>".to_string(),
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>".to_string(),
        ];
        objects.push(format!(
            "<< /Length {} >>\nstream\n{}\nendstream",
            content.len(),
            String::from_utf8_lossy(content)
        ));
        let mut pdf = b"%PDF-1.4\n".to_vec();
        let mut offsets = Vec::new();
        for (i, o) in objects.iter().enumerate() {
            offsets.push(pdf.len());
            pdf.extend(format!("{} 0 obj\n{o}\nendobj\n", i + 1).as_bytes());
        }
        let xref = pdf.len();
        pdf.extend(format!("xref\n0 {}\n0000000000 65535 f \n", objects.len() + 1).as_bytes());
        for off in offsets {
            pdf.extend(format!("{off:010} 00000 n \n").as_bytes());
        }
        pdf.extend(
            format!(
                "trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n",
                objects.len() + 1
            )
            .as_bytes(),
        );
        pdf
    }

    #[test]
    fn pdf_paths_at_scale() {
        let r = read("a.pdf", &pdf_bytes(), 100.0).unwrap();
        assert_eq!(r.polylines.len(), 2, "{:?} {:?}", r.polylines, r.notes);
        let k = 25.4 / 72.0 * 100.0;
        // The rectangle, moved by cm (10, 20) pt.
        assert!(near(r.polylines[0][0], [10.0 * k, 20.0 * k]));
        assert!(near(r.polylines[0][2], [110.0 * k, 70.0 * k]));
        // 72 pt = 1 inch = 25.4 mm on paper = 2540 mm at 1:100.
        assert!(
            near(r.polylines[1][1], [2540.0, 0.0]),
            "{:?}",
            r.polylines[1]
        );
    }

    #[test]
    fn bad_input_is_an_error_not_a_panic() {
        assert!(read("a.dwg", b"AC1032", 100.0).unwrap_err().contains("DXF"));
        assert!(read("a.dxf", b"not a dxf at all", 100.0).is_err());
        assert!(read("a.pdf", b"%PDF-1.4 garbage", 100.0).is_err());
        assert!(read("a.pdf", &pdf_bytes(), 0.0).is_err());
        assert!(read("a.txt", b"", 100.0).is_err());
        // A block that inserts itself stops at the nesting limit.
        use dxf::entities::*;
        let mut d = dxf::Drawing::new();
        let mut b = dxf::Block {
            name: "LOOP".into(),
            ..Default::default()
        };
        b.entities.push(Entity::new(EntityType::Line(Line::new(
            dxf::Point::new(0.0, 0.0, 0.0),
            dxf::Point::new(1.0, 0.0, 0.0),
        ))));
        let ins = Insert {
            name: "LOOP".into(),
            ..Default::default()
        };
        b.entities
            .push(Entity::new(EntityType::Insert(ins.clone())));
        d.add_block(b);
        d.add_entity(Entity::new(EntityType::Insert(ins)));
        let mut text = Vec::new();
        d.save(&mut text).unwrap();
        let r = read("loop.dxf", &text, 1.0).unwrap();
        assert_eq!(r.polylines.len(), super::MAX_DEPTH);
    }
}
