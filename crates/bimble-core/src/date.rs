//! Dates without a time-zone database: UTC only.

/// Seconds since 1970-01-01 UTC, now.
#[cfg(target_arch = "wasm32")]
pub fn now_secs() -> i64 {
    // std's clock panics in browsers.
    (js_sys::Date::now() / 1000.0) as i64
}

/// Seconds since 1970-01-01 UTC, now.
#[cfg(not(target_arch = "wasm32"))]
pub fn now_secs() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Today (UTC) as YYYY-MM-DD.
pub fn today() -> String {
    civil_date(now_secs().div_euclid(86_400))
}

/// `YYYY-MM-DD HH:MM` (UTC) of a time in seconds since 1970.
pub fn format_utc(secs: i64) -> String {
    let (d, s) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    format!("{} {:02}:{:02}", civil_date(d), s / 3600, s % 3600 / 60)
}

/// `YYYY-MM-DDTHH:MM:SS` (UTC, ISO 8601) of a time in seconds since 1970: IFC file headers.
pub fn iso_utc(secs: i64) -> String {
    let (d, s) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    format!(
        "{}T{:02}:{:02}:{:02}",
        civil_date(d),
        s / 3600,
        s % 3600 / 60,
        s % 60
    )
}

/// Calendar date of a day count since 1970-01-01 (H. Hinnant's algorithm).
pub fn civil_date(days: i64) -> String {
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
        assert_eq!(super::civil_date(-1), "1969-12-31");
        assert_eq!(
            super::format_utc(20_762 * 86_400 + 3_600 * 13 + 125),
            "2026-11-05 13:02"
        );
    }
}
