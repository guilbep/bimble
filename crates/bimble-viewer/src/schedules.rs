//! Schedules in the main view (ADR 0029): a table of elements and their quantities, grouped by
//! level, sortable by any column, exported as CSV. A row is an element: clicking it selects it.

use super::*;
use crate::ui_theme as th;
use bimble_core::schedule::{Field, ScheduleDef, effective, is_builtin};
use bimble_drawing::schedule::{Cell, Table, schedule, to_csv};

/// An open schedule: its name, its table for a scene revision, how it is sorted.
pub(crate) struct OpenSchedule {
    pub name: String,
    /// The table, for a scene revision and a definition.
    table: Option<((u64, String), Table)>,
    /// Sorted by this field, ascending or not; none: by level, location and name. A field, not
    /// a column index: columns can be removed or moved while the schedule is open.
    sort: Option<(Field, bool)>,
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

impl Viewer {
    /// The schedules this model keeps (new ones and built-in ones changed).
    pub(crate) fn stored_schedules(&self) -> Vec<ScheduleDef> {
        if let Some(ed) = self.loaded.as_ref().and_then(|l| l.edit.as_ref()) {
            return ed.model.schedules.clone();
        }
        self.side_views
            .as_ref()
            .map(|s| s.schedules.clone())
            .unwrap_or_default()
    }

    /// The schedules offered: built-in ones (as changed) and the model's own.
    pub(crate) fn schedule_defs(&self) -> Vec<ScheduleDef> {
        effective(&self.stored_schedules())
    }

    /// Store the model's schedules: a command in a `.bimble` model, the side file for an IFC.
    fn store_schedules(&mut self, v: Vec<ScheduleDef>) -> Result<(), String> {
        bimble_core::schedule::validate_all(&v)?;
        if self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) {
            self.apply_commands(vec![bimble_core::Command::SetSchedules { schedules: v }]);
            Ok(())
        } else if let Some(s) = &mut self.side_views {
            s.schedules = v;
            s.dirty = true;
            self.schedule_counts = None;
            Ok(())
        } else {
            Err("Open a model first.".into())
        }
    }

    /// Change one schedule (by its current name): stored under its (maybe new) name.
    fn change_schedule(&mut self, old_name: &str, d: ScheduleDef) -> Result<(), String> {
        let mut v = self.stored_schedules();
        match v.iter_mut().find(|s| s.name == old_name) {
            Some(s) => *s = d.clone(),
            None => v.push(d.clone()),
        }
        self.store_schedules(v)?;
        if let Some(o) = &mut self.schedule {
            o.name = d.name;
        }
        Ok(())
    }

    /// A new schedule of the model, listing every element, open.
    pub(crate) fn new_schedule(&mut self) {
        let defs = self.schedule_defs();
        let mut n = 1;
        let name = loop {
            let c = format!("Schedule {n}");
            if !defs.iter().any(|d| d.name.eq_ignore_ascii_case(&c)) {
                break c;
            }
            n += 1;
        };
        let d = ScheduleDef {
            name: name.clone(),
            classes: Vec::new(),
            family: None,
            fields: vec![
                Field::Level,
                Field::Name,
                Field::Class,
                Field::Material,
                Field::Volume,
            ],
            group_by_level: true,
            totals: true,
        };
        let mut v = self.stored_schedules();
        v.push(d);
        match self.store_schedules(v) {
            Ok(()) => self.open_schedule(&name),
            Err(e) => self.view_message = Some(e),
        }
    }

