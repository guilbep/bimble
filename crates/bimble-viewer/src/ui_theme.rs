//! The viewer's look (ADR 0027, ADR 0041): one place for its colours, spacing and type sizes,
//! mapped onto egui's style. The colours come from a theme the user picks in Settings
//! (Appearance); the default, Grey, is issue #53's (surfaces around RGB 105, white text).
//!
//! Every theme keeps its text at 4.5:1 or better on every surface it is drawn on (WCAG AA),
//! checked by a test.

use eframe::egui::{self, Color32, CornerRadius, FontId, Margin, Stroke, TextStyle, Visuals};
use std::sync::atomic::{AtomicU8, AtomicU32, Ordering};

/// A set of interface colours.
#[derive(Clone, Copy, Debug)]
pub struct Theme {
    /// Its id in the settings file.
    pub id: &'static str,
    /// Its name in Settings.
    pub name: &'static str,
    /// Light text on dark surfaces (egui's dark visuals as the base).
    pub dark: bool,
    /// Title row (file name, quick access).
    pub title_bg: Color32,
    /// Ribbon tab strip.
    pub tabs_bg: Color32,
    /// Ribbon panel (the tools of the open tab).
    pub ribbon_bg: Color32,
    /// Side panels and windows.
    pub panel_bg: Color32,
    /// Fields (text edits, drag values, combo boxes).
    pub field_bg: Color32,
    pub border: Color32,
    pub divider: Color32,
    pub text: Color32,
    /// Secondary text.
    pub text_muted: Color32,
    /// The accent: the active tab's mark, links, focus.
    pub accent: Color32,
    /// Selected rows and the active tool.
    pub selected_bg: Color32,
    /// Text on `selected_bg`.
    pub on_selected: Color32,
    /// Buttons at rest, and hovered.
    pub widget_bg: Color32,
    pub hover_bg: Color32,
    /// Icons of actions not available now: faint on purpose.
    pub disabled: Color32,
    /// Alternate rows.
    pub faint_bg: Color32,
}

const fn rgb(x: u32) -> Color32 {
    Color32::from_rgb((x >> 16) as u8, (x >> 8) as u8, x as u8)
}

/// The themes, the default first.
pub const THEMES: [Theme; 6] = [
    Theme {
        id: "grey",
        name: "Grey",
        dark: true,
        title_bg: rgb(0x4b4b4b),
        tabs_bg: rgb(0x585858),
        ribbon_bg: rgb(0x696969),
        panel_bg: rgb(0x5f5f5f),
        field_bg: rgb(0x4a4a4a),
        border: rgb(0x7a7a7a),
        divider: rgb(0x707070),
        text: rgb(0xffffff),
        text_muted: rgb(0xebebeb),
        accent: rgb(0xd6ecff),
        selected_bg: rgb(0x1f4e8c),
        on_selected: rgb(0xffffff),
        widget_bg: rgb(0x666666),
        hover_bg: rgb(0x555555),
        disabled: rgb(0x949494),
        faint_bg: rgb(0x646464),
    },
    Theme {
        id: "dark",
        name: "Dark",
        dark: true,
        title_bg: rgb(0x161616),
        tabs_bg: rgb(0x202020),
        ribbon_bg: rgb(0x2a2a2a),
        panel_bg: rgb(0x1e1e1e),
        field_bg: rgb(0x151515),
        border: rgb(0x3a3a3a),
        divider: rgb(0x2f2f2f),
        text: rgb(0xe6e6e6),
        text_muted: rgb(0xa8a8a8),
        accent: rgb(0x4a9eff),
        selected_bg: rgb(0x24476e),
        on_selected: rgb(0xffffff),
        widget_bg: rgb(0x2b2b2b),
        hover_bg: rgb(0x313131),
        disabled: rgb(0x666666),
        faint_bg: rgb(0x242424),
    },
    Theme {
        id: "light",
        name: "Light",
        dark: false,
        title_bg: rgb(0xdcdcdc),
        tabs_bg: rgb(0xe6e6e6),
        ribbon_bg: rgb(0xf2f2f2),
        panel_bg: rgb(0xf7f7f7),
        field_bg: rgb(0xffffff),
        border: rgb(0xc4c4c4),
        divider: rgb(0xd6d6d6),
        text: rgb(0x1a1a1a),
        text_muted: rgb(0x555555),
        accent: rgb(0x0b5cad),
        selected_bg: rgb(0xcfe3fa),
        on_selected: rgb(0x1a1a1a),
        widget_bg: rgb(0xe9e9e9),
        hover_bg: rgb(0xdcdcdc),
        disabled: rgb(0xa0a0a0),
        faint_bg: rgb(0xefefef),
    },
    // Ethan Schoonover's Solarized, its body text one step brighter for 4.5:1.
    Theme {
        id: "solarized-dark",
        name: "Solarized dark",
        dark: true,
        title_bg: rgb(0x00212b),
        tabs_bg: rgb(0x002b36),
        ribbon_bg: rgb(0x073642),
        panel_bg: rgb(0x002b36),
        field_bg: rgb(0x00212b),
        border: rgb(0x2a5560),
        divider: rgb(0x0d4150),
        text: rgb(0xeee8d5),
        text_muted: rgb(0x93a1a1),
        accent: rgb(0x6cb4f0),
        selected_bg: rgb(0x1d5a6e),
        on_selected: rgb(0xeee8d5),
        widget_bg: rgb(0x073642),
        hover_bg: rgb(0x04303c),
        disabled: rgb(0x4f6b73),
        faint_bg: rgb(0x042f3b),
    },
    Theme {
        id: "solarized-light",
        name: "Solarized light",
        dark: false,
        title_bg: rgb(0xeee8d5),
        tabs_bg: rgb(0xf5efdc),
        ribbon_bg: rgb(0xfdf6e3),
        panel_bg: rgb(0xfdf6e3),
        field_bg: rgb(0xffffff),
        border: rgb(0xd3cbb7),
        divider: rgb(0xe4dcc8),
        text: rgb(0x073642),
        text_muted: rgb(0x4c5f66),
        accent: rgb(0x155a8a),
        selected_bg: rgb(0xd3e6f5),
        on_selected: rgb(0x073642),
        widget_bg: rgb(0xefe8d4),
        hover_bg: rgb(0xe6dec8),
        disabled: rgb(0xa8a28f),
        faint_bg: rgb(0xf7f0dc),
    },
    Theme {
        id: "high-contrast",
        name: "High contrast",
        dark: true,
        title_bg: rgb(0x000000),
        tabs_bg: rgb(0x000000),
        ribbon_bg: rgb(0x101010),
        panel_bg: rgb(0x000000),
        field_bg: rgb(0x000000),
        border: rgb(0xffffff),
        divider: rgb(0x808080),
        text: rgb(0xffffff),
        text_muted: rgb(0xd0d0d0),
        accent: rgb(0xffd800),
        selected_bg: rgb(0x0050c0),
        on_selected: rgb(0xffffff),
        widget_bg: rgb(0x1a1a1a),
        hover_bg: rgb(0x333333),
        disabled: rgb(0x8a8a8a),
        faint_bg: rgb(0x0a0a0a),
    },
];

