//! Drawings on paper: storey plans at scale, written as SVG and PDF.

pub mod cut;
pub mod plan;
pub mod reference;
pub mod sheet;
pub mod template;

/// Today's date (UTC) as YYYY-MM-DD, for title blocks.
pub fn today() -> String {
    bimble_core::date::today()
}
