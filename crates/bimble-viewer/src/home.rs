//! Home (#52): the start panel, shown when nothing is open and from the house button. Who you
//! are, what to open, and the files opened last (the desktop). Signing in waits for a server
//! (ADR 0038); until then "who you are" is Settings → You.

use crate::Viewer;
use crate::ribbon::Action;
use eframe::egui;

/// Previews' size on Home, pixels (the picture is shrunk to fit, keeping its shape).
const PREVIEW: (u32, u32) = (192, 120);

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
                    let recent = self.settings.recent.clone();
                    egui::Grid::new("recent").striped(true).show(ui, |ui| {
                        for p in &recent {
                            match self.preview(ui.ctx(), p) {
                                Some(t) => {
                                    ui.add(egui::Image::new(&t).max_size(egui::vec2(
                                        PREVIEW.0 as f32 / 2.0,
                                        PREVIEW.1 as f32 / 2.0,
                                    )));
                                }
                                None => {
                                    ui.label("");
                                }
                            }
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
                            ui.weak(meta.map_or_else(String::new, |m| crate::big::size(m.len())));
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

    /// A recent file's preview, read once and kept as a texture.
    fn preview(&mut self, ctx: &egui::Context, file: &str) -> Option<egui::TextureHandle> {
        if let Some(t) = self.previews.get(file) {
            return t.clone();
        }
        let t = crate::settings::thumbnail(file)
            .and_then(|p| bimble_render::read_png(&p, 1024).ok())
            .map(|(w, h, rgba)| {
                ctx.load_texture(
                    format!("preview {file}"),
                    egui::ColorImage::from_rgba_unmultiplied([w as usize, h as usize], &rgba),
                    egui::TextureOptions::LINEAR,
                )
            });
        self.previews.insert(file.to_owned(), t.clone());
        t
    }

    /// Once a model opened on the desktop has drawn its first whole picture: keep a small copy
    /// of it as its preview on Home.
    pub(crate) fn keep_preview(&mut self) {
        let ready = self.texture.is_some()
            && !self.needs_render
            && !self.needs_upload
            && !self.fit_pending
            && self.loading.is_none()
            && !self.lean_shown;
        if !ready {
            return;
        }
        let Some(file) = self.preview_for.take() else {
            return;
        };
        let (Some(target), Some(dest)) = (&self.target, crate::settings::thumbnail(&file)) else {
            return;
        };
        let (w, h) = target.size;
        let Ok(rgba) = bimble_render::read_texture(&self.device, &self.queue, &target.color) else {
            return;
        };
        let (sw, sh, small) = shrink(&rgba, w, h, PREVIEW);
        let saved = dest
            .parent()
            .map_or(Ok(()), std::fs::create_dir_all)
            .and_then(|()| bimble_render::write_png(&dest, sw, sh, &small));
        if let Err(e) = saved {
            log::warn!("preview of {file}: {e}");
        }
        self.previews.remove(&file);
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

/// A picture shrunk to fit `max`, keeping its shape: each pixel the mean of the ones it covers.
fn shrink(rgba: &[u8], w: u32, h: u32, max: (u32, u32)) -> (u32, u32, Vec<u8>) {
    let k = (w as f64 / max.0 as f64)
        .max(h as f64 / max.1 as f64)
        .max(1.0);
    let (sw, sh) = (
        ((w as f64 / k) as u32).max(1),
        ((h as f64 / k) as u32).max(1),
    );
    let mut out = Vec::with_capacity((sw * sh * 4) as usize);
    for y in 0..sh {
        let (y0, y1) = (
            (y as f64 * k) as u32,
            (((y + 1) as f64 * k) as u32).min(h).max(1),
        );
        for x in 0..sw {
            let (x0, x1) = (
                (x as f64 * k) as u32,
                (((x + 1) as f64 * k) as u32).min(w).max(1),
            );
            let mut sum = [0u64; 4];
            let mut n = 0u64;
            for yy in y0..y1.max(y0 + 1).min(h) {
                for xx in x0..x1.max(x0 + 1).min(w) {
                    let i = ((yy * w + xx) * 4) as usize;
                    for c in 0..4 {
                        sum[c] += rgba[i + c] as u64;
                    }
                    n += 1;
                }
            }
            out.extend(sum.map(|s| (s / n.max(1)) as u8));
        }
    }
    (sw, sh, out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_picture_shrinks_to_fit_keeping_its_shape_and_colour() {
        let (w, h) = (1000u32, 500u32);
        let mut rgba = Vec::new();
        for _ in 0..w * h {
            rgba.extend([10, 200, 30, 255]);
        }
        let (sw, sh, small) = shrink(&rgba, w, h, PREVIEW);
        assert_eq!((sw, sh), (192, 96));
        assert_eq!(small.len(), (sw * sh * 4) as usize);
        assert!(small.chunks(4).all(|p| p == [10, 200, 30, 255]));
        // Smaller than the preview: kept as it is.
        assert_eq!(shrink(&rgba[..4 * 50 * 20], 50, 20, PREVIEW).0, 50);
    }
}
