//! Bimble viewer: a window around `bimble-render`.
//!
//! The 3D view is rendered offscreen by the same renderer as `bimble render`, then shown as an
//! egui image. It is only redrawn when something changes (camera, visibility, selection, size),
//! which keeps old laptops cool and quiet.

use bimble_core::MaterialCategory;
use bimble_geom::Scene;
use bimble_render::{Camera, DrawOptions, GpuScene, Renderer, Style, Target, palette};
use eframe::egui::{self, Color32, PointerButton, Sense, Vec2};
use eframe::egui_wgpu;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, channel};
use std::time::{Duration, Instant};

const SAMPLES: u32 = 4;

/// Open the viewer window, optionally loading a file. Blocks until the window is closed.
pub fn run(file: Option<PathBuf>) -> Result<(), String> {
    let options = eframe::NativeOptions {
        renderer: eframe::Renderer::Wgpu,
        viewport: egui::ViewportBuilder::default()
            .with_title("Bimble")
            .with_inner_size([1280.0, 800.0])
            .with_min_inner_size([480.0, 320.0])
            .with_drag_and_drop(true),
        ..Default::default()
    };
    eframe::run_native(
        "Bimble",
        options,
        Box::new(move |cc| {
            let rs = cc
                .wgpu_render_state
                .as_ref()
                .ok_or("the viewer needs the wgpu renderer")?;
            let mut app = Viewer::new(rs);
            if let Some(f) = file {
                app.open(f, &cc.egui_ctx);
            }
            Ok(Box::new(app))
        }),
    )
    .map_err(|e| e.to_string())
}

/// Load a .ifc or .bimble file as a scene.
pub fn load_scene(path: &Path) -> Result<Scene, String> {
    let is_ifc = path
        .extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"));
    if is_ifc {
        bimble_ifc::read::read_file(path).map_err(|e| e.to_string())
    } else {
        let m = bimble_core::file::load(path).map_err(|e| e.to_string())?;
        Ok(Scene::from_model(&m))
    }
}

struct Loading {
    path: PathBuf,
    rx: Receiver<Result<(Scene, Duration), String>>,
}

struct Loaded {
    path: PathBuf,
    scene: Scene,
    load_time: Duration,
    bounds: bimble_render::Sphere,
    classes: BTreeMap<String, usize>,
    storeys: BTreeMap<String, usize>,
    categories: BTreeMap<MaterialCategory, usize>,
}

struct Viewer {
    device: eframe::wgpu::Device,
    queue: eframe::wgpu::Queue,
    egui_renderer: std::sync::Arc<egui::mutex::RwLock<egui_wgpu::Renderer>>,
    adapter: String,
    renderer: Renderer,

    loading: Option<Loading>,
    loaded: Option<Loaded>,
    error: Option<String>,

    gpu_scene: Option<GpuScene>,
    target: Option<Target>,
    texture: Option<egui::TextureId>,
    needs_upload: bool,
    needs_render: bool,
    last_render: Duration,

    camera: Camera,
    opts: DrawOptions,
    hidden_categories: BTreeSet<MaterialCategory>,
    hidden_classes: BTreeSet<String>,
    hidden_storeys: BTreeSet<String>,
    selected: Option<usize>,
    fit_pending: bool,
}

impl Viewer {
    fn new(rs: &egui_wgpu::RenderState) -> Viewer {
        let info = rs.adapter.get_info();
        Viewer {
            device: rs.device.clone(),
            queue: rs.queue.clone(),
            egui_renderer: rs.renderer.clone(),
            adapter: format!("{} ({:?})", info.name, info.backend),
            renderer: Renderer::new(&rs.device, SAMPLES),
            loading: None,
            loaded: None,
            error: None,
            gpu_scene: None,
            target: None,
            texture: None,
            needs_upload: false,
            needs_render: true,
            last_render: Duration::ZERO,
            camera: Camera::default(),
            opts: DrawOptions::default(),
            hidden_categories: BTreeSet::new(),
            hidden_classes: BTreeSet::new(),
            hidden_storeys: BTreeSet::new(),
            selected: None,
            fit_pending: false,
        }
    }

    fn open(&mut self, path: PathBuf, ctx: &egui::Context) {
        let (tx, rx) = channel();
        let p = path.clone();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let r = load_scene(&p).map(|s| (s, t.elapsed()));
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.loading = Some(Loading { path, rx });
        self.error = None;
    }

