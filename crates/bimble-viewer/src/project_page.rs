//! The project information page (ADR 0030), in the main view: who and what the project is,
//! entered once; every title block's `{key}` takes it. In a `.bimble` model it is a command
//! (undo, history); for an IFC it goes in the side file with the views.

use super::*;
use crate::ui_theme as th;
use bimble_core::project::{KNOWN, key_of};

/// The page's state: what is being typed, and a new field's name.
#[derive(Default)]
pub(crate) struct ProjectPage {
    form: Option<BTreeMap<String, String>>,
    /// The project the form was taken from: when the project changes (undo, another file) the
    /// form follows it; otherwise what was typed is kept until stored.
    base: Option<BTreeMap<String, String>>,
    new_field: String,
    message: Option<String>,
}

impl Viewer {
    /// Store project information: a command in a `.bimble` model, the side file for an IFC.
    fn store_project(&mut self, mut project: BTreeMap<String, String>) -> Result<(), String> {
        // An entry emptied is removed (ADR 0030): a field of one's own too.
        project.retain(|_, v| !v.trim().is_empty());
        bimble_core::project::validate(&project)?;
        if self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) {
            self.apply_commands(vec![bimble_core::Command::SetProject { project }]);
            Ok(())
        } else if let Some(s) = &mut self.side_views {
            s.project = project;
            s.dirty = true;
            Ok(())
        } else {
            Err("Open a model first.".into())
        }
    }

    /// The page in the main view.
    pub(crate) fn project_ui(&mut self, ui: &mut egui::Ui) {
        let current = self.project_info();
        let page = self.project_page.get_or_insert_with(ProjectPage::default);
        // The form follows the project when the project changes (undo, another file). It is not
        // reset merely because no field has the focus: a click elsewhere takes the focus away
        // before the field reports it, and the typed value would be lost.
        if page.form.is_none() || page.base.as_ref() != Some(&current) {
            page.form = Some(current.clone());
            page.base = Some(current.clone());
        }
        let ifc = self.loaded.as_ref().is_some_and(|l| l.edit.is_none());
        let mut commit = false;
        let mut remove: Option<String> = None;
        let mut add: Option<String> = None;
        egui::Frame::new()
            .fill(th::panel_bg())
            .inner_margin(egui::Margin::same(th::GAP_L as i8))
            .show(ui, |ui| {
                ui.set_min_size(ui.available_size());
                ui.heading("Project information");
                ui.weak(if ifc {
                    "Saved with the views next to the IFC (the IFC itself is not changed). \
                     Title blocks write each value where they say {its key}."
                } else {
                    "Saved in the model. Title blocks write each value where they say {its key}."
                });
                if let Some(m) = &page.message {
                    ui.colored_label(ui.visuals().warn_fg_color, m);
                }
                ui.add_space(th::GAP_M);
                let form = page.form.as_mut().expect("set above");
                egui::ScrollArea::vertical()
                    .id_salt("project-scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let mut section = "";
                        let row = |ui: &mut egui::Ui,
                                   label: &str,
                                   key: &str,
                                   form: &mut BTreeMap<String, String>|
                         -> bool {
                            ui.label(label);
                            let mut v = form.get(key).cloned().unwrap_or_default();
                            let r = ui.add_sized([380.0, 22.0], egui::TextEdit::singleline(&mut v));
                            if r.changed() {
                                if v.is_empty() {
                                    form.remove(key);
                                } else {
                                    form.insert(key.to_string(), v);
                                }
                            }
                            let done = r.lost_focus();
                            ui.label(
                                egui::RichText::new(format!("{{{key}}}")).color(th::text_muted()),
                            )
                            .on_hover_text(
                                "Write this in a template's title block to show the value",
                            );
                            ui.end_row();
                            done
                        };
                        egui::Grid::new("project-grid")
                            .num_columns(3)
                            .spacing([16.0, 6.0])
                            .show(ui, |ui| {
                                for (sec, key, label) in KNOWN {
                                    if *sec != section {
                                        if !section.is_empty() {
                                            ui.end_row();
                                        }
                                        section = sec;
                                        ui.strong(*sec);
                                        ui.end_row();
                                    }
                                    commit |= row(ui, label, key, form);
                                }
                                // The fields of this project only.
                                let own: Vec<String> = form
                                    .keys()
                                    .filter(|k| !KNOWN.iter().any(|(_, kk, _)| kk == k))
                                    .cloned()
                                    .collect();
                                ui.end_row();
                                ui.strong("Other");
                                ui.end_row();
                                for k in own {
                                    ui.horizontal(|ui| {
                                        if ui
                                            .small_button("×")
                                            .on_hover_text("Remove this field")
                                            .clicked()
                                        {
                                            remove = Some(k.clone());
                                        }
                                        ui.label(k.replace('_', " "));
                                    });
                                    let mut v = form.get(&k).cloned().unwrap_or_default();
                                    let r = ui.add_sized(
                                        [380.0, 22.0],
                                        egui::TextEdit::singleline(&mut v),
                                    );
                                    if r.changed() {
                                        form.insert(k.clone(), v);
                                    }
                                    commit |= r.lost_focus();
                                    ui.label(
                                        egui::RichText::new(format!("{{{k}}}"))
                                            .color(th::text_muted()),
                                    );
                                    ui.end_row();
                                }
                            });
                        ui.add_space(th::GAP_M);
                        ui.horizontal(|ui| {
                            ui.add(
                                egui::TextEdit::singleline(&mut page.new_field)
                                    .hint_text("A field of your own, e.g. Project manager")
                                    .desired_width(260.0),
                            );
                            let key = key_of(&page.new_field);
                            if ui
                                .add_enabled(!key.is_empty(), egui::Button::new("Add field"))
                                .on_hover_text(format!("Its key: {{{key}}}"))
                                .clicked()
                            {
                                add = Some(key);
                            }
                        });
                    });
            });
        let page = self.project_page.as_mut().expect("set above");
        let form = page.form.as_mut().expect("set above");
        if let Some(k) = remove {
            form.remove(&k);
            commit = true;
        }
        if let Some(k) = add {
            // An empty value would not be kept: the new field starts with a dash to fill in.
            form.entry(k).or_insert_with(|| "-".into());
            page.new_field.clear();
            commit = true;
        }
        if commit {
            self.commit_project_page();
        }
    }

    /// Store what the page's form holds, if it differs from the project (when a field is left,
    /// and before the page goes away: another page, a view, closing).
    pub(crate) fn commit_project_page(&mut self) {
        let current = self.project_info();
        let Some(f) = self.project_page.as_ref().and_then(|p| p.form.clone()) else {
            return;
        };
        {
            let mut kept = f.clone();
            kept.retain(|_, v| !v.trim().is_empty());
            if kept != current {
                let r = self.store_project(f);
                let page = self.project_page.as_mut().expect("set above");
                match r {
                    Ok(()) => {
                        page.message = None;
                        page.form = None;
                        page.base = None;
                    }
                    Err(e) => page.message = Some(e),
                }
            }
        }
    }
}
