//! Drawings on paper: storey plans at scale, written as SVG and PDF.

pub mod cut;
pub mod plan;
pub mod sheet;
pub mod template;

/// Today's date (UTC) as YYYY-MM-DD, for title blocks.
pub fn today() -> String {
    let days = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() / 86_400)
        .unwrap_or(0) as i64;
    civil_date(days)
}

/// Calendar date of a day count since 1970-01-01 (H. Hinnant's algorithm).
fn civil_date(days: i64) -> String {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02}")
}

#[cfg(test)]
mod tests {
    #[test]
    fn calendar_dates() {
        assert_eq!(super::civil_date(0), "1970-01-01");
        assert_eq!(super::civil_date(20_397), "2025-11-05");
        assert_eq!(super::civil_date(20_762), "2026-11-05");
        assert_eq!(super::civil_date(11_016), "2000-02-29");
    }
}
