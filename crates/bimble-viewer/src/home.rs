//! Home (#52): the start panel, shown when nothing is open and from the house button. Who you
//! are, what to open, and the files opened last (the desktop). Signing in waits for a server
//! (ADR 0038); until then "who you are" is Settings → You.

use crate::Viewer;
use crate::ribbon::Action;
use eframe::egui;

impl Viewer {
    pub(crate) fn home_ui(&mut self, ui: &mut egui::Ui) {
        let mut run = None;
        let mut open: Option<std::path::PathBuf> = None;
        // The main view has no frame of its own: the theme's panel colour behind Home.
        ui.painter()
            .rect_filled(ui.max_rect(), 0.0, ui.visuals().panel_fill);
        egui::ScrollArea::vertical().show(ui, |ui| {
            ui.add_space(24.0);
            ui.horizontal(|ui| {
                ui.add_space(24.0);
                ui.vertical(|ui| {
                    ui.heading(format!("Bimble {}", env!("CARGO_PKG_VERSION")));
                    ui.horizontal(|ui| {
                        ui.label(format!("You: {}", self.settings.author()));
                        if ui.link("change").clicked() {
                            run = Some(Action::Settings);
                        }
                    });
                    if let Some(e) = &self.error {
                        ui.colored_label(egui::Color32::RED, e);
                    }
                    ui.add_space(12.0);
                    ui.horizontal(|ui| {
                        for a in [Action::Open, Action::ImportIfc] {
                            if ui.button(a.label()).clicked() {
                                run = Some(a);
                            }
                        }
                        if self.loaded.is_some() && ui.button("Back to the model").clicked() {
                            self.home_open = false;
                        }
                    });
                    ui.weak("Or drop an IFC, .bimble or .bimbleview file on this window.");
                    ui.add_space(16.0);
                    if cfg!(target_arch = "wasm32") {
                        ui.weak(
                            "Files you open stay on your computer. The desktop version lists the \
                             files opened last here.",
                        );
                        return;
                    }
                    ui.strong("Recent files");
                    if self.settings.recent.is_empty() {
                        ui.weak("None yet.");
                    }
                    egui::Grid::new("recent").striped(true).show(ui, |ui| {
                        for p in &self.settings.recent {
                            let path = std::path::Path::new(p);
                            let name = path
                                .file_name()
                                .map(|n| n.to_string_lossy().into_owned())
                                .unwrap_or_else(|| p.clone());
                            let meta = std::fs::metadata(path).ok();
                            let b = ui.add_enabled(meta.is_some(), egui::Button::new(&name));
                            if b.on_disabled_hover_text("Not found: moved or deleted")
                                .clicked()
                            {
                                open = Some(path.to_path_buf());
                            }
                            ui.weak(
                                path.parent()
                                    .map(|d| d.display().to_string())
                                    .unwrap_or_default(),
                            );
                            ui.weak(meta.map_or_else(String::new, |m| crate::big::gb(m.len())));
                            ui.end_row();
                        }
                    });
                });
            });
        });
        if let Some(a) = run {
            let ctx = ui.ctx().clone();
            self.run_action(a, &ctx);
        }
        if let Some(p) = open {
            let ctx = ui.ctx().clone();
            self.open_recent(p, &ctx);
        }
    }

    /// As if picked with Open…: unsaved work is asked about first.
    fn open_recent(&mut self, p: std::path::PathBuf, ctx: &egui::Context) {
        self.home_open = false;
        let _ = self.files.sender().send(crate::files::Picked {
            purpose: crate::files::Purpose::Open,
            name: p
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
            path: Some(p),
            bytes: None,
        });
        ctx.request_repaint();
    }
}