/// The interface font.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UiFont {
    /// Roboto, shipped with Bimble (the default, issue #53).
    Roboto,
    /// egui's own font (Ubuntu Light), Bimble's until 0.10.
    Ubuntu,
}

impl UiFont {
    pub const ALL: [UiFont; 2] = [UiFont::Roboto, UiFont::Ubuntu];

    pub fn id(self) -> &'static str {
        match self {
            UiFont::Roboto => "roboto",
            UiFont::Ubuntu => "ubuntu",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            UiFont::Roboto => "Roboto",
            UiFont::Ubuntu => "Ubuntu Light",
        }
    }

    pub fn from_id(id: &str) -> Option<UiFont> {
        UiFont::ALL.into_iter().find(|f| f.id() == id)
    }
}

/// Body text size, points: the default and the range offered (issue #53: smaller than 0.10's 13).
pub const TEXT_SIZE: f32 = 12.0;
pub const TEXT_SIZES: std::ops::RangeInclusive<f32> = 10.0..=16.0;

/// The theme in use, an index in `THEMES`; the body text size, as f32 bits.
static CURRENT: AtomicU8 = AtomicU8::new(0);
static BODY: AtomicU32 = AtomicU32::new(TEXT_SIZE.to_bits());

/// The theme in use.
pub fn current() -> &'static Theme {
    &THEMES[(CURRENT.load(Ordering::Relaxed) as usize).min(THEMES.len() - 1)]
}

/// The theme of that id; the default for one unknown.
pub fn by_id(id: &str) -> &'static Theme {
    THEMES.iter().find(|t| t.id == id).unwrap_or(&THEMES[0])
}

pub fn title_bg() -> Color32 {
    current().title_bg
}
pub fn tabs_bg() -> Color32 {
    current().tabs_bg
}
pub fn ribbon_bg() -> Color32 {
    current().ribbon_bg
}
pub fn panel_bg() -> Color32 {
    current().panel_bg
}
pub fn border() -> Color32 {
    current().border
}
pub fn text() -> Color32 {
    current().text
}
pub fn text_muted() -> Color32 {
    current().text_muted
}
pub fn accent() -> Color32 {
    current().accent
}
pub fn selected_bg() -> Color32 {
    current().selected_bg
}
pub fn hover_bg() -> Color32 {
    current().hover_bg
}
pub fn disabled() -> Color32 {
    current().disabled
}

/// The drawing area behind 2D views and sheets: paper, whatever the theme.
pub const CANVAS: Color32 = Color32::from_rgb(0xf4, 0xf4, 0xf2);
/// Text on `CANVAS`.
pub const ON_CANVAS: Color32 = Color32::from_rgb(0x1a, 0x1a, 0x1a);

/// Spacing steps, points.
pub const GAP_S: f32 = 4.0;
pub const GAP_M: f32 = 8.0;
pub const GAP_L: f32 = 12.0;
/// Corner radius of buttons and fields.
pub const RADIUS: u8 = 4;

