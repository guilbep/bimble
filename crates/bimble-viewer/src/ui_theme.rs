//! The viewer's look (ADR 0027): one place for its colours, spacing and type sizes, mapped onto
//! egui's style. The values are those of the design canvas "Bimble UI options" (option A).
//!
//! Dark panels around a light drawing; one accent (blue) for selection and the active tool;
//! text at least 4.5:1 on its background.

use eframe::egui::{self, Color32, CornerRadius, FontId, Margin, Stroke, TextStyle, Visuals};

/// Title row (file name, quick access).
pub const TITLE_BG: Color32 = Color32::from_rgb(0x16, 0x16, 0x16);
/// Ribbon tab strip.
pub const TABS_BG: Color32 = Color32::from_rgb(0x20, 0x20, 0x20);
/// Ribbon panel (the tools of the open tab).
pub const RIBBON_BG: Color32 = Color32::from_rgb(0x2a, 0x2a, 0x2a);
/// Side panels and windows.
pub const PANEL_BG: Color32 = Color32::from_rgb(0x1e, 0x1e, 0x1e);
/// Fields (text edits, drag values, combo boxes).
pub const FIELD_BG: Color32 = Color32::from_rgb(0x15, 0x15, 0x15);
pub const BORDER: Color32 = Color32::from_rgb(0x3a, 0x3a, 0x3a);
pub const DIVIDER: Color32 = Color32::from_rgb(0x2f, 0x2f, 0x2f);
pub const TEXT: Color32 = Color32::from_rgb(0xe6, 0xe6, 0xe6);
/// Secondary text: 7.9:1 on PANEL_BG.
pub const TEXT_MUTED: Color32 = Color32::from_rgb(0xa8, 0xa8, 0xa8);
/// The accent: the active tab's mark, links, focus.
pub const ACCENT: Color32 = Color32::from_rgb(0x4a, 0x9e, 0xff);
/// Selected rows and the active tool.
pub const SELECTED_BG: Color32 = Color32::from_rgb(0x24, 0x47, 0x6e);
/// The drawing area behind 2D views and sheets.
pub const CANVAS: Color32 = Color32::from_rgb(0xf4, 0xf4, 0xf2);

/// Spacing steps, points.
pub const GAP_S: f32 = 4.0;
pub const GAP_M: f32 = 8.0;
pub const GAP_L: f32 = 12.0;
/// Corner radius of buttons and fields.
pub const RADIUS: u8 = 4;

/// Type sizes, points: body, small (captions, ribbon group names), heading.
pub const TEXT_BODY: f32 = 13.0;
pub const TEXT_SMALL: f32 = 11.0;
pub const TEXT_HEADING: f32 = 15.0;

/// Apply the theme to a context, once at start.
pub fn apply(ctx: &egui::Context) {
    ctx.set_theme(egui::Theme::Dark);
    ctx.style_mut_of(egui::Theme::Dark, |s| {
        let mut v = Visuals::dark();
        v.override_text_color = Some(TEXT);
        v.panel_fill = PANEL_BG;
        v.window_fill = PANEL_BG;
        v.window_stroke = Stroke::new(1.0, BORDER);
        v.extreme_bg_color = FIELD_BG;
        v.faint_bg_color = Color32::from_rgb(0x24, 0x24, 0x24);
        v.hyperlink_color = ACCENT;
        v.selection.bg_fill = SELECTED_BG;
        v.selection.stroke = Stroke::new(1.0, ACCENT);
        let r = CornerRadius::same(RADIUS);
        for w in [
            &mut v.widgets.noninteractive,
            &mut v.widgets.inactive,
            &mut v.widgets.hovered,
            &mut v.widgets.active,
            &mut v.widgets.open,
        ] {
            w.corner_radius = r;
        }
        v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, DIVIDER);
        v.widgets.inactive.weak_bg_fill = Color32::from_rgb(0x2b, 0x2b, 0x2b);
        v.widgets.inactive.bg_fill = Color32::from_rgb(0x2b, 0x2b, 0x2b);
        v.widgets.inactive.bg_stroke = Stroke::new(1.0, BORDER);
        v.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x35, 0x35, 0x35);
        v.widgets.hovered.bg_stroke = Stroke::new(1.0, Color32::from_rgb(0x55, 0x55, 0x55));
        v.widgets.active.weak_bg_fill = SELECTED_BG;
        v.widgets.active.bg_stroke = Stroke::new(1.0, ACCENT);
        s.visuals = v;
        s.spacing.item_spacing = egui::vec2(GAP_M, 6.0);
        s.spacing.button_padding = egui::vec2(GAP_M, 3.0);
        s.spacing.window_margin = Margin::same(GAP_L as i8);
        s.spacing.menu_margin = Margin::same(GAP_M as i8);
        s.text_styles = [
            (TextStyle::Small, FontId::proportional(TEXT_SMALL)),
            (TextStyle::Body, FontId::proportional(TEXT_BODY)),
            (TextStyle::Button, FontId::proportional(TEXT_BODY)),
            (TextStyle::Heading, FontId::proportional(TEXT_HEADING)),
            (TextStyle::Monospace, FontId::monospace(12.0)),
        ]
        .into();
    });
}

/// Relative luminance contrast of two colours (WCAG).
#[cfg(test)]
pub fn contrast(a: Color32, b: Color32) -> f32 {
    let l = |c: Color32| {
        let f = |x: u8| {
            let x = x as f32 / 255.0;
            if x <= 0.03928 {
                x / 12.92
            } else {
                ((x + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * f(c.r()) + 0.7152 * f(c.g()) + 0.0722 * f(c.b())
    };
    let (x, y) = (l(a), l(b));
    (x.max(y) + 0.05) / (x.min(y) + 0.05)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every text colour on every background it is used on reads at 4.5:1 or better.
    #[test]
    fn text_contrast_meets_aa() {
        for bg in [TITLE_BG, TABS_BG, RIBBON_BG, PANEL_BG, FIELD_BG] {
            for fg in [TEXT, TEXT_MUTED, ACCENT] {
                let c = contrast(fg, bg);
                assert!(c >= 4.5, "{fg:?} on {bg:?}: {c:.2}");
            }
        }
        assert!(contrast(Color32::WHITE, SELECTED_BG) >= 4.5);
    }
}
