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
    ui.separator();
    ui.strong("On paper");
    egui::CollapsingHeader::new("Line weights")
        .id_salt("pens")
        .show(ui, |ui| changed |= pens(ui, &mut s.paper.pens));
    egui::CollapsingHeader::new("Line types")
        .id_salt("line-types")
        .show(ui, |ui| changed |= line_types(ui, &mut s.paper));
    egui::CollapsingHeader::new("Lines on drawings")
        .id_salt("line-roles")
        .show(ui, |ui| changed |= roles(ui, &mut s.paper));
    egui::CollapsingHeader::new("Text types")
        .id_salt("text-types")
        .show(ui, |ui| changed |= text_types(ui, &mut s.paper));
    egui::CollapsingHeader::new("Dimension types")
        .id_salt("dim-types")
        .show(ui, |ui| changed |= dim_types(ui, &mut s.paper));
    changed
}

/// A name not used yet: `base 1`, `base 2`…
fn fresh_name(base: &str, taken: impl Fn(&str) -> bool) -> String {
    let mut n = 1;
    while taken(&format!("{base} {n}")) {
        n += 1;
    }
    format!("{base} {n}")
}

/// Text types (#43): name, font, size on paper, colour.
fn text_types(ui: &mut egui::Ui, paper: &mut bimble_core::style::PaperStyle) -> bool {
    let v = &mut paper.text_types;
    let mut changed = false;
    let mut remove = None;
    egui::ScrollArea::vertical()
        .id_salt("text-types-scroll")
        .max_height(300.0)
        .show(ui, |ui| {
            egui::Grid::new("text-types-grid")
                .striped(true)
                .spacing([6.0, 2.0])
                .show(ui, |ui| {
                    ui.strong("Name");
                    ui.strong("Font");
                    ui.strong("Size (mm)");
                    ui.strong("Colour");
                    ui.label("");
                    ui.end_row();
                    for (i, t) in v.iter_mut().enumerate() {
                        changed |= ui
                            .add(egui::TextEdit::singleline(&mut t.name).desired_width(140.0))
                            .lost_focus();
                        changed |= ui
                            .add(egui::TextEdit::singleline(&mut t.font).desired_width(70.0))
                            .lost_focus();
                        changed |= ui
                            .add(
                                egui::DragValue::new(&mut t.size_mm)
                                    .range(0.5..=50.0)
                                    .speed(0.1),
                            )
                            .changed();
                        changed |= ui.color_edit_button_srgb(&mut t.color.0).changed();
                        if ui.small_button("x").clicked() {
                            remove = Some(i);
                        }
                        ui.end_row();
                    }
                });
        });
    if let Some(i) = remove {
        v.remove(i);
        changed = true;
    }
    if ui.button("Add text type").clicked() {
        let name = fresh_name("Text", |n| v.iter().any(|t| t.name == n));
        v.push(bimble_core::pens::TextType {
            name,
            font: "Arial".into(),
            size_mm: 2.5,
            color: Rgb([0, 0, 0]),
        });
        changed = true;
    }
    changed
}

/// Dimension types (#43): unit, decimals, unit written or not, colour, text size, line type.
fn dim_types(ui: &mut egui::Ui, paper: &mut bimble_core::style::PaperStyle) -> bool {
    use bimble_core::pens::DimUnit;
    let names: Vec<String> = paper.line_types.iter().map(|t| t.name.clone()).collect();
    let v = &mut paper.dimension_types;
    let mut changed = false;
    let mut remove = None;
    egui::ScrollArea::vertical()
        .id_salt("dim-types-scroll")
        .max_height(300.0)
        .show(ui, |ui| {
            egui::Grid::new("dim-types-grid")
                .striped(true)
                .spacing([6.0, 2.0])
                .show(ui, |ui| {
                    for h in [
                        "Name",
                        "Unit",
                        "Decimals",
                        "Unit shown",
                        "Colour",
                        "Text (mm)",
                        "Line",
                        "",
                    ] {
                        ui.strong(h);
                    }
                    ui.end_row();
                    for (i, t) in v.iter_mut().enumerate() {
                        changed |= ui
                            .add(egui::TextEdit::singleline(&mut t.name).desired_width(110.0))
                            .lost_focus();
                        egui::ComboBox::from_id_salt(("dim-unit", i))
                            .selected_text(match t.unit {
                                DimUnit::M => "m",
                                DimUnit::Cm => "cm",
                                DimUnit::Mm => "mm",
                            })
                            .width(50.0)
                            .show_ui(ui, |ui| {
                                for (u, l) in
                                    [(DimUnit::M, "m"), (DimUnit::Cm, "cm"), (DimUnit::Mm, "mm")]
                                {
                                    changed |= ui.selectable_value(&mut t.unit, u, l).changed();
                                }
                            });
                        changed |= ui
                            .add(egui::DragValue::new(&mut t.decimals).range(0..=4))
                            .changed();
                        changed |= ui.checkbox(&mut t.show_unit, "").changed();
                        changed |= ui.color_edit_button_srgb(&mut t.color.0).changed();
                        changed |= ui
                            .add(
                                egui::DragValue::new(&mut t.text_mm)
                                    .range(0.5..=20.0)
                                    .speed(0.1),
                            )
                            .changed();
                        egui::ComboBox::from_id_salt(("dim-line", i))
                            .selected_text(t.line_type.clone())
                            .height(300.0)
                            .show_ui(ui, |ui| {
                                for n in &names {
                                    changed |= ui
                                        .selectable_value(&mut t.line_type, n.clone(), n)
                                        .changed();
                                }
                            });
                        if ui.small_button("x").clicked() {
                            remove = Some(i);
                        }
                        ui.end_row();
                    }
                });
        });
    if let Some(i) = remove {
        v.remove(i);
        changed = true;
    }
    if ui.button("Add dimension type").clicked() {
        let name = fresh_name("Dimension", |n| v.iter().any(|t| t.name == n));
        v.push(bimble_core::pens::DimType {
            name,
            unit: DimUnit::Cm,
            decimals: 0,
            show_unit: false,
            color: Rgb([0, 0, 0]),
            text_mm: 2.5,
            line_type: names.first().cloned().unwrap_or_default(),
        });
        changed = true;
    }
    changed
}

