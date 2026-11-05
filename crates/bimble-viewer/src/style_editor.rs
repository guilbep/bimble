//! The "Graphic style" window: every parameter of a `GraphicStyle`, edited live.
//!
//! Edits only change the style uniform, never the uploaded model, so they are instant even on
//! large models.

use bimble_core::MaterialCategory;
use bimble_render::{Fill, GraphicStyle, Lines, Rgb, Stipple};
use eframe::egui::{self, Slider};

/// Draws the editor. Returns true when the style changed.
pub fn edit(ui: &mut egui::Ui, s: &mut GraphicStyle) -> bool {
    let mut changed = false;
    egui::Grid::new("style-general")
        .num_columns(2)
        .spacing([12.0, 6.0])
        .show(ui, |ui| {
            changed |= colour_row(ui, "Background", &mut s.background);
            changed |= colour_row(ui, "Edges", &mut s.edges);
            changed |= colour_row(ui, "Selection", &mut s.selection);
            changed |= colour_row(ui, "Hover", &mut s.hover);
            changed |= colour_row(ui, "Dimensions", &mut s.dimensions);
            ui.label("Hatch strength");
            changed |= ui
                .add(Slider::new(&mut s.hatch_strength, 0.0..=1.0))
                .changed();
            ui.end_row();
            ui.label("Hatch darkening")
                .on_hover_text("For families without an ink colour: how much darker the hatch is");
            changed |= ui
                .add(Slider::new(&mut s.hatch_darken, 0.0..=1.0))
                .changed();
            ui.end_row();
            ui.label("Ambient light")
                .on_hover_text("1 = flat colours, lower = faces shaded by orientation");
            changed |= ui.add(Slider::new(&mut s.ambient, 0.0..=1.0)).changed();
            ui.end_row();
        });
    ui.separator();
    for c in MaterialCategory::ALL {
        egui::CollapsingHeader::new(c.label())
            .id_salt(c.label())
            .show(ui, |ui| changed |= fill(ui, c, s.families.get_mut(c)));
    }
    changed
}

fn colour_row(ui: &mut egui::Ui, label: &str, c: &mut Rgb) -> bool {
    ui.label(label);
    let r = ui.color_edit_button_srgb(&mut c.0).changed();
    ui.end_row();
    r
}

fn fill(ui: &mut egui::Ui, c: MaterialCategory, f: &mut Fill) -> bool {
    let mut changed = false;
    egui::Grid::new(("fill", c.label()))
        .num_columns(2)
        .spacing([12.0, 4.0])
        .show(ui, |ui| {
            changed |= colour_row(ui, "Fill", &mut f.color);
            ui.label("Hatch ink");
            ui.horizontal(|ui| {
                let mut custom = f.ink.is_some();
                if ui.checkbox(&mut custom, "own colour").changed() {
                    f.ink = custom.then_some(Rgb([30, 30, 30]));
                    changed = true;
                }
                if let Some(ink) = &mut f.ink {
                    changed |= ui.color_edit_button_srgb(&mut ink.0).changed();
                }
            });
            ui.end_row();
        });
    for k in 0..2 {
        changed |= line_family(ui, c, k, &mut f.lines[k]);
    }
    let mut on = f.stipple.is_some();
    if ui.checkbox(&mut on, "Stipple").changed() {
        f.stipple = on.then_some(Stipple {
            cell_px: 2.0,
            density: 0.1,
            strength: 1.0,
        });
        changed = true;
    }
    if let Some(st) = &mut f.stipple {
        ui.indent(("stipple", c.label()), |ui| {
            changed |= ui
                .add(
                    Slider::new(&mut st.cell_px, 0.25..=20.0)
                        .logarithmic(true)
                        .text("cell (px)"),
                )
                .changed();
            changed |= ui
                .add(Slider::new(&mut st.density, 0.0..=1.0).text("density"))
                .changed();
            changed |= ui
                .add(Slider::new(&mut st.strength, 0.0..=1.0).text("strength"))
                .changed();
        });
    }
    changed
}

fn line_family(ui: &mut egui::Ui, c: MaterialCategory, k: usize, l: &mut Option<Lines>) -> bool {
    let mut changed = false;
    let mut on = l.is_some();
    if ui.checkbox(&mut on, format!("Lines {}", k + 1)).changed() {
        *l = on.then_some(Lines {
            angle_deg: if k == 0 { 45.0 } else { 135.0 },
            spacing_px: 8.0,
            width: 0.1,
            dash_px: None,
            dash_on: 1.0,
        });
        changed = true;
    }
    if let Some(l) = l {
        ui.indent(("lines", c.label(), k), |ui| {
            changed |= ui
                .add(Slider::new(&mut l.angle_deg, 0.0..=180.0).text("angle (°)"))
                .changed();
            changed |= ui
                .add(
                    Slider::new(&mut l.spacing_px, 1.0..=100.0)
                        .logarithmic(true)
                        .text("spacing (px)"),
                )
                .changed();
            changed |= ui
                .add(Slider::new(&mut l.width, 0.0..=1.0).text("width"))
                .changed();
            let mut dashed = l.dash_px.is_some();
            if ui.checkbox(&mut dashed, "dashed").changed() {
                l.dash_px = dashed.then_some(10.0);
                l.dash_on = if dashed { 0.6 } else { 1.0 };
                changed = true;
            }
            if let Some(d) = &mut l.dash_px {
                changed |= ui
                    .add(
                        Slider::new(d, 1.0..=100.0)
                            .logarithmic(true)
                            .text("dash period (px)"),
                    )
                    .changed();
                changed |= ui
                    .add(Slider::new(&mut l.dash_on, 0.0..=1.0).text("dash on"))
                    .changed();
            }
        });
    }
    changed
}
