//! The material browser (ADR 0031), in the main view: the model's materials on the left, the
//! selected one's identity, properties and graphics on the right, the library below the list.
//!
//! In a `.bimble` model materials are edited by commands (undo, history). An IFC's materials are
//! read only: their properties are those of the library grade their name refers to.

use super::*;
use crate::ui_theme as th;
use bimble_core::library::{MaterialProps, library, lookup, to_material};
use bimble_core::{Material, MaterialCategory};

/// The page's state.
#[derive(Default)]
pub(crate) struct MaterialsPage {
    query: String,
    /// The selected material, by name.
    selected: Option<String>,
    /// The name being typed (applied when the field is left), with the name it started from:
    /// when the material's name changes otherwise (undo), what was typed is dropped.
    name_edit: Option<(String, String)>,
    /// Values being dragged or typed, for the material named (one undo step when done).
    props_edit: Option<(String, MaterialProps)>,
}

/// A material as the page lists it.
struct Row {
    /// In a `.bimble` model: the material itself.
    model: Option<Material>,
    name: String,
    category: MaterialCategory,
    elements: usize,
}

impl Viewer {
    fn material_rows(&self) -> Vec<Row> {
        let Some(l) = &self.loaded else {
            return Vec::new();
        };
        let count = |name: &str| {
            l.scene
                .elements
                .iter()
                .filter(|e| e.material == name)
                .count()
        };
        let mut rows: Vec<Row> = match &l.edit {
            Some(ed) => ed
                .model
                .materials
                .values()
                .map(|m| Row {
                    model: Some(m.clone()),
                    name: m.name.clone(),
                    category: m.category,
                    elements: count(&m.name),
                })
                .collect(),
            None => {
                let mut seen: BTreeMap<String, MaterialCategory> = BTreeMap::new();
                for e in &l.scene.elements {
                    if !e.material.is_empty() {
                        seen.entry(e.material.clone()).or_insert(e.category);
                    }
                }
                seen.into_iter()
                    .map(|(name, category)| Row {
                        model: None,
                        elements: count(&name),
                        name,
                        category,
                    })
                    .collect()
            }
        };
        rows.sort_by_key(|a| a.name.to_lowercase());
        rows
    }

