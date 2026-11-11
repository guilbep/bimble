//! A point's coordinates as the status bar says them (ADR 0045): in the survey system (LV95
//! for Swiss models, the Swiss way: E 2'600'123.45), in WGS84 latitude and longitude, ETRS89 /
//! UTM, or another system of the table (LV03, US State Plane in feet or metres).

use bimble_core::georef::{self, Georef, LV95, Target};

/// `world_mm`: the point in world (model) millimetres; `target`: another system than the
/// model's own (Settings → Coordinates), when it can be said.
pub fn text(g: &Georef, world_mm: [f64; 3], target: Option<Target>) -> String {
    if let Some(t) = target {
        match g.in_system(world_mm, t) {
            Some(([lat, lon, h], _)) if t == Target::Wgs84 => {
                let ns = if lat >= 0.0 { "N" } else { "S" };
                let ew = if lon >= 0.0 { "E" } else { "W" };
                return format!(
                    "{:.6}° {ns}, {:.6}° {ew} · {} m (WGS84)",
                    lat.abs(),
                    lon.abs(),
                    swiss(h, 2)
                );
            }
            Some(([x, y, h], name)) => {
                let unit = match t {
                    Target::Epsg(n) => georef::system(n).map_or("m", |s| georef::unit_name(s.2)),
                    _ => "m",
                };
                return format!(
                    "E {} N {} · {} {unit} ({name})",
                    swiss(x, 2),
                    swiss(y, 2),
                    swiss(h, 2)
                );
            }
            // The model's system isn't known, or the point is outside that one: its own below.
            None => {}
        }
    }
    let s = g.survey(world_mm);
    let height = format!("{} m", swiss(s[2], 2));
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

impl crate::Viewer {
    /// Settings → Coordinates: the system the status bar says coordinates in.
    pub(crate) fn coordinates_ui(&mut self, ui: &mut eframe::egui::Ui) {
        use eframe::egui;
        ui.strong("Coordinates");
        let c = &mut self.settings.coordinates;
        for (id, label) in [
            (
                "survey",
                "The model's survey system (LV95 for Swiss models)",
            ),
            ("wgs84", "WGS84: latitude, longitude (GPS, worldwide)"),
            ("utm", "ETRS89 / UTM, the point's zone (Europe)"),
        ] {
            ui.radio_value(c, id.to_owned(), label);
        }
        let other = c.starts_with("epsg:");
        let chosen = c
            .strip_prefix("epsg:")
            .and_then(|n| n.parse().ok())
            .and_then(georef::system);
        ui.horizontal(|ui| {
            if ui
                .radio(other, "Another: ")
                .on_hover_text("Swiss LV03, a US State Plane zone (NAD83, feet or metres)…")
                .clicked()
                && !other
            {
                *c = "epsg:21781".into();
            }
            egui::ComboBox::from_id_salt("coordinate-system")
                .width(240.0)
                .selected_text(
                    chosen.map_or("Choose…".to_owned(), |s| format!("{} ({})", s.1, s.0)),
                )
                .show_ui(ui, |ui| {
                    ui.add(
                        egui::TextEdit::singleline(&mut self.coords_query)
                            .hint_text("Find: a state, \"ftUS\", a code…"),
                    );
                    let q = self.coords_query.to_lowercase();
                    egui::ScrollArea::vertical()
                        .max_height(260.0)
                        .show(ui, |ui| {
                            for (code, name, proj) in georef::systems()
                                .iter()
                                .filter(|s| s.0 != 2056)
                                .filter(|s| {
                                    q.split_whitespace().all(|w| {
                                        s.1.to_lowercase().contains(w)
                                            || s.0.to_string().contains(w)
                                    })
                                })
                            {
                                let label =
                                    format!("{name} · {} · EPSG:{code}", georef::unit_name(proj));
                                ui.selectable_value(c, format!("epsg:{code}"), label);
                            }
                        });
                });
        });
        ui.weak(
            "Shown in the status bar for the point under the pointer, worked out from the \
             model's survey system (pure-Rust PROJ). A model whose system isn't known shows its \
             own coordinates.",
        );
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
            text(&g, p, None),
            "E 2'600'123.45 N 1'200'000.00 · 432.10 m (LV95)"
        );
        assert!(
            text(&g, p, Some(Target::Wgs84)).starts_with("46.951"),
            "{}",
            text(&g, p, Some(Target::Wgs84))
        );
        // No survey system known: model metres; WGS84 can't be said.
        let none = Georef::default();
        assert_eq!(
            text(&none, [-1500.0, 20.0, 0.0], Some(Target::Wgs84)),
            "x -1.500 y 0.020 · 0.00 m"
        );
        assert!(
            text(&g, p, Some(Target::Utm)).ends_with("m (ETRS89 / UTM zone 32N)"),
            "{}",
            text(&g, p, Some(Target::Utm))
        );
        assert_eq!(swiss(999.0, 0), "999");
        assert_eq!(swiss(1000.0, 0), "1'000");
    }
}
