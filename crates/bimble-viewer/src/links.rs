//! Manage links (#58): every file linked to the model, grouped by type: models (IFC, .bimble,
//! .bimbleview) and the plans placed in views (DXF, PDF). Each can be deleted, reloaded from
//! where it was read (on the desktop), or reloaded from another file.

use crate::{Viewer, files};
use bimble_core::GlobalId;
use eframe::egui;
use std::path::PathBuf;

/// One row of the window.
struct Row {
    name: String,
    /// Where it shows: "all views" for a model, the view's name for a plan.
    place: String,
    /// What it can be reloaded from, if anything (a path on the desktop).
    source: Option<PathBuf>,
    what: Which,
}

#[derive(Clone, Copy)]
enum Which {
    Model(usize),
    Plan {
        view: GlobalId,
        id: GlobalId,
        pdf: bool,
    },
}

#[derive(Clone, Copy)]
enum Do {
    Delete(Which),
    Reload(Which),
    ReloadFrom(Which),
}

impl Viewer {
    fn link_rows(&self) -> [(&'static str, Vec<Row>); 3] {
        let models = self
            .links
            .iter()
            .enumerate()
            .map(|(i, l)| Row {
                name: l.name(),
                place: "all views".into(),
                source: (cfg!(not(target_arch = "wasm32")) && l.path.is_absolute())
                    .then(|| l.path.clone()),
                what: Which::Model(i),
            })
            .collect();
        let plans = |pdf: bool| {
            self.views
                .iter()
                .flat_map(|v| {
                    v.references
                        .iter()
                        .filter(move |r| (r.format == "pdf") == pdf)
                        .map(move |r| Row {
                            name: r.name.clone(),
                            place: v.name.clone(),
                            // Reload only from a file there on this computer (the path was
                            // written on whichever computer added it).
                            source: r
                                .source
                                .as_ref()
                                .filter(|_| cfg!(not(target_arch = "wasm32")))
                                .map(PathBuf::from)
                                .filter(|p| p.is_file()),
                            what: Which::Plan {
                                view: v.id,
                                id: r.id,
                                pdf,
                            },
                        })
                })
                .collect()
        };
        [
            ("IFC and Bimble models", models),
            ("DXF plans", plans(false)),
            ("PDF plans", plans(true)),
        ]
    }

    pub(crate) fn links_ui(&mut self, ctx: &egui::Context) {
        if !self.links_window {
            return;
        }
        let groups = self.link_rows();
        let mut open = true;
        let mut act = None;
        egui::Window::new("Manage links")
            .open(&mut open)
            .collapsible(false)
            .default_width(460.0)
            .show(ctx, |ui| {
                for (title, rows) in &groups {
                    ui.strong(*title);
                    if rows.is_empty() {
                        ui.weak("None.");
                    }
                    egui::Grid::new(*title).striped(true).show(ui, |ui| {
                        for r in rows {
                            ui.label(&r.name);
                            ui.weak(&r.place);
                            let reload =
                                ui.add_enabled(r.source.is_some(), egui::Button::new("Reload"));
                            let reload = match &r.source {
                                Some(p) => {
                                    reload.on_hover_text(format!("Read {} again", p.display()))
                                }
                                None => reload.on_disabled_hover_text(
                                    "Where it was read from isn't known here (a browser, or an \
                                     older file): use Reload from…",
                                ),
                            };
                            if reload.clicked() {
                                act = Some(Do::Reload(r.what));
                            }
                            if ui
                                .button("Reload from…")
                                .on_hover_text("Replace it with another file, kept where it is")
                                .clicked()
                            {
                                act = Some(Do::ReloadFrom(r.what));
                            }
                            if ui.button("Delete").clicked() {
                                act = Some(Do::Delete(r.what));
                            }
                            ui.end_row();
                        }
                    });
                    ui.add_space(8.0);
                }
            });
        self.links_window = open;
        let Some(act) = act else { return };
        let source = |w: Which| {
            groups
                .iter()
                .flat_map(|(_, rows)| rows)
                .find(|r| match (r.what, w) {
                    (Which::Model(a), Which::Model(b)) => a == b,
                    (Which::Plan { id: a, .. }, Which::Plan { id: b, .. }) => a == b,
                    _ => false,
                })
                .and_then(|r| r.source.clone())
        };
        match act {
            Do::Delete(Which::Model(i)) => self.unlink(i),
            Do::Reload(w @ Which::Model(i)) => {
                if let Some(p) = source(w) {
                    self.relink = Some(i);
                    self.link(p, ctx);
                }
            }
            Do::ReloadFrom(Which::Model(i)) => {
                self.relink = Some(i);
                self.files.pick(files::Purpose::Link, ctx);
            }
            Do::Delete(Which::Plan { view, id, .. }) => {
                if let Some(mut v) = self.views.iter().find(|v| v.id == view).cloned() {
                    v.references.retain(|r| r.id != id);
                    if self.moving_ref == Some(id) {
                        self.moving_ref = None;
                    }
                    match self.store_view(v) {
                        Ok(()) => self.view_form = None,
                        Err(e) => self.view_message = Some(e),
                    }
                }
            }
            Do::Reload(w @ Which::Plan { view, id, pdf }) => {
                if let Some(path) = source(w) {
                    self.reference_view = Some(view);
                    self.reference_replace = Some(id);
                    let name = path
                        .file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default();
                    self.add_reference(files::Picked {
                        purpose: if pdf {
                            files::Purpose::ReferencePdf
                        } else {
                            files::Purpose::ReferenceDxf
                        },
                        name,
                        path: Some(path),
                        bytes: None,
                    });
                }
            }
            Do::ReloadFrom(Which::Plan { view, id, pdf }) => {
                self.reference_view = Some(view);
                self.reference_replace = Some(id);
                self.files.pick(
                    if pdf {
                        files::Purpose::ReferencePdf
                    } else {
                        files::Purpose::ReferenceDxf
                    },
                    ctx,
                );
            }
        }
    }
}