    /// The page in the main view.
    pub(crate) fn materials_ui(&mut self, ui: &mut egui::Ui) {
        let rows = self.material_rows();
        let editable = self.loaded.as_ref().is_some_and(|l| l.edit.is_some());
        let page = self
            .materials_page
            .get_or_insert_with(MaterialsPage::default);
        if page
            .selected
            .as_ref()
            .is_none_or(|s| !rows.iter().any(|r| r.name == *s))
        {
            page.selected = rows.first().map(|r| r.name.clone());
            page.name_edit = None;
        }
        let mut cmds: Vec<bimble_core::Command> = Vec::new();
        let mut done_editing = false;
        let mut open_standards: Option<String> = None;
        let mut select: Option<String> = None;
        let paper = self.opts.style.paper.clone();
        let families = self.opts.style.families;
        // A refused change says why here (a name already taken, a value out of range).
        let refused = self
            .loaded
            .as_ref()
            .and_then(|l| l.edit.as_ref()?.message.clone());
        egui::Frame::new()
            .fill(th::panel_bg())
            .inner_margin(egui::Margin::same(th::GAP_L as i8))
            .show(ui, |ui| {
                ui.set_min_size(ui.available_size());
                ui.heading("Materials");
                ui.weak(if editable {
                    "The model's materials. Changes are commands: Undo takes them back."
                } else {
                    "The IFC's materials, read only. Values come from the library grade their name \
                     refers to."
                });
                if let Some(m) = &refused {
                    ui.colored_label(ui.visuals().warn_fg_color, m);
                }
                ui.add_space(th::GAP_M);
                ui.horizontal_top(|ui| {
                    // ---- left: the list, then the library
                    ui.vertical(|ui| {
                        ui.set_width(270.0);
                        ui.add(
                            egui::TextEdit::singleline(&mut page.query)
                                .hint_text("Find a material")
                                .desired_width(260.0),
                        );
                        let q = page.query.to_lowercase();
                        egui::ScrollArea::vertical()
                            .id_salt("materials-list")
                            .max_height(320.0)
                            .show(ui, |ui| {
                                for r in rows.iter().filter(|r| r.name.to_lowercase().contains(&q))
                                {
                                    let on = page.selected.as_deref() == Some(r.name.as_str());
                                    ui.horizontal(|ui| {
                                        let (rect, _) = ui.allocate_exact_size(
                                            egui::vec2(14.0, 14.0),
                                            egui::Sense::hover(),
                                        );
                                        let c = families.get(r.category).color.0;
                                        ui.painter().rect_filled(
                                            rect,
                                            2.0,
                                            egui::Color32::from_rgb(c[0], c[1], c[2]),
                                        );
                                        if ui
                                            .add(egui::Button::selectable(
                                                on,
                                                format!("{}  ({})", r.name, r.elements),
                                            ))
                                            .clicked()
                                        {
                                            select = Some(r.name.clone());
                                        }
                                    });
                                }
                            });
                        ui.add_space(th::GAP_M);
                        ui.strong("Library");
                        ui.weak("Standard grades with their values (EN / SIA).");
                        egui::ScrollArea::vertical()
                            .id_salt("materials-library")
                            .max_height(260.0)
                            .show(ui, |ui| {
                                for m in library() {
                                    if !m.name.to_lowercase().contains(&q) && !q.is_empty() {
                                        continue;
                                    }
                                    ui.horizontal(|ui| {
                                        ui.label(m.name);
                                        ui.weak(m.category.label());
                                        let have = rows.iter().any(|r| r.name == m.name);
                                        if editable
                                            && ui
                                                .add_enabled(!have, egui::Button::new("Add"))
                                                .on_hover_text(if have {
                                                    "Already in the model"
                                                } else {
                                                    "Add it to the model's materials"
                                                })
                                                .clicked()
                                        {
                                            cmds.push(bimble_core::Command::AddMaterial(
                                                to_material(&m),
                                            ));
                                            select = Some(m.name.to_string());
                                        }
                                    });
                                }
                            });
                    });
                    ui.separator();
                    // ---- right: the selected material
                    ui.vertical(|ui| {
                        let Some(r) = rows
                            .iter()
                            .find(|r| Some(&r.name) == page.selected.as_ref())
                        else {
                            ui.weak("No material.");
                            return;
                        };
                        let lib = lookup(&r.name);
                        ui.heading(&r.name);
                        ui.weak(format!(
                            "{} element{} made of it",
                            r.elements,
                            if r.elements == 1 { "" } else { "s" }
                        ));
                        ui.add_space(th::GAP_M);
                        ui.strong("Identity");
                        egui::Grid::new("material-identity")
                            .num_columns(2)
                            .spacing([12.0, 6.0])
                            .show(ui, |ui| {
                                ui.label("Name");
                                match &r.model {
                                    Some(m) => {
                                        if page
                                            .name_edit
                                            .as_ref()
                                            .is_some_and(|(from, _)| *from != m.name)
                                        {
                                            page.name_edit = None;
                                        }
                                        let (_, text) = page.name_edit.get_or_insert_with(|| {
                                            (m.name.clone(), m.name.clone())
                                        });
                                        let resp = ui.add_sized(
                                            [240.0, 22.0],
                                            egui::TextEdit::singleline(text),
                                        );
                                        if resp.lost_focus() && *text != m.name {
                                            let mut x = m.clone();
                                            x.name = text.trim().to_string();
                                            select = Some(x.name.clone());
                                            cmds.push(bimble_core::Command::SetMaterial(x));
                                        }
                                    }
                                    None => {
                                        ui.label(&r.name);
                                    }
                                }
                                ui.end_row();
                                ui.label("Family");
                                match &r.model {
                                    Some(m) => {
                                        let mut cat = m.category;
                                        egui::ComboBox::from_id_salt("material-family")
                                            .selected_text(cat.label())
                                            .show_ui(ui, |ui| {
                                                for c in MaterialCategory::ALL {
                                                    ui.selectable_value(&mut cat, c, c.label());
                                                }
                                            });
                                        if cat != m.category {
                                            let mut x = m.clone();
                                            x.category = cat;
                                            cmds.push(bimble_core::Command::SetMaterial(x));
                                        }
                                    }
                                    None => {
                                        ui.label(r.category.label());
                                    }
                                }
                                ui.end_row();
                                ui.label("IFC category");
                                ui.label(match r.category.ifc_category() {
                                    "" => "(none)",
                                    c => c,
                                });
                                ui.end_row();
                            });
                        ui.add_space(th::GAP_M);
                        ui.strong("Properties");
                        let props = r
                            .model
                            .as_ref()
                            .map(|m| m.props.clone())
                            .filter(|p| !p.is_empty())
                            .or_else(|| lib.as_ref().map(|l| l.props.clone()))
                            .unwrap_or_default();
                        let from_library =
                            r.model.as_ref().is_none_or(|m| m.props.is_empty()) && lib.is_some();
                        let symbol = lib.as_ref().map_or("f", |l| l.strength_symbol);
                        let mut edited = match &page.props_edit {
                            Some((n, p)) if *n == r.name => p.clone(),
                            _ => props.clone(),
                        };
                        egui::Grid::new("material-props")
                            .num_columns(2)
                            .spacing([12.0, 6.0])
                            .show(ui, |ui| {
                                let mut num =
                                    |ui: &mut egui::Ui,
                                     label: &str,
                                     v: &mut Option<f64>,
                                     unit: &str| {
                                        ui.label(label);
                                        match (editable, v.as_mut()) {
                                            (true, Some(x)) => {
                                                let resp = ui.add(
                                                    egui::DragValue::new(x)
                                                        .speed(1.0)
                                                        .range(0.001..=1_000_000.0)
                                                        .suffix(format!(" {unit}")),
                                                );
                                                done_editing |=
                                                    resp.drag_stopped() || resp.lost_focus();
                                            }
                                            (true, None) => {
                                                if ui.small_button("Set").clicked() {
                                                    *v = Some(1.0);
                                                    done_editing = true;
                                                }
                                            }
                                            (false, Some(x)) => {
                                                ui.label(format!("{x} {unit}"));
                                            }
                                            (false, None) => {
                                                ui.weak("unknown");
                                            }
                                        }
                                        ui.end_row();
                                    };
                                num(ui, "Density", &mut edited.density_kg_m3, "kg/m³");
                                num(ui, "Modulus E", &mut edited.e_mpa, "MPa");
                                num(
                                    ui,
                                    &format!("Strength {symbol}"),
                                    &mut edited.strength_mpa,
                                    "MPa",
                                );
                                ui.label("Source");
                                ui.label(if edited.source.is_empty() {
                                    "—"
                                } else {
                                    edited.source.as_str()
                                });
                                ui.end_row();
                            });
                        if from_library {
                            ui.weak(format!(
                                "From the library grade {}.",
                                lib.as_ref().map_or("", |l| l.name)
                            ));
                        }
                        if let Some(m) = &r.model {
                            // Held while dragging or typing; one command when done.
                            if edited != props {
                                if done_editing {
                                    let mut x = m.clone();
                                    x.props = edited.clone();
                                    if from_library {
                                        x.props.source = format!("{} (changed)", x.props.source);
                                    }
                                    cmds.push(bimble_core::Command::SetMaterial(x));
                                    page.props_edit = None;
                                } else {
                                    page.props_edit = Some((r.name.clone(), edited.clone()));
                                }
                            }
                            ui.horizontal(|ui| {
                                if let Some(l) = &lib
                                    && ui
                                        .add_enabled(
                                            m.props != l.props,
                                            egui::Button::new(format!(
                                                "Use the values of {}",
                                                l.name
                                            )),
                                        )
                                        .clicked()
                                {
                                    let mut x = m.clone();
                                    x.props = l.props.clone();
                                    cmds.push(bimble_core::Command::SetMaterial(x));
                                }
                                if ui
                                    .add_enabled(
                                        !m.props.is_empty(),
                                        egui::Button::new("Clear the values"),
                                    )
                                    .clicked()
                                {
                                    let mut x = m.clone();
                                    x.props = MaterialProps::default();
                                    cmds.push(bimble_core::Command::SetMaterial(x));
                                }
                            });
                        }
                        ui.add_space(th::GAP_M);
                        ui.strong("Graphics");
                        ui.weak(format!(
                            "From the standards of the {} family.",
                            r.category.label().to_lowercase()
                        ));
                        egui::Grid::new("material-graphics")
                            .num_columns(2)
                            .spacing([12.0, 6.0])
                            .show(ui, |ui| {
                                ui.label("On screen");
                                let c = families.get(r.category).color.0;
                                let (rect, _) = ui.allocate_exact_size(
                                    egui::vec2(72.0, 22.0),
                                    egui::Sense::hover(),
                                );
                                ui.painter().rect_filled(
                                    rect,
                                    2.0,
                                    egui::Color32::from_rgb(c[0], c[1], c[2]),
                                );
                                ui.end_row();
                                for (label, cut) in [("Cut", true), ("Seen", false)] {
                                    ui.label(label);
                                    match paper.material_hatch(r.category, cut) {
                                        Some(h) => {
                                            ui.horizontal(|ui| {
                                                let (rect, _) = ui.allocate_exact_size(
                                                    egui::vec2(72.0, 40.0),
                                                    egui::Sense::hover(),
                                                );
                                                crate::style_editor::paint_hatch(
                                                    ui.painter(),
                                                    rect,
                                                    h,
                                                );
                                                ui.label(&h.name);
                                            });
                                        }
                                        None => {
                                            ui.weak("the family's own pattern");
                                        }
                                    }
                                    ui.end_row();
                                }
                            });
                        if ui
                            .button("Edit in the standards")
                            .on_hover_text("Opens the Standards panel on this family")
                            .clicked()
                        {
                            open_standards = Some(r.category.label().to_string());
                        }
                    });
                });
            });
        if select.is_some() && cmds.is_empty() {
            // Another material chosen: what was typed for this one is kept first.
            cmds = self.materials_pending();
        }
        if !cmds.is_empty() {
            self.apply_commands(cmds);
            let refused = self
                .loaded
                .as_ref()
                .is_some_and(|l| l.edit.as_ref().is_some_and(|e| e.message.is_some()));
            if let Some(p) = &mut self.materials_page {
                p.name_edit = None;
                p.props_edit = None;
                if refused && select.as_ref() != p.selected.as_ref() {
                    // A rename refused: stay on the material, under its name.
                    select = None;
                }
            }
        }
        if let Some(s) = select
            && let Some(p) = &mut self.materials_page
        {
            p.selected = Some(s);
            p.name_edit = None;
            p.props_edit = None;
        }
        if let Some(q) = open_standards {
            self.standards_open = true;
            self.standards_query = q;
        }
    }
}

