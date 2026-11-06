//! Schedules in the main view (ADR 0029): a table of elements and their quantities, grouped by
//! level, sortable by any column, exported as CSV. A row is an element: clicking it selects it.

use super::*;
use crate::ui_theme as th;
use bimble_core::schedule::{ScheduleDef, default_schedules};
use bimble_drawing::schedule::{Cell, Table, schedule, to_csv};

/// An open schedule: its name, its table for a scene revision, how it is sorted.
pub(crate) struct OpenSchedule {
    pub name: String,
    table: Option<(u64, Table)>,
    /// Sorted by this column, ascending or not; none: by level, location and name.
    sort: Option<(usize, bool)>,
}

impl OpenSchedule {
    pub fn new(name: String) -> OpenSchedule {
        OpenSchedule {
            name,
            table: None,
            sort: None,
        }
    }
}

/// The schedule definitions offered (the built-in ones for now).
pub(crate) fn schedule_defs() -> Vec<ScheduleDef> {
    default_schedules()
}

impl Viewer {
    /// How many rows each schedule has, for the project browser (once per scene revision).
    pub(crate) fn schedule_counts(&mut self) -> Vec<(String, usize)> {
        let rev = self.scene_rev;
        if let Some((r, v)) = &self.schedule_counts
            && *r == rev
        {
            return v.clone();
        }
        let Some(l) = &self.loaded else {
            return Vec::new();
        };
        let v: Vec<(String, usize)> = schedule_defs()
            .iter()
            .map(|d| {
                let n = l
                    .scene
                    .elements
                    .iter()
                    .filter(|e| d.lists(&e.class, e.category))
                    .count();
                (d.name.clone(), n)
            })
            .collect();
        self.schedule_counts = Some((rev, v.clone()));
        v
    }

    /// Open a schedule in the main view (a sheet open is closed; the view stays as it was).
    pub(crate) fn open_schedule(&mut self, name: &str) {
        self.close_sheet();
        self.project_page = None;
        self.materials_page = None;
        if self.schedule.as_ref().is_some_and(|s| s.name == name) {
            return;
        }
        self.schedule = Some(OpenSchedule::new(name.to_string()));
    }

    /// The open schedule's table, measured again when the model changed.
    fn schedule_table(&mut self) -> Option<Table> {
        let rev = self.scene_rev;
        let l = self.loaded.as_ref()?;
        let s = self.schedule.as_mut()?;
        if s.table.as_ref().is_none_or(|(r, _)| *r != rev) {
            let def = schedule_defs().into_iter().find(|d| d.name == s.name)?;
            s.table = Some((rev, schedule(&l.scene, &def)));
        }
        let (_, t) = s.table.as_ref()?;
        let mut t = t.clone();
        if let Some((col, asc)) = s.sort {
            for g in &mut t.groups {
                g.rows.sort_by(|a, b| {
                    let o = a.cells[col]
                        .sort_key()
                        .partial_cmp(&b.cells[col].sort_key())
                        .unwrap_or(std::cmp::Ordering::Equal);
                    if asc { o } else { o.reverse() }
                });
            }
        }
        Some(t)
    }