    /// How many rows each schedule has, for the project browser (once per scene revision).
    pub(crate) fn schedule_counts(&mut self) -> Vec<(String, usize)> {
        let defs = self.schedule_defs();
        let rev = self.scene_rev ^ hash_text(&serde_json::to_string(&defs).unwrap_or_default());
        if let Some((r, v)) = &self.schedule_counts
            && *r == rev
        {
            return v.clone();
        }
        let Some(l) = &self.loaded else {
            return Vec::new();
        };
        let v: Vec<(String, usize)> = defs
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
        self.commit_pages();
        self.home_open = false;
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
        let name = self.schedule.as_ref()?.name.clone();
        let def = self.schedule_defs().into_iter().find(|d| d.name == name)?;
        let key = (rev, serde_json::to_string(&def).unwrap_or_default());
        let l = self.loaded.as_ref()?;
        let s = self.schedule.as_mut()?;
        if s.table.as_ref().is_none_or(|(k, _)| *k != key) {
            s.table = Some((key, schedule(&l.scene, &def)));
        }
        let (_, t) = s.table.as_ref()?;
        let mut t = t.clone();
        // A column gone: back to the default order, which the header then shows.
        if let Some(sort) = s.sort
            && !sort_rows(&mut t, sort)
        {
            s.sort = None;
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
            .fill(th::panel_bg())
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
                        .color(th::text_muted()),
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
                                for f in &t.fields {
                                    let mark = match sort {
                                        Some((c, true)) if c == *f => " (up)",
                                        Some((c, false)) if c == *f => " (down)",
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
                                            Some((c, asc)) if c == *f => (*f, !asc),
                                            _ => (*f, true),
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

/// A short hash of a text (cache keys).
fn hash_text(s: &str) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    s.hash(&mut h);
    h.finish()
}

impl Viewer {
    /// The open schedule's settings, where the properties are (Revit's Fields, Filter, Sort and
    /// group): name, what it lists, its columns in order, grouping and totals (ADR 0029).
    pub(crate) fn schedule_properties(&mut self, ui: &mut egui::Ui) {
        let Some(name) = self.schedule.as_ref().map(|s| s.name.clone()) else {
            return;
        };
        let Some(def) = self.schedule_defs().into_iter().find(|d| d.name == name) else {
            return;
        };
        let builtin = is_builtin(&name);
        let changed_builtin = builtin
            && self
                .stored_schedules()
                .iter()
                .any(|s| s.name.eq_ignore_ascii_case(&name));
        let mut d = def.clone();
        ui.heading("Schedule");
        if builtin {
            ui.weak(if changed_builtin {
                "A built-in schedule, changed for this model."
            } else {
                "A built-in schedule. Changes are kept with this model."
            });
        }
        let mut rename: Option<String> = None;
        egui::Grid::new("schedule-props")
            .num_columns(2)
            .spacing([10.0, 6.0])
            .show(ui, |ui| {
                ui.label("Name");
                let id = egui::Id::unique(("schedule-name", &name));
                let mut text: String = ui.data(|m| m.get_temp(id)).unwrap_or_else(|| name.clone());
                let r = ui.add_enabled(
                    !builtin,
                    egui::TextEdit::singleline(&mut text).desired_width(180.0),
                );
                if r.lost_focus() && text.trim() != name && !text.trim().is_empty() {
                    rename = Some(text.trim().to_string());
                }
                if r.has_focus() {
                    ui.data_mut(|m| m.insert_temp(id, text));
                } else {
                    ui.data_mut(|m| m.remove::<String>(id));
                }
                ui.end_row();
                ui.label("IFC classes");
                // Held while typed; applied when the field is left.
                let cid = egui::Id::unique(("schedule-classes", &name));
                let mut classes: String = ui
                    .data(|m| m.get_temp(cid))
                    .unwrap_or_else(|| d.classes.join(", "));
                let r = ui
                    .add(egui::TextEdit::singleline(&mut classes).desired_width(180.0))
                    .on_hover_text("Separated by commas; empty: every class");
                if r.lost_focus() {
                    d.classes = classes
                        .split(',')
                        .map(|c| c.trim().to_string())
                        .filter(|c| !c.is_empty())
                        .collect();
                }
                if r.has_focus() {
                    ui.data_mut(|m| m.insert_temp(cid, classes));
                } else {
                    ui.data_mut(|m| m.remove::<String>(cid));
                }
                ui.end_row();
                ui.label("Family");
                egui::ComboBox::from_id_salt("schedule-family")
                    .selected_text(d.family.map_or("Any", |f| f.label()))
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut d.family, None, "Any");
                        for c in bimble_core::MaterialCategory::ALL {
                            ui.selectable_value(&mut d.family, Some(c), c.label());
                        }
                    });
                ui.end_row();
            });
        ui.checkbox(&mut d.group_by_level, "Group by level, with subtotals");
        ui.checkbox(&mut d.totals, "Total at the end");
        ui.add_space(4.0);
        ui.strong("Columns");
        ui.weak("Ticked columns show, in this order.");
        let mut order: Vec<(Field, bool)> = d.fields.iter().map(|f| (*f, true)).collect();
        for f in Field::ALL {
            if !d.fields.contains(&f) {
                order.push((f, false));
            }
        }
        let mut swap: Option<usize> = None;
        for (k, (f, on)) in order.iter_mut().enumerate() {
            ui.horizontal(|ui| {
                ui.checkbox(on, f.title());
                if *on
                    && k > 0
                    && ui
                        .small_button("Earlier")
                        .on_hover_text("One column to the left")
                        .clicked()
                {
                    swap = Some(k);
                }
            });
        }
        if let Some(k) = swap {
            order.swap(k - 1, k);
        }
        let fields: Vec<Field> = order
            .iter()
            .filter(|(_, on)| *on)
            .map(|(f, _)| *f)
            .collect();
        if !fields.is_empty() {
            d.fields = fields;
        }
        ui.add_space(6.0);
        let mut delete = false;
        let mut reset = false;
        let mut duplicate = false;
        ui.horizontal(|ui| {
            duplicate = ui.button("Duplicate").clicked();
            if builtin {
                reset = ui
                    .add_enabled(changed_builtin, egui::Button::new("Back to built-in"))
                    .on_hover_text("Forget this model's changes to it")
                    .clicked();
            } else {
                delete = ui.button("Delete").clicked();
            }
        });
        if let Some(n) = rename {
            if self
                .schedule_defs()
                .iter()
                .any(|s| s.name.eq_ignore_ascii_case(&n))
            {
                self.view_message = Some(format!("A schedule is already named {n}."));
            } else {
                d.name = n;
            }
        }
        let r = if delete || reset {
            let v: Vec<ScheduleDef> = self
                .stored_schedules()
                .into_iter()
                .filter(|s| !s.name.eq_ignore_ascii_case(&name))
                .collect();
            let r = self.store_schedules(v);
            if delete {
                self.schedule = None;
            }
            r
        } else if duplicate {
            let defs = self.schedule_defs();
            let mut n = 2;
            let copy = loop {
                let c = format!("{name} ({n})");
                if !defs.iter().any(|s| s.name.eq_ignore_ascii_case(&c)) {
                    break c;
                }
                n += 1;
            };
            let mut c = d.clone();
            c.name = copy.clone();
            let mut v = self.stored_schedules();
            v.push(c);
            let r = self.store_schedules(v);
            if r.is_ok() {
                self.open_schedule(&copy);
            }
            r
        } else if d != def {
            self.change_schedule(&name, d)
        } else {
            Ok(())
        };
        if let Err(e) = r {
            self.view_message = Some(e);
        }
    }
}

/// Sort each group's rows by a field; false (rows unchanged) when the table has no such column.
fn sort_rows(t: &mut Table, (field, asc): (Field, bool)) -> bool {
    let Some(col) = t.fields.iter().position(|f| *f == field) else {
        return false;
    };
    for g in &mut t.groups {
        g.rows.sort_by(|a, b| {
            let (Some(x), Some(y)) = (a.cells.get(col), b.cells.get(col)) else {
                return std::cmp::Ordering::Equal;
            };
            let o = x
                .sort_key()
                .partial_cmp(&y.sort_key())
                .unwrap_or(std::cmp::Ordering::Equal);
            if asc { o } else { o.reverse() }
        });
    }
    true
}

/// After undo or redo removed the open schedule's name: the one name that appeared, if exactly one
/// did (a rename undone), else none (the schedule itself was undone).
pub(crate) fn follow_rename(before: &[String], after: &[String]) -> Option<String> {
    let mut new = after.iter().filter(|n| !before.contains(n));
    match (new.next(), new.next()) {
        (Some(n), None) => Some(n.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn undo_follows_a_rename_or_closes() {
        let v = |x: &[&str]| x.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        // "Schedule 1" renamed "Piles", undone: back to "Schedule 1".
        assert_eq!(
            follow_rename(&v(&["Slabs", "Piles"]), &v(&["Slabs", "Schedule 1"])),
            Some("Schedule 1".into())
        );
        // "New schedule" undone: nothing to follow.
        assert_eq!(
            follow_rename(&v(&["Slabs", "Schedule 1"]), &v(&["Slabs"])),
            None
        );
    }
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use bimble_core::schedule::default_schedules;

    /// Sorted by its last column, then that column removed (or undone): the old index pointed past
    /// the row's cells and panicked. A field that is gone leaves the order as it is.
    #[test]
    fn sort_by_a_removed_column_is_ignored() {
        let scene = bimble_geom::Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut def = default_schedules()
            .into_iter()
            .find(|d| d.name == "Concrete beams")
            .unwrap();
        let last = *def.fields.last().unwrap();
        let mut t = schedule(&scene, &def);
        assert!(sort_rows(&mut t, (last, false)));
        def.fields.pop();
        let mut t = schedule(&scene, &def);
        let before: Vec<usize> = t
            .groups
            .iter()
            .flat_map(|g| g.rows.iter().map(|r| r.element))
            .collect();
        assert!(!sort_rows(&mut t, (last, true)));
        let after: Vec<usize> = t
            .groups
            .iter()
            .flat_map(|g| g.rows.iter().map(|r| r.element))
            .collect();
        assert_eq!(before, after);
    }
}
