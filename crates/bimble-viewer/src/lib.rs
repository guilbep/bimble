//! Bimble viewer: a window around `bimble-render`.
//!
//! The 3D view is rendered offscreen by the same renderer as `bimble render`, then shown as an
//! egui image. It is only redrawn when something changes (camera, visibility, selection, size),
//! which keeps old laptops cool and quiet.

use bimble_core::MaterialCategory;
use bimble_geom::Scene;
use bimble_render::{
    Camera, ClipBox, DrawOptions, GpuScene, GraphicStyle, Renderer, Style, Target,
};
use eframe::egui::{self, Color32, PointerButton, Sense, Vec2};
use eframe::egui_wgpu;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, channel};
use std::time::{Duration, Instant};

mod section;
mod style_editor;
mod viewcube;

const SAMPLES: u32 = 4;
/// Duration of a view-cube camera turn.
const TURN_SECONDS: f32 = 0.3;

struct Turn {
    from: (f32, f32),
    to: (f32, f32),
    start: Instant,
}

/// Open the viewer window, optionally loading a file and a graphic style. Blocks until the
/// window is closed.
pub fn run(file: Option<PathBuf>, style: Option<PathBuf>) -> Result<(), String> {
    let initial_style = match &style {
        Some(p) => GraphicStyle::load(p)?,
        None => GraphicStyle::default(),
    };
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
            app.opts.style = initial_style;
            app.style_path = style;
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

    /// Section box: on/off, and the box (kept when switched off).
    section_on: bool,
    section_box: Option<ClipBox>,
    /// Camera turn in progress (view cube).
    turn: Option<Turn>,

    style_window: bool,
    /// File the style was loaded from or saved to.
    style_path: Option<PathBuf>,
    style_message: Option<String>,
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
            section_on: false,
            section_box: None,
            turn: None,
            style_window: false,
            style_path: None,
            style_message: None,
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
                // A box from the previous model means nothing for this one.
                self.section_on = false;
                self.section_box = None;
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    /// The active clipping box, if the section box is on.
    fn clip(&self) -> Option<ClipBox> {
        self.section_box.filter(|_| self.section_on)
    }

    /// Fit the section box to the selection, or to everything visible.
    fn fit_section_box(&mut self) {
        let Some(l) = &self.loaded else { return };
        let sel = self.selected;
        let b = ClipBox::around(&l.scene, |i| self.visible(i) && sel.is_none_or(|s| s == i));
        if b.is_some() {
            self.section_box = b;
            self.needs_render = true;
        }
    }

    fn toggle_section_box(&mut self) {
        self.section_on = !self.section_on;
        // The box needs edge normals in the uploaded scene; the first use re-uploads once.
        if self.section_on && self.gpu_scene.as_ref().is_some_and(|g| !g.edge_normals) {
            self.needs_upload = true;
        }
        if self.section_on && self.section_box.is_none() {
            self.fit_section_box();
        }
        self.needs_render = true;
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
        // Edge normals only while the section box is on (they cost GPU memory).
        self.gpu_scene =
            Some(
                self.renderer
                    .upload_with(&self.device, &l.scene, style, self.section_on),
            );
        self.needs_upload = false;
        self.needs_render = true;
    }

    /// Frame what is visible (within the section box, if on).
    fn fit(&mut self, aspect: f32) {
        self.fit_where(aspect, |_| true);
    }

    /// Frame the selection, or everything visible when nothing is selected.
    fn fit_selection(&mut self, aspect: f32) {
        let sel = self.selected;
        self.fit_where(aspect, |i| sel.is_none_or(|s| s == i));
    }

    fn fit_where(&mut self, aspect: f32, keep: impl Fn(usize) -> bool) {
        let Some(l) = &self.loaded else { return };
        let b = ClipBox::around(&l.scene, |i| self.visible(i) && keep(i))
            .or_else(|| ClipBox::around(&l.scene, |_| true));
        let Some(mut b) = b else { return };
        if let Some(c) = self.clip() {
            let (lo, hi) = (b.min.max(c.min), b.max.min(c.max));
            if lo.cmple(hi).all() {
                b = ClipBox { min: lo, max: hi };
            }
        }
        self.camera.fit_box(b.min, b.max, aspect);
        self.needs_render = true;
    }

    /// Turn the camera smoothly to a yaw and pitch.
    fn turn_to(&mut self, yaw: f32, pitch: f32) {
        let tau = std::f32::consts::TAU;
        // Shortest way round.
        let mut dy = (yaw - self.camera.yaw) % tau;
        if dy > std::f32::consts::PI {
            dy -= tau;
        } else if dy < -std::f32::consts::PI {
            dy += tau;
        }
        self.turn = Some(Turn {
            from: (self.camera.yaw, self.camera.pitch),
            to: (self.camera.yaw + dy, pitch),
            start: Instant::now(),
        });
    }

    /// Advance a camera turn; true while it is running.
    fn animate(&mut self) -> bool {
        let Some(t) = &self.turn else { return false };
        let k = (t.start.elapsed().as_secs_f32() / TURN_SECONDS).min(1.0);
        let e = k * k * (3.0 - 2.0 * k); // smoothstep
        self.camera.yaw = t.from.0 + (t.to.0 - t.from.0) * e;
        self.camera.pitch = t.from.1 + (t.to.1 - t.from.1) * e;
        self.needs_render = true;
        if k >= 1.0 {
            self.turn = None;
        }
        true
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
            clip: self.clip(),
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
            // Only redraw when the pointer actually moved: holding the button still must not
            // loop (a redraw request here would schedule the next frame, forever).
            let d = resp.drag_delta();
            if d != egui::Vec2::ZERO {
                self.camera.orbit(d.x * 0.008, d.y * 0.008);
                self.needs_render = true;
            }
        } else if resp.dragged_by(PointerButton::Secondary)
            || resp.dragged_by(PointerButton::Middle)
            || (resp.dragged_by(PointerButton::Primary) && ui.input(|i| i.modifiers.shift))
        {
            let d = resp.drag_delta() / rect.height().max(1.0);
            if d != egui::Vec2::ZERO {
                self.camera.pan(d.x, d.y);
                self.needs_render = true;
            }
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
            let hit =
                bimble_render::pick(&l.scene, |i| self.visible(i), o, d, self.clip().as_ref())
                    .map(|(i, _)| i);
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
        let (f, o, e, h, t, esc, b) = ctx.input(|i| {
            (
                i.key_pressed(egui::Key::F),
                i.key_pressed(egui::Key::O),
                i.key_pressed(egui::Key::E),
                i.key_pressed(egui::Key::H),
                i.key_pressed(egui::Key::T),
                i.key_pressed(egui::Key::Escape),
                i.key_pressed(egui::Key::B),
            )
        });
        if b && self.loaded.is_some() {
            self.toggle_section_box();
        }
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
            self.opts.hatching = !self.opts.hatching;
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
            changed |= ui.checkbox(&mut self.opts.hatching, "Hatching (H)").changed();
            changed |= ui.checkbox(&mut self.camera.orthographic, "Orthographic (O)").changed();
            if changed {
                self.needs_render = true;
            }
            ui.separator();
            let mut on = self.section_on;
            if ui
                .checkbox(&mut on, "Section box (B)")
                .on_hover_text("Clip the model to a box; drag the arrows to move its faces")
                .changed()
            {
                self.toggle_section_box();
            }
            if self.section_on
                && ui
                    .button(if self.selected.is_some() { "Box to selection" } else { "Box to visible" })
                    .clicked()
            {
                self.fit_section_box();
            }
            ui.separator();
            ui.toggle_value(&mut self.style_window, "Style…");
            ui.separator();
            ui.weak("Drag: orbit · Right-drag / Shift-drag: pan · Scroll: zoom · Click: select · Double-click / F: fit · T: top");
        });
    }

    /// View cube and navigation bar, top right of the 3D view.
    fn navigation(&mut self, ui: &mut egui::Ui, rect: egui::Rect, aspect: f32) {
        let size = 120.0;
        let cube = egui::Rect::from_min_size(
            egui::pos2(rect.max.x - size - 8.0, rect.min.y + 8.0),
            Vec2::splat(size),
        );
        if !rect.contains_rect(cube) {
            return;
        }
        let a = self.opts.style.selection.0;
        let accent = Color32::from_rgb(a[0], a[1], a[2]);
        match viewcube::show(ui, cube, self.camera.yaw, self.camera.pitch, accent) {
            viewcube::Action::Look(d) => {
                let (yaw, pitch) = viewcube::orientation(d, self.camera.yaw);
                self.turn_to(yaw, pitch);
            }
            viewcube::Action::Orbit(d) => {
                self.turn = None;
                self.camera.orbit(d.x * 0.008, d.y * 0.008);
                self.needs_render = true;
            }
            viewcube::Action::Turn(da) => {
                self.turn = None;
                self.camera.orbit(da, 0.0);
                self.needs_render = true;
            }
            viewcube::Action::None => {}
        }
        // Navigation bar under the cube.
        let w = 64.0;
        let mut y = cube.max.y + 4.0;
        let mut button = |ui: &mut egui::Ui, text: &str, tip: &str| {
            let r = egui::Rect::from_min_size(
                egui::pos2(cube.center().x - w / 2.0, y),
                Vec2::new(w, 22.0),
            );
            y += 26.0;
            ui.put(r, egui::Button::new(text))
                .on_hover_text(tip)
                .clicked()
        };
        if button(ui, "Home", "Default 3D view of everything visible") {
            let d = Camera::default();
            self.turn_to(d.yaw, d.pitch);
            self.fit(aspect);
        }
        if button(
            ui,
            "Fit all",
            "Zoom to everything visible (F or double-click)",
        ) {
            self.fit(aspect);
        }
        if self.selected.is_some() && button(ui, "Fit sel.", "Zoom to the selection") {
            self.fit_selection(aspect);
        }
        if button(ui, "+", "Zoom in") {
            self.camera.zoom(0.8);
            self.needs_render = true;
        }
        if button(ui, "-", "Zoom out") {
            self.camera.zoom(1.25);
            self.needs_render = true;
        }
    }

    fn style_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui.button("Load…").clicked()
                && let Some(p) = rfd::FileDialog::new()
                    .add_filter("Bimble style", &["json"])
                    .pick_file()
            {
                match GraphicStyle::load(&p) {
                    Ok(s) => {
                        self.opts.style = s;
                        self.style_message = Some(format!("Loaded {}", p.display()));
                        self.style_path = Some(p);
                        self.needs_render = true;
                    }
                    Err(e) => self.style_message = Some(e),
                }
            }
            if ui
                .button("Save…")
                .on_hover_text("Writes only what differs from the default style")
                .clicked()
            {
                let mut dialog = rfd::FileDialog::new().add_filter("Bimble style", &["json"]);
                dialog = match &self.style_path {
                    Some(p) => dialog.set_file_name(
                        p.file_name()
                            .map(|n| n.to_string_lossy().into_owned())
                            .unwrap_or_default(),
                    ),
                    None => dialog.set_file_name("style.json"),
                };
                if let Some(p) = dialog.save_file() {
                    self.style_message =
                        Some(match std::fs::write(&p, self.opts.style.to_json_diff()) {
                            Ok(()) => format!("Saved {}", p.display()),
                            Err(e) => format!("{}: {e}", p.display()),
                        });
                    self.style_path = Some(p);
                }
            }
            if ui.button("Reset").clicked() {
                self.opts.style = GraphicStyle::default();
                self.needs_render = true;
            }
        });
        if let Some(m) = &self.style_message {
            ui.weak(m);
        }
        ui.separator();
        if style_editor::edit(ui, &mut self.opts.style) {
            self.needs_render = true;
        }
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
                    let c = self.opts.style.families.get(*cat).color.0;
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
        if self.style_window {
            let mut open = true;
            egui::Window::new("Graphic style")
                .open(&mut open)
                .default_width(320.0)
                .default_height(620.0)
                .vscroll(true)
                .show(&ctx, |ui| self.style_panel(ui));
            self.style_window = open;
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
                if let (true, Some(mut b), Some(l)) = (self.section_on, self.section_box, &self.loaded) {
                    let vp = self.camera.view_proj(aspect, &l.bounds);
                    let a = self.opts.style.selection.0;
                    let accent = Color32::from_rgb(a[0], a[1], a[2]);
                    if section::overlay(ui, rect, &vp, &mut b, accent) {
                        self.section_box = Some(b);
                        self.needs_render = true;
                    }
                }
                self.navigation(ui, rect, aspect);
                if self.animate() {
                    ctx.request_repaint();
                }
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