    /// The schedule in the main view.
    pub(crate) fn schedule_ui(&mut self, ui: &mut egui::Ui) {
        let Some(t) = self.schedule_table() else {
            ui.label("This schedule can't be drawn.");
            return;
        };
        let sort = self.schedule.as_ref().and_then(|s| s.sort);
        let mut new_sort = None;
        let mut pick = None;
        let mut export = false;
        let mut show_in_view = false;
        egui::Frame::new()
            .fill(th::PANEL_BG)
            .inner_margin(egui::Margin::same(th::GAP_L as i8))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.heading(&t.name);
                    ui.label(
                        egui::RichText::new(format!(
                            "{} element{}",
                            t.count,
                            if t.count == 1 { "" } else { "s" }
                        ))
                        .color(th::TEXT_MUTED),
                    );
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        export = ui
                            .button("Export CSV…")
                            .on_hover_text("For a spreadsheet: rows, subtotals and the total")
                            .clicked();
                        show_in_view = ui
                            .add_enabled(
                                self.selected.is_some(),
                                egui::Button::new("Show in the view"),
                            )
                            .on_hover_text("Back to the view, framed on the selected element")
                            .clicked();
                        if sort.is_some() && ui.button("Default order").clicked() {
                            new_sort = Some(None);
                        }
                    });
                });
                ui.weak(
                    "Measured from the geometry. Click a column's title to sort by it, a row to \
                     select its element.",
                );
                ui.add_space(th::GAP_S);
                egui::ScrollArea::both()
                    .id_salt("schedule-scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        egui::Grid::new(("schedule", &t.name))
                            .striped(true)
                            .spacing([16.0, 4.0])
                            .min_col_width(40.0)
                            .show(ui, |ui| {
                                for (k, f) in t.fields.iter().enumerate() {
                                    let mark = match sort {
                                        Some((c, true)) if c == k => " ▲",
                                        Some((c, false)) if c == k => " ▼",
                                        _ => "",
                                    };
                                    if ui
                                        .add(
                                            egui::Button::new(
                                                egui::RichText::new(format!("{}{mark}", f.title()))
                                                    .strong(),
                                            )
                                            .frame(false),
                                        )
                                        .on_hover_text("Sort by this column; again: the other way")
                                        .clicked()
                                    {
                                        new_sort = Some(Some(match sort {
                                            Some((c, asc)) if c == k => (k, !asc),
                                            _ => (k, true),
                                        }));
                                    }
                                }
                                ui.end_row();
                                let cell = |ui: &mut egui::Ui, c: &Cell, strong: bool| {
                                    let text = egui::RichText::new(c.text());
                                    let text = if strong { text.strong() } else { text };
                                    match c {
                                        Cell::Num(..) => {
                                            ui.with_layout(
                                                egui::Layout::right_to_left(egui::Align::Center),
                                                |ui| ui.label(text),
                                            );
                                        }
                                        _ => {
                                            ui.label(text);
                                        }
                                    }
                                };
                                for g in &t.groups {
                                    for r in &g.rows {
                                        let selected = self.selected == Some(r.element);
                                        for (k, c) in r.cells.iter().enumerate() {
                                            if k == 0 {
                                                let resp = ui.add(egui::Button::selectable(
                                                    selected,
                                                    egui::RichText::new(c.text()),
                                                ));
                                                if resp.clicked() {
                                                    pick = Some(r.element);
                                                }
                                            } else {
                                                cell(ui, c, false);
                                            }
                                        }
                                        ui.end_row();
                                    }
                                    if !g.subtotal.is_empty() {
                                        for c in &g.subtotal {
                                            cell(ui, c, true);
                                        }
                                        ui.end_row();
                                    }
                                }
                                if let Some(tot) = &t.total {
                                    for c in tot {
                                        cell(ui, c, true);
                                    }
                                    ui.end_row();
                                }
                            });
                        if t.count == 0 {
                            ui.add_space(th::GAP_M);
                            ui.weak("No element of the model belongs in this schedule.");
                        }
                    });
            });
        if let Some(s) = new_sort
            && let Some(o) = &mut self.schedule
        {
            o.sort = s;
        }
        if let Some(i) = pick {
            self.selected = Some(i);
            self.needs_upload = true;
        }
        if export {
            let name = format!("{}.csv", t.name.to_lowercase().replace(' ', "-"));
            self.view_message = Some(
                match files::save(&name, &[("CSV", &["csv"])], to_csv(&t).as_bytes()) {
                    Ok(Some(to)) => format!("Saved {to}"),
                    Ok(None) => return,
                    Err(e) => e,
                },
            );
        }
        if show_in_view {
            self.schedule = None;
            self.fit_selection(self.last_aspect);
        }
    }
}