    fn poll_loading(&mut self) {
        let Some(l) = &self.loading else { return };
        let Ok(result) = l.rx.try_recv() else { return };
        let path = self.loading.take().unwrap().path;
        match result {
            Ok((scene, load_time)) => {
                let mut classes = BTreeMap::new();
                let mut storeys = BTreeMap::new();
                let mut categories = BTreeMap::new();
                for e in &scene.elements {
                    *classes.entry(e.class.clone()).or_default() += 1;
                    *storeys.entry(e.storey.clone()).or_default() += 1;
                    *categories.entry(e.category).or_default() += 1;
                }
                let bounds = bimble_render::Sphere::of(&scene);
                self.loaded = Some(Loaded {
                    path,
                    scene,
                    load_time,
                    bounds,
                    classes,
                    storeys,
                    categories,
                });
                self.hidden_classes.clear();
                self.hidden_storeys.clear();
                self.hidden_categories.clear();
                self.selected = None;
                self.needs_upload = true;
                self.fit_pending = true;
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    fn visible(&self, i: usize) -> bool {
        let Some(l) = &self.loaded else { return false };
        let e = &l.scene.elements[i];
        !self.hidden_categories.contains(&e.category)
            && !self.hidden_classes.contains(&e.class)
            && !self.hidden_storeys.contains(&e.storey)
    }

    fn upload(&mut self) {
        let Some(l) = &self.loaded else { return };
        let style = |i: usize, _: &bimble_geom::SceneElement| {
            if !self.visible(i) {
                Style::Hidden
            } else if self.selected == Some(i) {
                Style::Selected
            } else {
                Style::Normal
            }
        };
        self.gpu_scene = Some(self.renderer.upload(&self.device, &l.scene, style));
        self.needs_upload = false;
        self.needs_render = true;
    }

    fn fit(&mut self, aspect: f32) {
        if let Some(l) = &self.loaded {
            let mut shown = l.scene.clone();
            let keep: Vec<bool> = (0..shown.elements.len()).map(|i| self.visible(i)).collect();
            let mut k = keep.iter();
            shown.elements.retain(|_| *k.next().unwrap());
            if shown.elements.is_empty() {
                shown = l.scene.clone();
            }
            self.camera.fit(&shown, aspect);
            self.needs_render = true;
        }
    }

    /// Make sure the offscreen target matches the view size, and redraw it if needed.
    fn render_view(&mut self, size_px: (u32, u32), ppp: f32) {
        let resized = self.target.as_ref().is_none_or(|t| t.size != size_px);
        if resized {
            let target = Target::new(&self.device, size_px, SAMPLES);
            let mut r = self.egui_renderer.write();
            match self.texture {
                Some(id) => r.update_egui_texture_from_wgpu_texture(
                    &self.device,
                    &target.color_view,
                    eframe::wgpu::FilterMode::Linear,
                    id,
                ),
                None => {
                    self.texture = Some(r.register_native_texture(
                        &self.device,
                        &target.color_view,
                        eframe::wgpu::FilterMode::Linear,
                    ))
                }
            }
            self.target = Some(target);
            self.needs_render = true;
        }
        if self.needs_upload {
            self.upload();
        }
        if !self.needs_render {
            return;
        }
        let (Some(target), Some(gs)) = (&self.target, &self.gpu_scene) else {
            return;
        };
        let t = Instant::now();
        let opts = DrawOptions {
            hatch_scale: ppp,
            ..self.opts
        };
        self.renderer
            .render(&self.device, &self.queue, target, gs, &self.camera, &opts);
        self.last_render = t.elapsed();
        self.needs_render = false;
    }

    fn handle_view_input(&mut self, ui: &egui::Ui, resp: &egui::Response) {
        let rect = resp.rect;
        let aspect = rect.width() / rect.height().max(1.0);
        if resp.dragged_by(PointerButton::Primary) && !ui.input(|i| i.modifiers.shift) {
            let d = resp.drag_delta();
            self.camera.orbit(d.x * 0.008, d.y * 0.008);
            self.needs_render = true;
        } else if resp.dragged_by(PointerButton::Secondary)
            || resp.dragged_by(PointerButton::Middle)
            || (resp.dragged_by(PointerButton::Primary) && ui.input(|i| i.modifiers.shift))
        {
            let d = resp.drag_delta() / rect.height().max(1.0);
            self.camera.pan(d.x, d.y);
            self.needs_render = true;
        }
        if resp.hovered() {
            let (scroll, zoom) = ui.input(|i| (i.smooth_scroll_delta().y, i.zoom_delta()));
            if scroll != 0.0 || zoom != 1.0 {
                self.camera.zoom((-scroll * 0.0015).exp() / zoom);
                self.needs_render = true;
            }
        }
        if resp.double_clicked() {
            self.fit(aspect);
        } else if resp.clicked()
            && let (Some(pos), Some(l)) = (resp.interact_pointer_pos(), &self.loaded)
        {
            let ndc = [
                (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
                1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
            ];
            let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
            let hit = bimble_render::pick(&l.scene, |i| self.visible(i), o, d).map(|(i, _)| i);
            if hit != self.selected {
                self.selected = hit;
                self.needs_upload = true;
            }
        }
    }

    fn handle_keys(&mut self, ctx: &egui::Context, aspect: f32) {
        if ctx.egui_wants_keyboard_input() {
            return;
        }
        let (f, o, e, h, t, esc) = ctx.input(|i| {
            (
                i.key_pressed(egui::Key::F),
                i.key_pressed(egui::Key::O),
                i.key_pressed(egui::Key::E),
                i.key_pressed(egui::Key::H),
                i.key_pressed(egui::Key::T),
                i.key_pressed(egui::Key::Escape),
            )
        });
        if f {
            self.fit(aspect);
        }
        if o {
            self.camera.orthographic = !self.camera.orthographic;
            self.needs_render = true;
        }
        if e {
            self.opts.edges = !self.opts.edges;
            self.needs_render = true;
        }
        if h {
            self.opts.hatch_strength = if self.opts.hatch_strength > 0.0 {
                0.0
            } else {
                0.5
            };
            self.needs_render = true;
        }
        if t {
            self.camera.pitch = 1.5;
            self.camera.yaw = -std::f32::consts::FRAC_PI_2;
            self.needs_render = true;
        }
        if esc && self.selected.take().is_some() {
            self.needs_upload = true;
        }
    }

    fn top_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui.button("Open…").clicked()
                && let Some(p) = rfd::FileDialog::new()
                    .add_filter("Building models", &["ifc", "IFC", "bimble"])
                    .pick_file()
            {
                self.open(p, ui.ctx());
            }
            ui.separator();
            let mut changed = false;
            changed |= ui.checkbox(&mut self.opts.edges, "Edges (E)").changed();
            let mut hatch = self.opts.hatch_strength > 0.0;
            if ui.checkbox(&mut hatch, "Hatching (H)").changed() {
                self.opts.hatch_strength = if hatch { 0.5 } else { 0.0 };
                changed = true;
            }
            changed |= ui.checkbox(&mut self.camera.orthographic, "Orthographic (O)").changed();
            if changed {
                self.needs_render = true;
            }
            ui.separator();
            ui.weak("Drag: orbit · Right-drag / Shift-drag: pan · Scroll: zoom · Click: select · Double-click / F: fit · T: top");
        });
    }

    fn side_panel(&mut self, ui: &mut egui::Ui) {
        let Some(l) = &self.loaded else {
            ui.weak("No model loaded.");
            return;
        };
        let mut changed = false;
        egui::ScrollArea::vertical().show(ui, |ui| {
            ui.heading("Materials");
            for (cat, n) in &l.categories {
                let mut on = !self.hidden_categories.contains(cat);
                ui.horizontal(|ui| {
                    let c = palette::category(*cat);
                    let (r, _) = ui.allocate_exact_size(Vec2::splat(12.0), Sense::hover());
                    ui.painter()
                        .rect_filled(r, 2.0, Color32::from_rgb(c[0], c[1], c[2]));
                    if ui
                        .checkbox(&mut on, format!("{} ({n})", cat.label()))
                        .changed()
                    {
                        toggle(&mut self.hidden_categories, *cat, on);
                        changed = true;
                    }
                });
            }
            ui.add_space(8.0);
            ui.heading("IFC classes");
            for (class, n) in &l.classes {
                let mut on = !self.hidden_classes.contains(class);
                if ui.checkbox(&mut on, format!("{class} ({n})")).changed() {
                    toggle(&mut self.hidden_classes, class.clone(), on);
                    changed = true;
                }
            }
            ui.add_space(8.0);
            ui.heading("Storeys");
            for (storey, n) in &l.storeys {
                let mut on = !self.hidden_storeys.contains(storey);
                let label = if storey.is_empty() {
                    "(none)"
                } else {
                    storey.as_str()
                };
                if ui.checkbox(&mut on, format!("{label} ({n})")).changed() {
                    toggle(&mut self.hidden_storeys, storey.clone(), on);
                    changed = true;
                }
            }
            if !l.scene.warnings.is_empty() {
                ui.add_space(8.0);
                egui::CollapsingHeader::new(format!("Warnings ({})", l.scene.warnings.len())).show(
                    ui,
                    |ui| {
                        for w in &l.scene.warnings {
                            ui.label(w);
                        }
                    },
                );
            }
        });
        if changed {
            if self.selected.is_some_and(|i| !self.visible(i)) {
                self.selected = None;
            }
            self.needs_upload = true;
        }
    }

    fn properties(&self, ui: &mut egui::Ui) {
        let (Some(i), Some(l)) = (self.selected, &self.loaded) else {
            return;
        };
        let e = &l.scene.elements[i];
        ui.heading(&e.class);
        egui::Grid::new("props")
            .num_columns(2)
            .striped(true)
            .show(ui, |ui| {
                let mut row = |k: &str, v: &str| {
                    ui.label(k);
                    ui.add(egui::Label::new(v).selectable(true));
                    ui.end_row();
                };
                row("Name", &e.name);
                row("GlobalId", &e.global_id);
                row("Storey", &e.storey);
                row("Material", &e.material);
                row("Family", e.category.label());
                row("Triangles", &e.mesh.triangle_count().to_string());
                row("Volume", &format!("{:.3} m³", e.mesh.volume() / 1e9));
            });
    }

    fn status_bar(&self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if let Some(l) = &self.loaded {
                let visible = (0..l.scene.elements.len()).filter(|&i| self.visible(i)).count();
                ui.label(format!(
                    "{} · {visible}/{} elements · {} triangles · loaded in {:.0?} · last draw {:.1?} (CPU)",
                    l.path.file_name().map(|s| s.to_string_lossy()).unwrap_or_default(),
                    l.scene.elements.len(),
                    self.gpu_scene.as_ref().map_or(0, |g| g.triangles()),
                    l.load_time,
                    self.last_render,
                ));
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.weak(&self.adapter);
            });
        });
    }
}