/// Which pen table is shown.
#[derive(Clone, Copy, PartialEq, Eq, Default)]
enum PenTab {
    #[default]
    Model,
    Perspective,
    Annotation,
}

/// The pen tables, like Revit's *Line weights* dialog: 16 pens; model pens per scale.
fn pens(ui: &mut egui::Ui, t: &mut bimble_core::pens::PenTable) -> bool {
    use bimble_core::pens::PENS;
    let id = egui::Id::unique("bimble-pen-tab");
    let mut tab: PenTab = ui.data(|d| d.get_temp(id)).unwrap_or_default();
    let mut changed = false;
    ui.horizontal(|ui| {
        for (k, label) in [
            (PenTab::Model, "Model"),
            (PenTab::Perspective, "Perspective"),
            (PenTab::Annotation, "Annotation"),
        ] {
            if ui.selectable_label(tab == k, label).clicked() {
                tab = k;
            }
        }
    });
    ui.data_mut(|d| d.insert_temp(id, tab));
    ui.weak(match tab {
        PenTab::Model => {
            "Lines of objects (cut, seen, hatches): the thickness depends on the drawing's scale; \
             the nearest scale's column is used."
        }
        PenTab::Perspective => "Lines in 3D views.",
        PenTab::Annotation => "Dimensions, grids, texts, frames: the same at every scale.",
    });
    let cell = |ui: &mut egui::Ui, v: &mut f32| {
        ui.add(
            egui::DragValue::new(v)
                .range(0.01..=20.0)
                .speed(0.01)
                .max_decimals(4)
                .suffix(" mm"),
        )
        .changed()
    };
    egui::ScrollArea::horizontal().show(ui, |ui| {
        egui::Grid::new(("pen-grid", tab as u8))
            .striped(true)
            .spacing([6.0, 2.0])
            .show(ui, |ui| match tab {
                PenTab::Model => {
                    ui.label("");
                    let mut remove = None;
                    for sc in t.scales.clone() {
                        ui.horizontal(|ui| {
                            ui.strong(format!("1:{sc}"));
                            if t.scales.len() > 1
                                && ui
                                    .small_button("x")
                                    .on_hover_text("Remove this scale")
                                    .clicked()
                            {
                                remove = Some(sc);
                            }
                        });
                    }
                    ui.end_row();
                    for (p, row) in t.model.iter_mut().enumerate().take(PENS) {
                        ui.label((p + 1).to_string());
                        for v in row.iter_mut() {
                            changed |= cell(ui, v);
                        }
                        ui.end_row();
                    }
                    if let Some(sc) = remove {
                        t.remove_scale(sc);
                        changed = true;
                    }
                }
                PenTab::Perspective | PenTab::Annotation => {
                    let v = if tab == PenTab::Perspective {
                        &mut t.perspective
                    } else {
                        &mut t.annotation
                    };
                    for (p, x) in v.iter_mut().enumerate().take(PENS) {
                        ui.label((p + 1).to_string());
                        changed |= cell(ui, x);
                        ui.end_row();
                    }
                }
            });
    });
    if tab == PenTab::Model {
        let id = egui::Id::unique("bimble-new-scale");
        let mut n: u32 = ui.data(|d| d.get_temp(id)).unwrap_or(75);
        ui.horizontal(|ui| {
            ui.label("Add scale 1:");
            ui.add(egui::DragValue::new(&mut n).range(1..=10_000));
            if ui.button("Add").clicked() {
                t.add_scale(n);
                changed = true;
            }
        });
        ui.data_mut(|d| d.insert_temp(id, n));
    }
    changed
}

