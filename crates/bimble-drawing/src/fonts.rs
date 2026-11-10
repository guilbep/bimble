//! Fonts shipped with Bimble (issue #53, ADR 0041), the same on every machine and in the
//! browser: Roboto 2.138 (Apache-2.0, `fonts/LICENSE-Roboto`; from Fedora's signed
//! `google-roboto-fonts` package), for the interface and for text on paper.

use skrifa::instance::{LocationRef, Size};
use skrifa::{FontRef, GlyphId, MetadataProvider};
use std::sync::OnceLock;

pub const ROBOTO_REGULAR: &[u8] = include_bytes!("../fonts/Roboto-Regular.ttf");
pub const ROBOTO_BOLD: &[u8] = include_bytes!("../fonts/Roboto-Bold.ttf");

/// A font file and what a PDF needs to know about it, read once.
pub struct Face {
    font: FontRef<'static>,
    /// The file, embedded as is in PDFs.
    pub data: &'static [u8],
    /// Its PostScript name.
    pub name: &'static str,
    /// Font units per em; the metrics below are in 1/1000 em, as PDF wants them.
    units_per_em: f32,
    pub ascent: f32,
    pub descent: f32,
    pub cap_height: f32,
    /// x min, y min, x max, y max.
    pub bbox: [f32; 4],
}

impl Face {
    fn read(data: &'static [u8], name: &'static str) -> Face {
        // Our own file, checked by a test: it parses.
        let font = FontRef::new(data).expect("shipped font parses");
        let m = font.metrics(Size::unscaled(), LocationRef::default());
        let upem = m.units_per_em as f32;
        let k = 1000.0 / upem;
        let bbox = m.bounds.map_or([0.0, m.descent, upem, m.ascent], |b| {
            [b.x_min, b.y_min, b.x_max, b.y_max]
        });
        Face {
            font,
            data,
            name,
            units_per_em: upem,
            ascent: m.ascent * k,
            descent: m.descent * k,
            cap_height: m.cap_height.unwrap_or(m.ascent) * k,
            bbox: bbox.map(|v| v * k),
        }
    }

    /// Roboto, regular or bold.
    pub fn roboto(bold: bool) -> &'static Face {
        static REGULAR: OnceLock<Face> = OnceLock::new();
        static BOLD: OnceLock<Face> = OnceLock::new();
        if bold {
            BOLD.get_or_init(|| Face::read(ROBOTO_BOLD, "Roboto-Bold"))
        } else {
            REGULAR.get_or_init(|| Face::read(ROBOTO_REGULAR, "Roboto-Regular"))
        }
    }

    /// The glyph of a character; 0 (the missing-glyph box) for one the font lacks.
    pub fn glyph(&self, c: char) -> u16 {
        self.font
            .charmap()
            .map(c)
            .map_or(0, |g| u16::try_from(g.to_u32()).unwrap_or(0))
    }

    /// Advance widths of glyphs, 1/1000 em.
    pub fn advances(&self, glyphs: impl IntoIterator<Item = u16>) -> Vec<f32> {
        let m = self
            .font
            .glyph_metrics(Size::unscaled(), LocationRef::default());
        let k = 1000.0 / self.units_per_em;
        glyphs
            .into_iter()
            .map(|g| m.advance_width(GlyphId::new(g.into())).unwrap_or(0.0) * k)
            .collect()
    }

    /// Width of `text`, in em.
    pub fn width_em(&self, text: &str) -> f64 {
        let sum: f32 = self
            .advances(text.chars().map(|c| self.glyph(c)))
            .iter()
            .sum();
        sum as f64 / 1000.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The shipped fonts parse, map Latin and accented letters, and measure like Roboto: its
    /// digits are 0.562 em (tabular), wider than Helvetica's 0.556.
    #[test]
    fn roboto_reads_and_measures() {
        for bold in [false, true] {
            let f = Face::roboto(bold);
            assert!(f.glyph('A') != 0 && f.glyph('é') != 0 && f.glyph('–') != 0);
            assert!(
                f.ascent > 900.0 && f.descent < -200.0,
                "{} {}",
                f.ascent,
                f.descent
            );
            assert!(f.cap_height > 650.0 && f.cap_height < 750.0);
        }
        let w = Face::roboto(false).width_em("0123456789");
        assert!((w - 5.62).abs() < 0.01, "{w}");
        assert!(Face::roboto(true).width_em("Bimble") > Face::roboto(false).width_em("Bimble"));
    }
}