fn toggle<T: Ord>(set: &mut BTreeSet<T>, item: T, visible: bool) {
    if visible {
        set.remove(&item);
    } else {
        set.insert(item);
    }
}

impl eframe::App for Viewer {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.poll_loading();

        // Files dropped on the window.
        if let Some(f) = ctx.input(|i| i.raw.dropped_files.first().map(|f| f.path().to_path_buf()))
        {
            self.open(f, &ctx);
        }

        egui::Panel::top("toolbar").show(ui, |ui| self.top_bar(ui));
        egui::Panel::bottom("status").show(ui, |ui| self.status_bar(ui));
        egui::Panel::left("filters")
            .resizable(true)
            .default_size(220.0)
            .show(ui, |ui| self.side_panel(ui));
        if self.selected.is_some() {
            egui::Panel::right("properties")
                .resizable(true)
                .default_size(280.0)
                .show(ui, |ui| self.properties(ui));
        }
        egui::CentralPanel::no_frame().show(ui, |ui| {
            let rect = ui.available_rect_before_wrap();
            let aspect = rect.width() / rect.height().max(1.0);
            self.handle_keys(&ctx, aspect);

            if let Some(l) = &self.loading {
                ui.centered_and_justified(|ui| {
                    ui.spinner();
                    ui.label(format!("Loading {}…", l.path.display()));
                });
                return;
            }
            if self.loaded.is_none() {
                ui.centered_and_justified(|ui| {
                    let msg = self.error.clone().unwrap_or_else(|| {
                        "Open an IFC or .bimble file (button above), or drop one on this window.".into()
                    });
                    ui.label(msg);
                });
                return;
            }
            if self.fit_pending {
                self.fit(aspect);
                self.fit_pending = false;
            }
            let ppp = ctx.pixels_per_point();
            let size_px = ((rect.width() * ppp).round().max(1.0) as u32, (rect.height() * ppp).round().max(1.0) as u32);
            self.render_view(size_px, ppp);
            if let Some(tex) = self.texture {
                let img = egui::Image::from_texture((tex, rect.size()))
                    .fit_to_exact_size(rect.size())
                    .sense(Sense::click_and_drag());
                let resp = ui.add(img);
                self.handle_view_input(ui, &resp);
                if self.needs_render || self.needs_upload {
                    ctx.request_repaint();
                }
            }
            if let Some(err) = &self.error {
                ui.colored_label(Color32::RED, err);
            }
        });
    }
}