/// The named line types: name, colour, dashes, pen.
fn line_types(ui: &mut egui::Ui, paper: &mut bimble_core::style::PaperStyle) -> bool {
    let old: Vec<String> = paper.line_types.iter().map(|t| t.name.clone()).collect();
    let v = &mut paper.line_types;
    let mut changed = false;
    let mut remove = None;
    ui.weak("Dashes: dash, gap, dash, gap… in mm (empty: continuous).");
    egui::ScrollArea::vertical()
        .id_salt("line-types-scroll")
        .max_height(300.0)
        .show(ui, |ui| {
            egui::Grid::new("line-types-grid")
                .striped(true)
                .spacing([6.0, 2.0])
                .show(ui, |ui| {
                    ui.strong("Name");
                    ui.strong("Colour");
                    ui.strong("Dashes");
                    ui.strong("Pen");
                    ui.label("");
                    ui.end_row();
                    for (i, t) in v.iter_mut().enumerate() {
                        changed |= ui
                            .add(egui::TextEdit::singleline(&mut t.name).desired_width(140.0))
                            .lost_focus();
                        changed |= ui.color_edit_button_srgb(&mut t.color.0).changed();
                        let mut text = t
                            .dash_mm
                            .iter()
                            .map(|d| d.to_string())
                            .collect::<Vec<_>>()
                            .join(" ");
                        let r = ui.add(egui::TextEdit::singleline(&mut text).desired_width(90.0));
                        if r.changed() {
                            let parsed: Option<Vec<f32>> =
                                text.split_whitespace().map(|x| x.parse().ok()).collect();
                            if let Some(d) = parsed.filter(|d| d.len() % 2 == 0) {
                                t.dash_mm = d;
                                changed = true;
                            }
                        }
                        changed |= ui
                            .add(egui::DragValue::new(&mut t.pen).range(1..=16))
                            .changed();
                        if ui.small_button("x").clicked() {
                            remove = Some(i);
                        }
                        ui.end_row();
                    }
                });
        });
    // A renamed line type: the kinds of line that used it follow.
    for (i, t) in v.iter().enumerate() {
        if let Some(o) = old.get(i)
            && *o != t.name
        {
            let l = &mut paper.lines;
            for f in [
                &mut l.cut,
                &mut l.seen,
                &mut l.above,
                &mut l.hatch,
                &mut l.axis,
                &mut l.dimension,
                &mut l.frame,
            ] {
                if f == o {
                    *f = t.name.clone();
                }
            }
            for d in &mut paper.dimension_types {
                if d.line_type == *o {
                    d.line_type = t.name.clone();
                }
            }
        }
    }
    let v = &mut paper.line_types;
    if let Some(i) = remove {
        v.remove(i);
        changed = true;
    }
    if ui.button("Add line type").clicked() {
        let mut n = v.len() + 1;
        while v.iter().any(|t| t.name == format!("line_{n}")) {
            n += 1;
        }
        v.push(bimble_core::pens::LineType {
            name: format!("line_{n}"),
            color: Rgb([0, 0, 0]),
            dash_mm: Vec::new(),
            pen: 1,
        });
        changed = true;
    }
    changed
}

/// Which line type each kind of line uses.
fn roles(ui: &mut egui::Ui, p: &mut bimble_core::style::PaperStyle) -> bool {
    let mut changed = false;
    let names: Vec<String> = p.line_types.iter().map(|t| t.name.clone()).collect();
    let l = &mut p.lines;
    egui::Grid::new("line-roles-grid")
        .num_columns(2)
        .show(ui, |ui| {
            for (label, field) in [
                ("Cut outlines", &mut l.cut),
                ("Seen edges", &mut l.seen),
                ("Above the cut", &mut l.above),
                ("Hatches (width)", &mut l.hatch),
                ("Grid axes", &mut l.axis),
                ("Dimensions", &mut l.dimension),
                ("Frame, title block", &mut l.frame),
            ] {
                ui.label(label);
                egui::ComboBox::from_id_salt(("role", label))
                    .selected_text(field.clone())
                    .height(300.0)
                    .show_ui(ui, |ui| {
                        for n in &names {
                            changed |= ui.selectable_value(field, n.clone(), n).changed();
                        }
                    });
                ui.end_row();
            }
        });
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
