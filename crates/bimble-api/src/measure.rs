//! Measurements as the viewers list them (the egui Measurements panel, the bridge's `measurements`
//! query): the length text and the list's JSON. Here, below the viewer, so the C core's golden
//! (`ccore_golden`, `ccore/golden/measure.txt`) is made by the code the viewer runs.

/// A length in mm for display: whole mm from 100 mm, one decimal below. Thousands separated by
/// a thin space, as on drawings.
pub fn format_mm(x: f32) -> String {
    let s = if x.abs() >= 100.0 {
        format!("{}", x.round() as i64)
    } else {
        format!("{x:.1}")
    };
    let (int, frac) = s
        .split_once('.')
        .map_or((s.as_str(), None), |(i, f)| (i, Some(f)));
    let (sign, digits) = int.strip_prefix('-').map_or(("", int), |d| ("-", d));
    let mut grouped = String::new();
    for (k, c) in digits.chars().enumerate() {
        if k > 0 && (digits.len() - k).is_multiple_of(3) {
            grouped.push('\u{2009}');
        }
        grouped.push(c);
    }
    match frac {
        Some(f) => format!("{sign}{grouped}.{f} mm"),
        None => format!("{sign}{grouped} mm"),
    }
}

/// The `measurements` query: `[{id, label, value_mm}]` for the distances (mm, f32: the viewer's
/// `Dimension::distance`) in order, as the panel lists them ("1. 2 500 mm", thin space); id is
/// the position, as the panel's delete uses it.
pub fn measurements_json(distances: &[f32]) -> serde_json::Value {
    distances
        .iter()
        .enumerate()
        .map(|(k, &mm)| {
            serde_json::json!({
                "id": k,
                "label": format!("{}. {}", k + 1, format_mm(mm)),
                "value_mm": f64::from(mm),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lengths_read_like_on_a_drawing() {
        assert_eq!(format_mm(3245.4), "3\u{2009}245 mm");
        assert_eq!(format_mm(1234567.0), "1\u{2009}234\u{2009}567 mm");
        assert_eq!(format_mm(250.0), "250 mm");
        assert_eq!(format_mm(12.345), "12.3 mm");
        assert_eq!(format_mm(-4500.0), "-4\u{2009}500 mm");
        assert_eq!(format_mm(0.0), "0.0 mm");
    }

    #[test]
    fn the_list_numbers_from_one() {
        assert_eq!(
            measurements_json(&[2500.0]),
            serde_json::json!([{"id": 0, "label": "1. 2\u{2009}500 mm", "value_mm": 2500.0}])
        );
    }
}
