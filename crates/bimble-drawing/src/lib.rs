//! Drawings on paper: storey plans at scale, written as SVG and PDF.

pub mod crop;
pub mod cut;
pub mod fonts;
pub mod plan;
#[cfg(feature = "reference-plans")]
pub mod reference;
/// The web viewer build has no DXF or PDF reader (ADR 0050): every reference plan is refused.
#[cfg(not(feature = "reference-plans"))]
pub mod reference {
    pub type Polyline = Vec<[f64; 2]>;

    #[derive(Debug, Clone, PartialEq)]
    pub struct Read {
        pub polylines: Vec<Polyline>,
        pub notes: Vec<String>,
    }

    pub fn read(name: &str, _bytes: &[u8], _pdf_scale: f64) -> Result<Read, String> {
        Err(format!(
            "{name}: reference plans are not in the viewer build"
        ))
    }
}
pub mod schedule;
pub mod sheet;
pub mod sheetdraw;
pub mod template;
pub mod viewdraw;

/// Today's date (UTC) as YYYY-MM-DD, for title blocks.
pub fn today() -> String {
    bimble_core::date::today()
}
