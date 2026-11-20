//! Graphic standards for the web panel (D5): pens per scale, hatch swatches drawn from the real
//! `HatchType`, families moved to another one. Read by the bridge query "standards".
#![cfg_attr(not(target_arch = "wasm32"), allow(dead_code))]

use bimble_core::MaterialCategory;
use bimble_core::hatches::HatchType;
use bimble_core::style::GraphicStyle;

/// Swatch size in px, and px per mm on paper.
const W: f64 = 48.0;
const H: f64 = 32.0;
const PX_PER_MM: f64 = 4.0;
/// Most lines per line family in one swatch (a tiny spacing must not make a huge string).
const MAX_LINES: i64 = 64;

fn hex(c: bimble_core::style::Rgb) -> String {
    let [r, g, b] = c.0;
    format!("#{r:02x}{g:02x}{b:02x}")
}

/// A 48×32 SVG swatch of a hatch type: its fill, then each line family across the box.
pub fn swatch_svg(t: &HatchType) -> String {
    let mut s = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{W}\" height=\"{H}\" viewBox=\"0 0 {W} {H}\"><rect width=\"{W}\" height=\"{H}\" fill=\"{}\"/>",
        t.fill.map(hex).unwrap_or_else(|| "#ffffff".into())
    );
    let ink = hex(t.color);
    for l in &t.lines {
        let step = l.spacing_mm as f64 * PX_PER_MM;
        if !(step.is_finite() && step > 0.0 && l.angle_deg.is_finite()) {
            continue;
        }
        let a = (l.angle_deg as f64).to_radians();
        // Along the line (SVG y down, angle counter-clockwise) and across it.
        let (dx, dy) = (a.cos(), -a.sin());
        let (nx, ny) = (-dy, dx);
        let off = l.offset_mm as f64 * PX_PER_MM;
        let across = [(0.0, 0.0), (W, 0.0), (0.0, H), (W, H)].map(|(x, y)| x * nx + y * ny);
        let lo = across.iter().copied().fold(f64::INFINITY, f64::min);
        let hi = across.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        let k0 = ((lo - off) / step).ceil() as i64;
        let k1 = (((hi - off) / step).floor() as i64).min(k0 + MAX_LINES - 1);
        let dash = if l.dash_mm.is_empty() {
            String::new()
        } else {
            let d: Vec<String> = l
                .dash_mm
                .iter()
                .map(|x| format!("{}", (*x as f64 * PX_PER_MM).max(0.1)))
                .collect();
            format!(" stroke-dasharray=\"{}\"", d.join(" "))
        };
        let r = W + H; // long enough to cross the box
        for k in k0..=k1 {
            let c = k as f64 * step + off;
            let (px, py) = (nx * c, ny * c);
            s += &format!(
                "<line x1=\"{:.2}\" y1=\"{:.2}\" x2=\"{:.2}\" y2=\"{:.2}\" stroke=\"{ink}\" stroke-width=\"0.75\"{dash}/>",
                px - dx * r,
                py - dy * r,
                px + dx * r,
                py + dy * r
            );
        }
    }
    s + "</svg>"
}

/// `{source, pens: {scales, rows: [{pen, mm}], roles: {cut, seen, thin}: pen|null}, hatches: [{family, cut, seen}], moved: [{from, to, count}]}`.
/// A family with no hatch type (it keeps its own on-screen pattern) has `cut`/`seen` null.
pub fn json(style: &GraphicStyle, source: Option<String>) -> serde_json::Value {
    let p = &style.paper.pens;
    let rows: Vec<_> = (1..=p.model.len() as u8)
        .map(|pen| {
            serde_json::json!({
                "pen": pen,
                "mm": p.scales.iter().map(|&s| p.model_mm(pen, s)).collect::<Vec<_>>(),
            })
        })
        .collect();
    // The pen of the cut, seen and hatch (thin) lines, for the reference's three columns.
    let pen = |name: &str| style.paper.line_type(name).map(|t| t.pen);
    let l = &style.paper.lines;
    let roles =
        serde_json::json!({ "cut": pen(&l.cut), "seen": pen(&l.seen), "thin": pen(&l.hatch) });
    let swatch = |c, cut| {
        style
            .paper
            .material_hatch(c, cut)
            .map(|t| serde_json::json!({ "name": t.name, "svg": swatch_svg(t) }))
    };
    let hatches: Vec<_> = MaterialCategory::ALL
        .iter()
        .map(|&c| serde_json::json!({ "family": c, "cut": swatch(c, true), "seen": swatch(c, false) }))
        .collect();
    let mut moved =
        std::collections::BTreeMap::<(MaterialCategory, MaterialCategory), usize>::new();
    for (name, &to) in &style.material_families {
        *moved
            .entry((MaterialCategory::classify(name), to))
            .or_default() += 1;
    }
    serde_json::json!({
        "source": source,
        "pens": { "scales": p.scales, "rows": rows, "roles": roles },
        "hatches": hatches,
        "moved": moved.into_iter().map(|((from, to), count)| serde_json::json!({ "from": from, "to": to, "count": count })).collect::<Vec<_>>(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn swatch_draws_the_hatch_lines() {
        let types = bimble_core::hatches::default_hatch_types();
        // "timber seen": horizontal lines 2 mm apart = 8 px; across 32 px: y = 0, 8, 16, 24, 32.
        let t = types.iter().find(|t| t.name == "timber seen").unwrap();
        let s = swatch_svg(t);
        assert_eq!(s.matches("<line").count(), 5, "{s}");
        assert!(s.contains("y1=\"8.00\" x2=\"80.00\" y2=\"8.00\""), "{s}");
        // "masonry cut": two families (45° every 1 mm, 135° every 3 mm) both drawn.
        let t = types.iter().find(|t| t.name == "masonry cut").unwrap();
        let n = swatch_svg(t).matches("<line").count();
        // 45°: across the box spans 0..56.6 px → k = 0..14 at 4 px (15 lines); 135°: -22.6..33.9 px
        // → -12, 0, 12, 24 at 12 px (4 lines).
        assert_eq!(n, 15 + 4);
        // A solid fill, no lines.
        let t = types.iter().find(|t| t.name == "steel cut").unwrap();
        let s = swatch_svg(t);
        assert!(s.contains("fill=\"#000000\"") && !s.contains("<line"));
    }

    #[test]
    fn pens_rows_are_the_pen_table() {
        let mut style = GraphicStyle::default();
        style
            .material_families
            .insert("Aluminium".into(), MaterialCategory::Steel);
        let j = json(&style, Some("office.json".into()));
        let p = &style.paper.pens;
        let rows = j["pens"]["rows"].as_array().unwrap();
        assert_eq!(rows.len(), p.model.len());
        for (i, r) in rows.iter().enumerate() {
            let want: Vec<f64> = p.model[i].iter().map(|&x| x as f64).collect();
            let got: Vec<f64> = r["mm"]
                .as_array()
                .unwrap()
                .iter()
                .map(|x| x.as_f64().unwrap())
                .collect();
            assert_eq!(got, want, "pen {}", i + 1);
        }
        assert_eq!(j["source"], "office.json");
        assert_eq!(j["pens"]["roles"]["cut"], 5, "black_continu_5");
        assert_eq!(
            j["hatches"].as_array().unwrap().len(),
            MaterialCategory::ALL.len()
        );
        assert_eq!(j["moved"][0]["to"], "steel");
        assert_eq!(j["moved"][0]["count"], 1);
    }
}