/// Body text size in use, points.
pub fn text_body() -> f32 {
    f32::from_bits(BODY.load(Ordering::Relaxed))
}
/// Captions, ribbon group names.
pub fn text_small() -> f32 {
    text_body() - 2.0
}

/// The interface fonts: `font` first for text, egui's for what it lacks (symbols) and for
/// monospace.
fn fonts(font: UiFont) -> egui::FontDefinitions {
    let mut f = egui::FontDefinitions::default();
    if font == UiFont::Roboto {
        f.font_data.insert(
            "Roboto".into(),
            std::sync::Arc::new(egui::FontData::from_static(
                bimble_drawing::fonts::ROBOTO_REGULAR,
            )),
        );
        if let Some(p) = f.families.get_mut(&egui::FontFamily::Proportional) {
            p.insert(0, "Roboto".into());
        }
    }
    f
}

/// Apply a theme, a font and a body text size to a context: at start, and when changed in
/// Settings.
pub fn apply(ctx: &egui::Context, theme: &'static Theme, font: UiFont, size: f32) {
    let index = THEMES.iter().position(|t| t.id == theme.id).unwrap_or(0);
    CURRENT.store(index as u8, Ordering::Relaxed);
    let size = size.clamp(*TEXT_SIZES.start(), *TEXT_SIZES.end());
    BODY.store(size.to_bits(), Ordering::Relaxed);
    ctx.set_fonts(fonts(font));
    let t = theme;
    let egui_theme = if t.dark {
        egui::Theme::Dark
    } else {
        egui::Theme::Light
    };
    ctx.set_theme(egui_theme);
    ctx.style_mut_of(egui_theme, |s| {
        let mut v = if t.dark {
            Visuals::dark()
        } else {
            Visuals::light()
        };
        v.override_text_color = Some(t.text);
        v.panel_fill = t.panel_bg;
        v.window_fill = t.panel_bg;
        v.window_stroke = Stroke::new(1.0, t.border);
        v.extreme_bg_color = t.field_bg;
        v.faint_bg_color = t.faint_bg;
        v.hyperlink_color = t.accent;
        v.selection.bg_fill = t.selected_bg;
        v.selection.stroke = Stroke::new(1.0, t.on_selected);
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
        v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, t.divider);
        v.widgets.inactive.weak_bg_fill = t.widget_bg;
        v.widgets.inactive.bg_fill = t.widget_bg;
        v.widgets.inactive.bg_stroke = Stroke::new(1.0, t.border);
        v.widgets.hovered.weak_bg_fill = t.hover_bg;
        v.widgets.hovered.bg_fill = t.hover_bg;
        v.widgets.hovered.bg_stroke = Stroke::new(1.0, t.text_muted);
        v.widgets.active.weak_bg_fill = t.selected_bg;
        v.widgets.active.bg_fill = t.selected_bg;
        v.widgets.active.bg_stroke = Stroke::new(1.0, t.accent);
        v.widgets.open.weak_bg_fill = t.hover_bg;
        s.visuals = v;
        s.spacing.item_spacing = egui::vec2(GAP_M, 6.0);
        s.spacing.button_padding = egui::vec2(GAP_M, 3.0);
        s.spacing.window_margin = Margin::same(GAP_L as i8);
        s.spacing.menu_margin = Margin::same(GAP_M as i8);
        s.text_styles = [
            (TextStyle::Small, FontId::proportional(size - 2.0)),
            (TextStyle::Body, FontId::proportional(size)),
            (TextStyle::Button, FontId::proportional(size)),
            (TextStyle::Heading, FontId::proportional(size + 2.0)),
            (TextStyle::Monospace, FontId::monospace(size - 1.0)),
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

    /// In every theme, every text colour on every surface it is used on reads at 4.5:1 or
    /// better.
    #[test]
    fn text_contrast_meets_aa() {
        for t in &THEMES {
            let surfaces = [
                ("title", t.title_bg),
                ("tabs", t.tabs_bg),
                ("ribbon", t.ribbon_bg),
                ("panel", t.panel_bg),
                ("field", t.field_bg),
                ("button", t.widget_bg),
                ("hovered button", t.hover_bg),
            ];
            for (bg_name, bg) in surfaces {
                for (fg_name, fg) in [
                    ("text", t.text),
                    ("secondary text", t.text_muted),
                    ("accent", t.accent),
                ] {
                    let c = contrast(fg, bg);
                    assert!(c >= 4.5, "{}: {fg_name} on {bg_name}: {c:.2}", t.name);
                }
            }
            let c = contrast(t.on_selected, t.selected_bg);
            assert!(c >= 4.5, "{}: text on selected: {c:.2}", t.name);
        }
        assert!(contrast(ON_CANVAS, CANVAS) >= 4.5);
    }

    /// Issue #53: the default theme's ribbon is RGB 105, its text white.
    #[test]
    fn the_default_is_issue_53s_grey() {
        assert_eq!(THEMES[0].ribbon_bg, Color32::from_gray(105));
        assert_eq!(THEMES[0].text, Color32::WHITE);
        assert_eq!(by_id("no such theme").id, "grey");
    }
}