impl Viewer {
    /// What was typed or dragged on the page and not stored yet, as commands: a name not yet
    /// confirmed, values held while dragging.
    fn materials_pending(&self) -> Vec<bimble_core::Command> {
        let (Some(p), Some(ed)) = (
            &self.materials_page,
            self.loaded.as_ref().and_then(|l| l.edit.as_ref()),
        ) else {
            return Vec::new();
        };
        let Some(m) = ed
            .model
            .materials
            .values()
            .find(|m| Some(&m.name) == p.selected.as_ref())
        else {
            return Vec::new();
        };
        let mut x = m.clone();
        if let Some((n, props)) = &p.props_edit
            && *n == m.name
        {
            x.props = props.clone();
        }
        if let Some((from, n)) = &p.name_edit
            && *from == m.name
            && !n.trim().is_empty()
        {
            x.name = n.trim().to_string();
        }
        if x == *m {
            Vec::new()
        } else {
            vec![bimble_core::Command::SetMaterial(x)]
        }
    }

    /// Store what the page holds before it goes away (another page, a view, closing).
    pub(crate) fn commit_materials_page(&mut self) {
        let cmds = self.materials_pending();
        if !cmds.is_empty() {
            self.apply_commands(cmds);
        }
        if let Some(p) = &mut self.materials_page {
            p.name_edit = None;
            p.props_edit = None;
        }
    }
}
