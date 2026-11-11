//! A point's coordinates as the status bar says them (ADR 0045): in the survey system (LV95
//! for Swiss models, the Swiss way: E 2'600'123.45), or in WGS84 latitude and longitude.

use bimble_core::georef::{Georef, LV95};

/// `world_mm`: the point in world (model) millimetres.
pub fn text(g: &Georef, world_mm: [f64; 3], wgs84: bool) -> String {
    let s = g.survey(world_mm);
    let height = format!("{} m", swiss(s[2], 2));
    if wgs84 && let Some([lat, lon]) = g.wgs84(world_mm) {
        let ns = if lat >= 0.0 { "N" } else { "S" };
        let ew = if lon >= 0.0 { "E" } else { "W" };
        return format!(
            "≈ {:.6}° {ns}, {:.6}° {ew} · {height}",
            lat.abs(),
            lon.abs()
        );
    }
    match g.crs.as_deref() {
        Some(LV95) => format!(
            "E {} N {} · {height} (LV95)",
            swiss(s[0], 2),
            swiss(s[1], 2)
        ),
        Some(crs) => format!(
            "E {} N {} · {height} ({crs})",
            swiss(s[0], 2),
            swiss(s[1], 2)
        ),
        None => format!("x {} y {} · {height}", swiss(s[0], 3), swiss(s[1], 3)),
    }
}

/// A number with `'` between thousands (Swiss), `decimals` after the point.
fn swiss(x: f64, decimals: usize) -> String {
    let t = format!("{:.*}", decimals, x.abs());
    let (int, frac) = t.split_once('.').unwrap_or((&t, ""));
    let mut out = String::new();
    for (i, c) in int.chars().enumerate() {
        if i > 0 && (int.len() - i) % 3 == 0 {
            out.push('\'');
        }
        out.push(c);
    }
    let sign = if x < 0.0 && t.chars().any(|c| c.is_ascii_digit() && c != '0') {
        "-"
    } else {
        ""
    };
    if frac.is_empty() {
        format!("{sign}{out}")
    } else {
        format!("{sign}{out}.{frac}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coordinates_the_swiss_way_or_in_wgs84() {
        let g = Georef::from_frame([1.0, 0.0], [2_600_000_000.0, 1_200_000_000.0, 0.0]);
        let p = [2_600_123_450.0, 1_200_000_000.0, 432_100.0];
        assert_eq!(
            text(&g, p, false),
            "E 2'600'123.45 N 1'200'000.00 · 432.10 m (LV95)"
        );
        assert!(
            text(&g, p, true).starts_with("≈ 46.951"),
            "{}",
            text(&g, p, true)
        );
        // No survey system known: model metres; WGS84 can't be said.
        let none = Georef::default();
        assert_eq!(
            text(&none, [-1500.0, 20.0, 0.0], true),
            "x -1.500 y 0.020 · 0.00 m"
        );
        assert_eq!(swiss(999.0, 0), "999");
        assert_eq!(swiss(1000.0, 0), "1'000");
    }
}
