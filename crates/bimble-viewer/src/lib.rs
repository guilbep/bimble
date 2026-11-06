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
use std::time::Duration;
// std's Instant panics in browsers; web-time is std's on the desktop.
use web_time::Instant;

mod cutlines;
mod edit;
mod files;
mod icons;
mod layout;
mod measure;
mod ribbon;
mod section;
mod snap;
mod style_editor;
mod ui_theme;
mod viewcube;
mod viewing;
mod views;

const SAMPLES: u32 = 4;
/// Duration of a view-cube camera turn.
const TURN_SECONDS: f32 = 0.3;

/// Settings of the plan export window.
struct PlanSettings {
    storey: String,
    scale: u32,
    /// None: the smallest paper that fits.
    paper: Option<bimble_drawing::sheet::Paper>,
    cut_mm: f64,
    message: Option<String>,
    /// Preview: shown or not, zoom (1 = fit the window), and the image with what it was made from.
    preview: bool,
    zoom: f32,
    preview_image: Option<PlanPreview>,
    /// Title block, legend, texts and what is shown; `template_rev` counts its changes.
    template: bimble_drawing::template::PlanTemplate,
    template_path: Option<std::path::PathBuf>,
    template_rev: u64,
    /// Draw this view (plan with its range, ceiling plan, section) instead of `storey` cut at
    /// `cut_mm`; follows the active 2D view.
    view_id: Option<bimble_core::GlobalId>,
}

type PreviewKey = (
    String,
    u32,
    Option<bimble_drawing::sheet::Paper>,
    u64,
    bimble_core::style::GraphicStyle,
    u32,
    u64,
    String,
);

/// A rendered plan preview and the settings it shows (recomputed when they change).
struct PlanPreview {
    key: PreviewKey,
    texture: Option<egui::TextureHandle>,
    error: Option<String>,
    size_mm: (f64, f64),
}

/// The plan settings as drawing options; the project name is the model's file name.
fn plan_options(
    p: &PlanSettings,
    path: &std::path::Path,
    style: bimble_core::style::GraphicStyle,
    view: Option<bimble_core::view::View>,
    views: &[bimble_core::view::View],
) -> bimble_drawing::plan::PlanOptions {
    bimble_drawing::plan::PlanOptions {
        storey: p.storey.clone(),
        cut_above_mm: p.cut_mm,
        scale: p.scale,
        paper: p.paper,
        style,
        project: path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default(),
        date: today(),
        template: p.template.clone(),
        view,
        bare: false,
        section_marks: bimble_drawing::plan::section_marks(views),
    }
}

impl Default for PlanSettings {
    fn default() -> Self {
        PlanSettings {
            storey: String::new(),
            scale: 100,
            paper: None,
            cut_mm: 1200.0,
            message: None,
            preview: false,
            zoom: 1.0,
            preview_image: None,
            template: Default::default(),
            template_path: None,
            template_rev: 0,
            view_id: None,
        }
    }
}

/// How a linked model is shown.
#[derive(Clone, Copy, PartialEq, Eq)]
enum LinkMode {
    Solid,
    Edges,
    Hidden,
}

/// A model loaded next to ours as a reference (e.g. the architect's IFC).
struct Linked {
    path: PathBuf,
    scene: Scene,
    gpu: Option<GpuScene>,
    /// Added to the linked scene's coordinates to get ours (both are placed in world mm).
    offset: glam::Vec3,
    mode: LinkMode,
    /// For each of our elements / each of theirs: whether the other model has something there.
    matched_here: Vec<bool>,
    matched_there: Vec<bool>,
}

impl Linked {
    /// Its file name: how views refer to it (#39).
    fn name(&self) -> String {
        self.path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default()
    }
}

/// Our scene's bounds grown to cover the linked models (the depth range must cover them all).
fn bounds_with_links(scene: &Scene, links: &[Linked]) -> bimble_render::Sphere {
    links.iter().fold(bimble_render::Sphere::of(scene), |b, k| {
        let rb = bimble_render::Sphere::of(&k.scene);
        b.merge(bimble_render::Sphere {
            center: rb.center + k.offset,
            radius: rb.radius,
        })
    })
}

struct Turn {
    from: (f32, f32),
    to: (f32, f32),
    start: Instant,
}

/// Start the viewer in a browser page, drawing into `canvas` (WebGPU, or WebGL2 where the
/// browser has no WebGPU). Files are opened with the file picker or by dropping them on the page.
#[cfg(target_arch = "wasm32")]
pub async fn start_web(canvas: web_sys::HtmlCanvasElement) -> Result<(), String> {
    let mut options = eframe::WebOptions::default();
    // Browsers can expose WebGPU without an adapter behind it (Chrome on many Linux machines):
    // wgpu would pick WebGPU and fail, so ask for an adapter first and use WebGL2 without one.
    if !has_webgpu_adapter().await
        && let eframe::egui_wgpu::WgpuSetup::CreateNew(c) = &mut options.wgpu_options.wgpu_setup
    {
        c.instance_descriptor.backends = eframe::wgpu::Backends::GL;
    }
    eframe::WebRunner::new()
        .start(
            canvas,
            options,
            Box::new(|cc| {
                let rs = cc
                    .wgpu_render_state
                    .as_ref()
                    .ok_or("the viewer needs the wgpu renderer")?;
                ui_theme::apply(&cc.egui_ctx);
                Ok(Box::new(Viewer::new(rs)))
            }),
        )
        .await
        .map_err(|e| format!("{e:?}"))
}

/// Whether `navigator.gpu.requestAdapter()` gives an adapter.
#[cfg(target_arch = "wasm32")]
async fn has_webgpu_adapter() -> bool {
    use wasm_bindgen::JsCast;
    let Some(window) = web_sys::window() else {
        return false;
    };
    let get = |o: &wasm_bindgen::JsValue, k: &str| js_sys::Reflect::get(o, &k.into()).ok();
    let Some(gpu) = get(&window.navigator().into(), "gpu").filter(|g| !g.is_undefined()) else {
        return false;
    };
    let Some(request) =
        get(&gpu, "requestAdapter").and_then(|f| f.dyn_into::<js_sys::Function>().ok())
    else {
        return false;
    };
    let Ok(promise) = request.call0(&gpu) else {
        return false;
    };
    match wasm_bindgen_futures::JsFuture::from(js_sys::Promise::from(promise)).await {
        Ok(adapter) => !adapter.is_null() && !adapter.is_undefined(),
        Err(_) => false,
    }
}

/// Open the viewer window, optionally loading a file and a graphic style. Blocks until the
/// window is closed.
#[cfg(not(target_arch = "wasm32"))]
pub fn run(
    file: Option<PathBuf>,
    style: Option<PathBuf>,
    link: Option<PathBuf>,
) -> Result<(), String> {
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
            ui_theme::apply(&cc.egui_ctx);
            let mut app = Viewer::new(rs);
            app.opts.style = initial_style;
            app.style_path = style;
            if let Some(f) = file {
                app.open(f, &cc.egui_ctx);
            }
            app.pending_link = link;
            Ok(Box::new(app))
        }),
    )
    .map_err(|e| e.to_string())
}

/// Load a .ifc or .bimble file as a scene.
pub fn load_scene(path: &Path) -> Result<Scene, String> {
    load_any(path).map(|(s, _)| s)
}

/// A scene, and for a .bimble file the model itself (it can then be edited).
fn load_any(path: &Path) -> Result<(Scene, Option<bimble_core::Model>), String> {
    let is_ifc = path
        .extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"));
    if is_ifc {
        bimble_ifc::read::read_file(path)
            .map(|s| (s, None))
            .map_err(|e| e.to_string())
    } else {
        let m = bimble_core::file::load(path).map_err(|e| e.to_string())?;
        Ok((Scene::from_model(&m), Some(m)))
    }
}

/// A model from a file's content (picked or dropped in a browser): IFC or .bimble by name.
fn load_bytes(name: &str, bytes: &[u8]) -> Result<(Scene, Option<bimble_core::Model>), String> {
    let is_ifc = name.to_ascii_lowercase().ends_with(".ifc");
    if is_ifc {
        bimble_ifc::read::read_bytes(bytes)
            .map(|s| (s, None))
            .map_err(|e| e.to_string())
    } else {
        let m = bimble_core::file::from_bytes(bytes).map_err(|e| e.to_string())?;
        Ok((Scene::from_model(&m), Some(m)))
    }
}

/// A file's content to load on a later frame, so "Loading…" is drawn before the browser's only
/// thread is busy parsing.
struct ParseLater {
    name: String,
    bytes: Vec<u8>,
    link: bool,
    frames: u8,
    tx: std::sync::mpsc::Sender<LoadResult>,
}

type LoadResult = Result<(Scene, Option<bimble_core::Model>, Duration), String>;

struct Loading {
    path: PathBuf,
    rx: Receiver<LoadResult>,
}

struct Loaded {
    path: PathBuf,
    scene: Scene,
    load_time: Duration,
    bounds: bimble_render::Sphere,
    classes: BTreeMap<String, usize>,
    storeys: BTreeMap<String, usize>,
    categories: BTreeMap<MaterialCategory, usize>,
    /// The model of a .bimble file, editable.
    edit: Option<edit::Editor>,
}

type Counts = (
    BTreeMap<String, usize>,
    BTreeMap<String, usize>,
    BTreeMap<MaterialCategory, usize>,
);

/// Elements per IFC class, storey and material family.
fn counts(scene: &Scene) -> Counts {
    let mut classes = BTreeMap::new();
    let mut storeys = BTreeMap::new();
    let mut categories = BTreeMap::new();
    for e in &scene.elements {
        *classes.entry(e.class.clone()).or_default() += 1;
        *storeys.entry(e.storey.clone()).or_default() += 1;
        *categories.entry(e.category).or_default() += 1;
    }
    (classes, storeys, categories)
}

/// What the history window shows, read when it opens and after each history action.
struct HistoryView {
    /// None: the model has no history yet.
    branch: Option<String>,
    branches: Vec<(String, String)>,
    log: Vec<(String, bimble_core::repo::Commit)>,
    /// Changes not committed.
    pending: usize,
    error: Option<String>,
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

    /// Measuring: on/off, first point of the dimension being made, finished dimensions, and
    /// what the cursor would snap to.
    measure_on: bool,
    measure_first: Option<measure::Snap>,
    dimensions: Vec<measure::Dimension>,
    hover_snap: Option<measure::Snap>,
    /// Snap targets while placing (#25), for (scene revision, storey level), and the point the
    /// pointer snaps to (model mm) with what it snapped to.
    place_targets: Option<((u64, i64), snap::Targets)>,
    place_snap: Option<([f64; 2], Option<snap::SnapTo>)>,
    /// Cut outlines of the clip box's front faces (#35), for a key of what they depend on.
    cut_cache: Option<(u64, std::sync::Arc<Vec<[glam::Vec3; 2]>>)>,

    /// Element under the cursor, its edges uploaded for the highlight, and the pointer position
    /// it was picked at (picking again only when the pointer moves).
    hovered: Option<usize>,
    hover_gpu: Option<GpuScene>,
    hover_at: Option<egui::Pos2>,

    /// Linked (reference) models, read-only (#39), one being loaded, and the one whose
    /// differences with ours are coloured, if any.
    links: Vec<Linked>,
    linking: Option<Loading>,
    compare_with: Option<usize>,
    /// Linked models hidden in the active view, by file name.
    hidden_links: BTreeSet<String>,
    /// Model to link once ours is loaded (`bimble view --link`).
    pending_link: Option<PathBuf>,

    /// Plan export window and its settings.
    plan_window: bool,
    plan: PlanSettings,

    /// The Standards panel (graphic style) on the right (ADR 0027).
    standards_open: bool,
    standards_query: String,
    /// The ribbon's open tab, the command palette, tabs of open views and sheets.
    ribbon_tab: ribbon::Tab,
    palette: Option<ribbon::Palette>,
    open_tabs: Vec<bimble_core::GlobalId>,
    /// File the style was loaded from or saved to.
    style_path: Option<PathBuf>,
    style_message: Option<String>,

    /// Editing panel (for .bimble models) and the history window.
    edit_window: bool,
    history_window: bool,
    history_view: Option<HistoryView>,

    files: files::Files,
    parse_later: Option<ParseLater>,

    /// Views (ADR 0021): all of them (stored over the defaults), the one shown, the camera each
    /// view was left with, the side file of an IFC model, the view properties being edited, the
    /// reference plan dragged in the view, the view a picked reference goes to, the PDF scale.
    views: Vec<bimble_core::view::View>,
    active_view: Option<bimble_core::GlobalId>,
    view_cameras: std::collections::HashMap<bimble_core::GlobalId, Camera>,
    side_views: Option<viewing::SideViews>,
    view_form: Option<bimble_core::view::View>,
    view_message: Option<String>,
    /// What was just done with views (added a reference, saved the side file).
    view_note: Option<String>,
    moving_ref: Option<bimble_core::GlobalId>,
    reference_view: Option<bimble_core::GlobalId>,
    pdf_scale: f64,
    last_aspect: f32,
    /// The sheet open in the main view, if any (#34), and a counter of scene changes (its
    /// drawing is redone when the model changes).
    layout: Option<layout::Layout>,
    scene_rev: u64,
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
            measure_on: false,
            measure_first: None,
            dimensions: Vec::new(),
            hover_snap: None,
            place_targets: None,
            place_snap: None,
            cut_cache: None,
            hovered: None,
            hover_gpu: None,
            hover_at: None,
            links: Vec::new(),
            linking: None,
            compare_with: None,
            hidden_links: BTreeSet::new(),
            pending_link: None,
            plan_window: false,
            plan: PlanSettings::default(),
            standards_open: false,
            standards_query: String::new(),
            ribbon_tab: ribbon::Tab::default(),
            palette: None,
            open_tabs: Vec::new(),
            style_path: None,
            style_message: None,
            edit_window: false,
            history_window: false,
            history_view: None,
            files: files::Files::default(),
            parse_later: None,
            views: Vec::new(),
            active_view: None,
            view_cameras: Default::default(),
            side_views: None,
            view_form: None,
            view_message: None,
            view_note: None,
            moving_ref: None,
            reference_view: None,
            pdf_scale: 100.0,
            last_aspect: 1.0,
            layout: None,
            scene_rev: 0,
        }
    }

    /// Load a picked or dropped file: from its path on a thread (desktop), or from its bytes on a
    /// later frame (browser).
    fn open_picked(&mut self, p: files::Picked, ctx: &egui::Context) {
        let link = p.purpose == files::Purpose::Link;
        if let (Some(path), None) = (&p.path, &p.bytes) {
            if link {
                self.link(path.clone(), ctx);
            } else {
                self.open(path.clone(), ctx);
            }
            return;
        }
        let Some(bytes) = p.bytes else { return };
        let (tx, rx) = channel();
        let loading = Loading {
            path: PathBuf::from(&p.name),
            rx,
        };
        if link {
            self.linking = Some(loading);
        } else {
            self.loading = Some(loading);
            self.error = None;
        }
        self.parse_later = Some(ParseLater {
            name: p.name,
            bytes,
            link,
            frames: 2,
            tx,
        });
        ctx.request_repaint();
    }

    /// Parse a file's content once "Loading…" has been drawn.
    fn run_parse_later(&mut self, ctx: &egui::Context) {
        let Some(job) = &mut self.parse_later else {
            return;
        };
        if job.frames > 0 {
            job.frames -= 1;
            ctx.request_repaint();
            return;
        }
        let job = self.parse_later.take().unwrap();
        let t = Instant::now();
        let r = load_bytes(&job.name, &job.bytes).map(|(s, m)| {
            let m = if job.link { None } else { m };
            (s, m, t.elapsed())
        });
        let _ = job.tx.send(r);
        ctx.request_repaint();
    }

    /// Files picked in dialogs (or dropped), by what they are for.
    fn handle_picked(&mut self, ctx: &egui::Context) {
        while let Some(p) = self.files.poll() {
            // A plan dropped on a 2D view is a reference plan for it.
            let lower = p.name.to_ascii_lowercase();
            if p.purpose == files::Purpose::Open
                && [".dxf", ".pdf", ".dwg"].iter().any(|x| lower.ends_with(x))
            {
                if self.in_2d() && self.loaded.is_some() {
                    self.reference_view = self.active_view;
                    self.add_reference(p);
                } else {
                    self.view_message =
                        Some("Open a plan or section view to add a reference plan.".into());
                    self.error = Some(format!(
                        "{}: reference plans go in a plan or section view",
                        p.name
                    ));
                }
                continue;
            }
            // A views side file (#36), recognised by its format field.
            if p.purpose == files::Purpose::Open && lower.ends_with(".json") {
                match p.content().and_then(|b| self.open_side_file(&p.name, &b)) {
                    Ok(m) => {
                        self.view_message = Some(m);
                        self.error = None;
                    }
                    Err(e) => self.error = Some(e),
                }
                continue;
            }
            match p.purpose {
                files::Purpose::Reference => self.add_reference(p),
                files::Purpose::Open | files::Purpose::Link => self.open_picked(p, ctx),
                files::Purpose::PlanTemplate => {
                    let r = p.content().and_then(|b| {
                        bimble_drawing::template::PlanTemplate::from_json(&String::from_utf8_lossy(
                            &b,
                        ))
                        .map_err(|e| format!("{}: {e}", p.name))
                    });
                    match r {
                        Ok(t) => {
                            self.plan.template = t;
                            self.plan.template_path = Some(p.path_or_name());
                            self.plan.template_rev += 1;
                            self.plan.message = None;
                        }
                        Err(e) => self.plan.message = Some(e),
                    }
                }
                files::Purpose::Style => {
                    let r = p.content().and_then(|b| {
                        GraphicStyle::from_json(&String::from_utf8_lossy(&b))
                            .map_err(|e| format!("{}: {e}", p.name))
                    });
                    match r {
                        Ok(s) => {
                            self.opts.style = s;
                            self.style_message = Some(format!("Loaded {}", p.name));
                            self.style_path = Some(p.path_or_name());
                            self.needs_render = true;
                        }
                        Err(e) => self.style_message = Some(e),
                    }
                }
            }
        }
    }

    #[cfg(target_arch = "wasm32")]
    fn open(&mut self, _path: PathBuf, _ctx: &egui::Context) {}

    #[cfg(target_arch = "wasm32")]
    fn link(&mut self, _path: PathBuf, _ctx: &egui::Context) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn open(&mut self, path: PathBuf, ctx: &egui::Context) {
        let (tx, rx) = channel();
        let p = path.clone();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let r = load_any(&p).map(|(s, m)| (s, m, t.elapsed()));
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.loading = Some(Loading { path, rx });
        self.error = None;
    }

    /// Load another model as a reference, next to ours.
    #[cfg(not(target_arch = "wasm32"))]
    fn link(&mut self, path: PathBuf, ctx: &egui::Context) {
        let (tx, rx) = channel();
        let p = path.clone();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let r = load_scene(&p).map(|s| (s, None, t.elapsed()));
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.linking = Some(Loading { path, rx });
    }

    fn poll_linking(&mut self) {
        let Some(l) = &self.linking else { return };
        let Ok(result) = l.rx.try_recv() else { return };
        let path = self.linking.take().unwrap().path;
        let Some(main) = &mut self.loaded else { return };
        match result {
            Ok((scene, _, _)) => {
                let o = |k: usize| (scene.origin_mm[k] - main.scene.origin_mm[k]) as f32;
                let offset = glam::Vec3::new(o(0), o(1), o(2));
                let (matched_here, matched_there) = bimble_geom::compare::match_models(
                    &main.scene,
                    &scene,
                    bimble_geom::compare::Tolerance::default(),
                );
                let k = Linked {
                    path,
                    scene,
                    gpu: None,
                    offset,
                    mode: LinkMode::Solid,
                    matched_here,
                    matched_there,
                };
                // The same file linked again replaces the old link.
                let name = k.name();
                match self.links.iter().position(|x| x.name() == name) {
                    Some(i) => self.links[i] = k,
                    None => self.links.push(k),
                }
                main.bounds = bounds_with_links(&main.scene, &self.links);
                self.needs_upload = true;
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    fn unlink(&mut self, i: usize) {
        if i >= self.links.len() {
            return;
        }
        self.links.remove(i);
        self.compare_with = match self.compare_with {
            Some(c) if c == i => None,
            Some(c) if c > i => Some(c - 1),
            c => c,
        };
        if let Some(l) = &mut self.loaded {
            l.bounds = bounds_with_links(&l.scene, &self.links);
        }
        self.needs_upload = true;
    }

    fn poll_loading(&mut self) {
        let Some(l) = &self.loading else { return };
        let Ok(result) = l.rx.try_recv() else { return };
        let path = self.loading.take().unwrap().path;
        match result {
            Ok((scene, model, load_time)) => {
                // Also in the browser's console, where they can be copied.
                for w in scene.warnings.iter().take(50) {
                    log::warn!("{w}");
                }
                let (classes, storeys, categories) = counts(&scene);
                let bounds = bimble_render::Sphere::of(&scene);
                self.loaded = Some(Loaded {
                    path,
                    scene,
                    load_time,
                    bounds,
                    classes,
                    storeys,
                    categories,
                    edit: model.map(edit::Editor::new),
                });
                self.history_view = None;
                self.hidden_classes.clear();
                self.hidden_storeys.clear();
                self.hidden_categories.clear();
                self.selected = None;
                self.needs_upload = true;
                self.fit_pending = true;
                self.set_hovered(None);
                // A linked model and a box from the previous model mean nothing for this one.
                self.links.clear();
                self.compare_with = None;
                self.hidden_links.clear();
                self.section_on = false;
                self.section_box = None;
                self.measure_first = None;
                self.dimensions.clear();
                self.hover_snap = None;
                self.scene_rev += 1;
                self.layout = None;
                self.setup_views_after_load();
            }
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
    }

    // ---------- editing

    /// Apply commands from the editor as one undoable step, then show the result.
    fn apply_commands(&mut self, cmds: Vec<bimble_core::Command>) {
        let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) else {
            return;
        };
        match ed.apply(cmds) {
            Ok(()) => {
                ed.message = None;
                let select = ed.select_after.take();
                self.rebuild_scene(select.map(|g| g.to_string()));
            }
            Err(e) => {
                ed.message = Some(e);
                ed.select_after = None;
            }
        }
    }

    fn undo_redo(&mut self, undo: bool) {
        let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) else {
            return;
        };
        if if undo { ed.undo() } else { ed.redo() } {
            self.rebuild_scene(None);
        }
    }

    /// The scene again from the edited model; keeps the selection (by GlobalId) unless told
    /// what to select.
    fn rebuild_scene(&mut self, select: Option<String>) {
        let Some(l) = self.loaded.as_mut() else {
            return;
        };
        let Some(ed) = &l.edit else { return };
        let keep = select.or_else(|| {
            self.selected
                .and_then(|i| l.scene.elements.get(i))
                .map(|e| e.global_id.clone())
        });
        l.scene = Scene::from_model(&ed.model);
        (l.classes, l.storeys, l.categories) = counts(&l.scene);
        l.bounds = bounds_with_links(&l.scene, &self.links);
        self.selected = keep.and_then(|g| l.scene.elements.iter().position(|e| e.global_id == g));
        self.hovered = None;
        self.hover_gpu = None;
        self.hover_at = None;
        for k in &mut self.links {
            // Matches were computed against the old scene.
            let (here, there) = bimble_geom::compare::match_models(
                &l.scene,
                &k.scene,
                bimble_geom::compare::Tolerance::default(),
            );
            k.matched_here = here;
            k.matched_there = there;
        }
        self.needs_upload = true;
        self.scene_rev += 1;
        // Undo and redo can change views too; storeys can change the default ones.
        self.refresh_views();
    }

    fn save(&mut self) {
        if self.side_views.is_some() {
            // An IFC model is not written: its views go in the side file.
            self.save_side_views();
            return;
        }
        let Some(l) = self.loaded.as_mut() else {
            return;
        };
        let Some(ed) = l.edit.as_mut() else { return };
        // In a browser there is no file to write back to: the model is downloaded.
        #[cfg(target_arch = "wasm32")]
        let r = files::download(
            &l.path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| "model.bimble".into()),
            bimble_core::file::to_string(&ed.model).as_bytes(),
        );
        #[cfg(not(target_arch = "wasm32"))]
        let r = bimble_core::file::save(&ed.model, &l.path).map_err(|e| e.to_string());
        match r {
            Ok(()) => {
                ed.dirty = false;
                ed.message = Some(format!("Saved {}", l.path.display()));
            }
            Err(e) => ed.message = Some(format!("{}: {e}", l.path.display())),
        }
        if self.history_window {
            self.refresh_history();
        }
    }

    fn refresh_history(&mut self) {
        let Some(l) = &self.loaded else { return };
        let Some(ed) = &l.edit else { return };
        use bimble_core::repo::{Head, Repo, RepoError};
        let read = || -> Result<HistoryView, RepoError> {
            let r = match Repo::open(&l.path) {
                Ok(r) => r,
                Err(RepoError::NoRepo(_)) => {
                    return Ok(HistoryView {
                        branch: None,
                        branches: Vec::new(),
                        log: Vec::new(),
                        pending: 0,
                        error: None,
                    });
                }
                Err(e) => return Err(e),
            };
            let branch = match r.head()? {
                Head::Branch(b) => b,
                Head::Detached(h) => format!("(commit {})", &h[..12]),
            };
            let log = match r.head_commit()? {
                Some(_) => r.log("HEAD", 50)?,
                None => Vec::new(),
            };
            Ok(HistoryView {
                branch: Some(branch),
                branches: r.branches()?,
                log,
                pending: r.status(&ed.model)?.len(),
                error: None,
            })
        };
        self.history_view = Some(read().unwrap_or_else(|e| HistoryView {
            branch: None,
            branches: Vec::new(),
            log: Vec::new(),
            pending: 0,
            error: Some(e.to_string()),
        }));
    }

    fn history_panel(&mut self, ui: &mut egui::Ui) {
        use bimble_core::repo::Repo;
        let Some(l) = self.loaded.as_mut() else {
            return;
        };
        let Some(ed) = l.edit.as_mut() else {
            ui.weak("History is for .bimble models.");
            return;
        };
        let Some(v) = &self.history_view else {
            return;
        };
        let author = std::env::var("BIMBLE_AUTHOR")
            .or_else(|_| std::env::var("USER"))
            .or_else(|_| std::env::var("USERNAME"))
            .unwrap_or_else(|_| "unknown".into());
        let now = bimble_core::date::now_secs();
        let mut refresh = false;
        let mut reload: Option<bimble_core::Model> = None;
        if let Some(e) = &v.error {
            ui.colored_label(Color32::RED, e);
        }
        let Some(branch) = &v.branch else {
            ui.label("This model has no history yet.");
            ui.label("Message of the first commit:");
            ui.text_edit_singleline(&mut ed.commit_message);
            if ui
                .add_enabled(
                    !ed.commit_message.trim().is_empty(),
                    egui::Button::new("Start history and commit"),
                )
                .clicked()
            {
                let saved = if ed.dirty {
                    bimble_core::file::save(&ed.model, &l.path).map_err(|e| e.to_string())
                } else {
                    Ok(())
                };
                ed.message = Some(
                    match saved.and_then(|()| {
                        Repo::init(&l.path)
                            .and_then(|r| r.commit(&ed.model, &ed.commit_message, &author, now))
                            .map_err(|e| e.to_string())
                    }) {
                        Ok(h) => {
                            ed.dirty = false;
                            ed.commit_message.clear();
                            format!("Committed {}", &h[..12])
                        }
                        Err(e) => e,
                    },
                );
                refresh = true;
            }
            if let Some(m) = &ed.message {
                ui.label(m);
            }
            if refresh {
                self.refresh_history();
            }
            return;
        };
        ui.label(format!(
            "On {branch} · {} change(s) not committed",
            v.pending
        ));
        ui.horizontal(|ui| {
            ui.text_edit_singleline(&mut ed.commit_message)
                .on_hover_text("Commit message");
            if ui
                .add_enabled(
                    v.pending > 0 && !ed.commit_message.trim().is_empty(),
                    egui::Button::new("Commit"),
                )
                .clicked()
            {
                // Save first, so the file and the history agree.
                let saved = if ed.dirty {
                    bimble_core::file::save(&ed.model, &l.path).map_err(|e| e.to_string())
                } else {
                    Ok(())
                };
                ed.message = Some(
                    match saved.and_then(|()| {
                        Repo::open(&l.path)
                            .and_then(|r| r.commit(&ed.model, &ed.commit_message, &author, now))
                            .map_err(|e| e.to_string())
                    }) {
                        Ok(h) => {
                            ed.dirty = false;
                            ed.commit_message.clear();
                            format!("Committed {}", &h[..12])
                        }
                        Err(e) => e,
                    },
                );
                refresh = true;
            }
        });
        ui.separator();
        ui.label("Branches");
        for (name, h) in &v.branches {
            ui.horizontal(|ui| {
                let current = Some(name) == v.branch.as_ref();
                ui.label(format!("{} {name}  {}", if current { "●" } else { "○" }, &h[..10]));
                if !current
                    && ui
                        .add_enabled(v.pending == 0, egui::Button::new("Switch"))
                        .on_hover_text("Commit or undo the changes first")
                        .clicked()
                {
                    match Repo::open(&l.path).and_then(|r| r.switch(name)) {
                        Ok(m) => reload = Some(m),
                        Err(e) => ed.message = Some(e.to_string()),
                    }
                }
                if !current
                    && ui
                        .add_enabled(v.pending == 0, egui::Button::new("Merge"))
                        .on_hover_text("Merge this branch into the current one")
                        .clicked()
                {
                    use bimble_core::repo::Prefer;
                    match Repo::open(&l.path).and_then(|r| r.merge(name, Prefer::Neither, &author, now)) {
                        Ok(Ok((_, m))) => reload = Some(m),
                        Ok(Err(conflicts)) => {
                            let list: Vec<String> = conflicts.iter().take(8).map(|c| c.to_string()).collect();
                            ed.message = Some(format!(
                                "{} conflict(s), nothing merged (merge from the command line with --prefer):\n{}",
                                conflicts.len(),
                                list.join("\n")
                            ));
                        }
                        Err(e) => ed.message = Some(e.to_string()),
                    }
                }
            });
        }
        ui.horizontal(|ui| {
            ui.text_edit_singleline(&mut ed.new_branch)
                .on_hover_text("New branch name");
            if ui
                .add_enabled(
                    !ed.new_branch.trim().is_empty(),
                    egui::Button::new("New branch"),
                )
                .clicked()
            {
                let name = ed.new_branch.trim().to_owned();
                ed.message = Some(
                    match Repo::open(&l.path).and_then(|r| {
                        r.create_branch(&name, None)?;
                        r.switch(&name)
                    }) {
                        Ok(_) => {
                            ed.new_branch.clear();
                            format!("On new branch {name}")
                        }
                        Err(e) => e.to_string(),
                    },
                );
                refresh = true;
            }
        });
        if let Some(m) = &ed.message {
            ui.label(m);
        }
        ui.separator();
        egui::ScrollArea::vertical()
            .max_height(320.0)
            .show(ui, |ui| {
                for (h, c) in &v.log {
                    ui.label(egui::RichText::new(&c.message).strong());
                    ui.weak(format!(
                        "{}  {}  {}{}",
                        &h[..10],
                        bimble_core::date::format_utc(c.time),
                        c.author,
                        if c.parents.len() > 1 { "  [merge]" } else { "" }
                    ));
                }
            });
        if let Some(m) = reload {
            // The working file follows the history (no changes were pending).
            let ed = l.edit.as_mut().unwrap();
            ed.model = m;
            ed.history = bimble_core::History::default();
            ed.form = None;
            match bimble_core::file::save(&ed.model, &l.path) {
                Ok(()) => ed.dirty = false,
                Err(e) => ed.message = Some(e.to_string()),
            }
            self.rebuild_scene(None);
            refresh = true;
        }
        if refresh {
            self.refresh_history();
        }
    }

    /// Change the element under the cursor; uploads its edges for the highlight.
    fn set_hovered(&mut self, h: Option<usize>) {
        if h == self.hovered {
            return;
        }
        self.hovered = h;
        self.hover_gpu = match (h, &self.loaded) {
            (Some(i), Some(l)) => Some(self.renderer.upload_with(
                &self.device,
                &l.scene,
                |j, _| if j == i { Style::Normal } else { Style::Hidden },
                self.clipping(),
            )),
            _ => None,
        };
        self.needs_render = true;
    }

    /// The active clipping box: the 2D view's range, else the section box if it is on.
    fn clip(&self) -> Option<ClipBox> {
        if self.in_2d() {
            return self.view_clip();
        }
        self.section_box.filter(|_| self.section_on)
    }

    /// Whether the scene is cut (the upload then needs edge normals).
    fn clipping(&self) -> bool {
        self.section_on || self.in_2d()
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
        if self.loaded.is_none() {
            return;
        }
        let compare = self.compare_with.filter(|c| *c < self.links.len());
        // The linked models first: they need no borrow of ours.
        let linked_gpu: Vec<GpuScene> = self
            .links
            .iter()
            .enumerate()
            .map(|(n, k)| {
                let comparing = compare == Some(n);
                let there = &k.matched_there;
                self.renderer.upload_with(
                    &self.device,
                    &k.scene,
                    |i, _| {
                        if comparing && !there.get(i).copied().unwrap_or(true) {
                            Style::OnlyInReference
                        } else {
                            Style::Reference
                        }
                    },
                    self.clipping(),
                )
            })
            .collect();
        for (k, g) in self.links.iter_mut().zip(linked_gpu) {
            k.gpu = Some(g);
        }
        // Re-picked at the next pointer move (visibility or the section box may have changed).
        self.hovered = None;
        self.hover_gpu = None;
        self.hover_at = None;
        let comparing = compare.is_some();
        let here = compare.map(|c| &self.links[c].matched_here);
        let style = |i: usize, _: &bimble_geom::SceneElement| {
            if !self.visible(i) {
                Style::Hidden
            } else if self.selected == Some(i) {
                Style::Selected
            } else if comparing && here.is_some_and(|m| !m.get(i).copied().unwrap_or(true)) {
                Style::OnlyHere
            } else {
                Style::Normal
            }
        };
        let Some(l) = &self.loaded else { return };
        // Edge normals only while the section box is on (they cost GPU memory).
        self.gpu_scene =
            Some(
                self.renderer
                    .upload_with(&self.device, &l.scene, style, self.clipping()),
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
            ..self.opts.clone()
        };
        let mut layers = vec![bimble_render::Layer::main(gs)];
        for k in &self.links {
            let Some(g) = &k.gpu else { continue };
            if k.mode == LinkMode::Hidden || self.hidden_links.contains(&k.name()) {
                continue;
            }
            layers.push(bimble_render::Layer {
                scene: g,
                offset: k.offset,
                faces: k.mode == LinkMode::Solid,
                edge_color: Some(self.opts.style.reference_edges),
                linked: true,
            });
        }
        if let Some(h) = &self.hover_gpu {
            layers.push(bimble_render::Layer::highlight(h, self.opts.style.hover));
        }
        let bounds = self.loaded.as_ref().map_or(gs.bounds, |l| l.bounds);
        self.renderer.render_layers(
            &self.device,
            &self.queue,
            target,
            &layers,
            &bounds,
            &self.camera,
            &opts,
        );
        self.last_render = t.elapsed();
        self.needs_render = false;
    }

    fn handle_view_input(&mut self, ui: &egui::Ui, resp: &egui::Response) {
        let rect = resp.rect;
        let aspect = rect.width() / rect.height().max(1.0);
        let flat = self.in_2d();
        if flat && resp.dragged_by(PointerButton::Primary) && self.moving_ref.is_some() {
            // Moving a reference plan over the model.
            let d = resp.drag_delta();
            if d != egui::Vec2::ZERO && self.drag_reference(d, rect, aspect) {
                self.needs_render = true;
            }
        } else if flat && resp.dragged_by(PointerButton::Primary) {
            // 2D views don't orbit: dragging pans.
            let d = resp.drag_delta() / rect.height().max(1.0);
            if d != egui::Vec2::ZERO {
                self.camera.pan(d.x, d.y);
                self.needs_render = true;
            }
        } else if resp.dragged_by(PointerButton::Primary) && !ui.input(|i| i.modifiers.shift) {
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
        if resp.drag_stopped() && self.moving_ref.is_some() {
            self.finish_reference_drag();
        }
        if resp.hovered() {
            let (scroll, zoom) = ui.input(|i| (i.smooth_scroll_delta().y, i.zoom_delta()));
            if scroll != 0.0 || zoom != 1.0 {
                self.camera.zoom((-scroll * 0.0015).exp() / zoom);
                self.needs_render = true;
            }
        }
        if self
            .loaded
            .as_ref()
            .and_then(|l| l.edit.as_ref())
            .is_some_and(|ed| ed.placing.is_some())
        {
            self.set_hovered(None);
            let free = ui.input(|i| i.modifiers.shift);
            self.place_snap = resp
                .hover_pos()
                .or(resp.interact_pointer_pos())
                .and_then(|pos| self.placing_point(pos, rect, aspect, free));
            if resp.clicked()
                && let Some(pos) = resp.interact_pointer_pos()
                && let Some((p, _)) = self.placing_point(pos, rect, aspect, free)
            {
                let ed = self.loaded.as_mut().and_then(|l| l.edit.as_mut()).unwrap();
                match ed.click(p) {
                    Some(Ok(cmds)) => self.apply_commands(cmds),
                    Some(Err(e)) => ed.message = Some(e),
                    None => {}
                }
            }
            return;
        }
        if self.measure_on {
            self.set_hovered(None);
            // Snap under the cursor, for the marker and the next click.
            self.hover_snap = resp
                .hover_pos()
                .or(resp.interact_pointer_pos())
                .and_then(|p| self.snap_at(p, rect, aspect));
            if resp.clicked()
                && let Some(s) = self.hover_snap
            {
                match self.measure_first.take() {
                    None => self.measure_first = Some(s),
                    Some(a) => self.dimensions.push(measure::Dimension { a, b: s }),
                }
            }
            return;
        }
        // Hover highlight: pick again only when the pointer moved, never while dragging.
        let pointer = resp.hover_pos();
        if resp.dragged() || pointer.is_none() {
            if pointer.is_none() {
                self.hover_at = None;
                self.set_hovered(None);
            }
        } else if pointer != self.hover_at
            && let (Some(pos), Some(l)) = (pointer, &self.loaded)
        {
            self.hover_at = pointer;
            let ndc = [
                (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
                1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
            ];
            let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
            let clip = self.clip();
            let h = bimble_render::pick(&l.scene, |i| self.visible(i), o, d, clip.as_ref())
                .map(|(i, _)| i);
            self.set_hovered(h);
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
        if self.palette.is_none()
            && ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::K))
        {
            self.palette = Some(ribbon::Palette::default());
        }
        if self.palette.is_some() || ctx.egui_wants_keyboard_input() {
            return;
        }
        // Editing shortcuts (a .bimble model).
        if self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) {
            let (undo, redo, save, del) = ctx.input_mut(|i| {
                let cmd = egui::Modifiers::COMMAND;
                let shift = egui::Modifiers::COMMAND | egui::Modifiers::SHIFT;
                (
                    i.consume_key(cmd, egui::Key::Z),
                    i.consume_key(cmd, egui::Key::Y) || i.consume_key(shift, egui::Key::Z),
                    i.consume_key(cmd, egui::Key::S),
                    i.consume_key(egui::Modifiers::NONE, egui::Key::Delete),
                )
            });
            if undo || redo {
                self.undo_redo(undo);
            }
            if save {
                self.save();
            }
            if del
                && let (Some(i), Some(l)) = (self.selected, &self.loaded)
                && let Ok(id) = l.scene.elements[i].global_id.parse()
            {
                self.apply_commands(edit::delete(&[id]));
            }
        }
        // An IFC model's views are saved to their side file.
        if self.side_views.is_some()
            && ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::S))
        {
            self.save_side_views();
        }
        let (f, o, e, h, t, esc, b, m) = ctx.input(|i| {
            (
                i.key_pressed(egui::Key::F),
                i.key_pressed(egui::Key::O),
                i.key_pressed(egui::Key::E),
                i.key_pressed(egui::Key::H),
                i.key_pressed(egui::Key::T),
                i.key_pressed(egui::Key::Escape),
                i.key_pressed(egui::Key::B),
                i.key_pressed(egui::Key::M),
            )
        });
        if m {
            self.measure_on = !self.measure_on;
            self.measure_first = None;
            self.hover_snap = None;
        }
        // 2D views are cut by their range and seen square on: no box, perspective or turn.
        let flat = self.in_2d();
        let (b, o, t) = (b && !flat, o && !flat, t && !flat);
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
        // Esc first stops placing, drops a half-made dimension, then the selection.
        let stopped_placing = esc
            && self
                .loaded
                .as_mut()
                .and_then(|l| l.edit.as_mut())
                .is_some_and(|ed| ed.placing.take().is_some());
        let handled = stopped_placing || (esc && self.measure_first.take().is_some());
        if !handled && esc && self.selected.take().is_some() {
            self.needs_upload = true;
        }
    }

    /// Where a click places a point (#25): snapped to axis crossings, column centres, corners,
    /// axes and edges within 12 pixels; with Shift (`free`) or nothing near, rounded to 10 mm.
    fn placing_point(
        &mut self,
        pos: egui::Pos2,
        rect: egui::Rect,
        aspect: f32,
        free: bool,
    ) -> Option<([f64; 2], Option<snap::SnapTo>)> {
        let p = self.storey_point(pos, rect, aspect)?;
        let round = |v: f64| (v / 10.0).round() * 10.0;
        if free {
            return Some(([round(p[0]), round(p[1])], None));
        }
        let tol = self
            .storey_point(pos + Vec2::new(12.0, 0.0), rect, aspect)
            .map_or(0.0, |q| (q[0] - p[0]).hypot(q[1] - p[1]));
        let l = self.loaded.as_ref()?;
        let level = l.edit.as_ref()?.storey_elevation();
        let key = (self.scene_rev, level.round() as i64);
        if self.place_targets.as_ref().is_none_or(|(k, _)| *k != key) {
            let next = l
                .scene
                .levels()
                .iter()
                .map(|v| v.elevation_mm)
                .filter(|e| *e > level + 1.0)
                .fold(level + 4000.0, f64::min);
            self.place_targets = Some((key, snap::targets(&l.scene, level, next)));
        }
        let t = &self.place_targets.as_ref()?.1;
        Some(snap::snap(p, tol, t))
    }

    /// Where a screen point's ray meets the level of the storey being edited: storey-relative
    /// x, y in mm (model coordinates).
    fn storey_point(&self, pos: egui::Pos2, rect: egui::Rect, aspect: f32) -> Option<[f64; 2]> {
        let l = self.loaded.as_ref()?;
        let ed = l.edit.as_ref()?;
        let ndc = [
            (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
            1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
        ];
        let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
        let z = (ed.storey_elevation() - l.scene.origin_mm[2]) as f32;
        if d.z.abs() < 1e-6 {
            return None;
        }
        let t = (z - o.z) / d.z;
        if t < 0.0 {
            return None;
        }
        let p = o + d * t;
        Some([
            p.x as f64 + l.scene.origin_mm[0],
            p.y as f64 + l.scene.origin_mm[1],
        ])
    }

    /// What a click at a screen point would measure from (corner, edge or face).
    fn snap_at(&self, pos: egui::Pos2, rect: egui::Rect, aspect: f32) -> Option<measure::Snap> {
        let l = self.loaded.as_ref()?;
        let ndc = [
            (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
            1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
        ];
        let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
        let clip = self.clip();
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let ours = bimble_render::pick_hit(&l.scene, |i| self.visible(i), o, d, clip.as_ref()).map(
            |hit| {
                let mesh = &l.scene.elements[hit.element].mesh;
                (
                    hit.distance,
                    measure::snap(mesh, &hit, (o, d), &vp, rect, pos),
                )
            },
        );
        // Linked models (#39): measured and snapped to like ours, in their place.
        let theirs = self.links.iter().filter_map(|k| {
            if k.mode == LinkMode::Hidden || self.hidden_links.contains(&k.name()) {
                return None;
            }
            let clip = clip.map(|c| ClipBox {
                min: c.min - k.offset,
                max: c.max - k.offset,
            });
            let hit = bimble_render::pick_hit(&k.scene, |_| true, o - k.offset, d, clip.as_ref())?;
            let vpl = vp * glam::Mat4::from_translation(k.offset);
            let mesh = &k.scene.elements[hit.element].mesh;
            let mut s = measure::snap(mesh, &hit, (o - k.offset, d), &vpl, rect, pos);
            s.point += k.offset;
            s.element = usize::MAX;
            Some((hit.distance, s))
        });
        let model = ours
            .into_iter()
            .chain(theirs)
            .min_by(|a, b| a.0.total_cmp(&b.0))
            .map(|(_, s)| s);
        // Reference plans (2D views): a corner or edge of theirs against the model's snap,
        // corners before edges before faces, then the nearer on screen.
        let Some((r, rd)) = self.reference_snap(pos, rect, aspect) else {
            return model;
        };
        let rank = |k: measure::SnapKind| match k {
            measure::SnapKind::Vertex => 0,
            measure::SnapKind::Edge => 1,
            measure::SnapKind::Face => 2,
        };
        match model {
            Some(m) => {
                let md = section::project(&vp, rect, m.point).map_or(f32::MAX, |p| p.distance(pos));
                if (rank(m.kind), md) <= (rank(r.kind), rd) {
                    Some(m)
                } else {
                    Some(r)
                }
            }
            None => Some(r),
        }
    }

    /// Outlines of what the clip box cuts (#35), on its faces turned to the camera: crisp and
    /// complete, where the mesh's own edges may lose against a face at the same depth.
    fn draw_cut_lines(&mut self, ui: &egui::Ui, rect: egui::Rect, aspect: f32) {
        let Some(clip) = self.clip() else {
            self.cut_cache = None;
            return;
        };
        let Some(l) = &self.loaded else { return };
        let eye = if self.camera.orthographic {
            self.camera.target + (self.camera.eye() - self.camera.target).normalize_or_zero() * 1e9
        } else {
            self.camera.eye()
        };
        let faces = cutlines::front_faces(&clip, eye);
        let key = {
            use std::hash::{Hash, Hasher};
            let mut h = std::collections::hash_map::DefaultHasher::new();
            self.scene_rev.hash(&mut h);
            for v in clip.min.to_array().into_iter().chain(clip.max.to_array()) {
                v.to_bits().hash(&mut h);
            }
            faces.hash(&mut h);
            for i in 0..l.scene.elements.len() {
                self.visible(i).hash(&mut h);
            }
            h.finish()
        };
        if self.cut_cache.as_ref().is_none_or(|(k, _)| *k != key) {
            let lines = cutlines::cut_lines(&l.scene, &clip, faces, |i| self.visible(i));
            self.cut_cache = Some((key, std::sync::Arc::new(lines)));
        }
        let Some((_, lines)) = &self.cut_cache else {
            return;
        };
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let c = self.opts.style.edges.0;
        let stroke = egui::Stroke::new(1.5, Color32::from_rgb(c[0], c[1], c[2]));
        let painter = ui.painter_at(rect);
        for s in lines.iter() {
            if let (Some(a), Some(b)) = (
                section::project(&vp, rect, s[0]),
                section::project(&vp, rect, s[1]),
            ) {
                painter.line_segment([a, b], stroke);
            }
        }
    }

    /// Dimensions, the one being made, and the snap marker, over the 3D view.
    fn draw_dimensions(&self, ui: &egui::Ui, rect: egui::Rect, aspect: f32) {
        let Some(l) = &self.loaded else { return };
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let painter = ui.painter_at(rect);
        let c = self.opts.style.dimensions.0;
        let col = Color32::from_rgb(c[0], c[1], c[2]);
        let stroke = egui::Stroke::new(1.5, col);
        let p = |v: glam::Vec3| section::project(&vp, rect, v);
        let label = |at: egui::Pos2, text: String| {
            let galley =
                painter.layout_no_wrap(text, egui::FontId::proportional(13.0), Color32::WHITE);
            let r = egui::Rect::from_center_size(at, galley.size() + Vec2::new(8.0, 4.0));
            painter.rect_filled(r, 3.0, col);
            painter.galley(r.min + Vec2::new(4.0, 2.0), galley, Color32::WHITE);
        };
        for d in &self.dimensions {
            if let (Some(a), Some(b)) = (p(d.a.point), p(d.b.point)) {
                painter.line_segment([a, b], stroke);
                for e in [a, b] {
                    painter.circle_filled(e, 3.0, col);
                }
                let text = match d.perpendicular() {
                    Some(n) if (n - d.distance()).abs() > 0.5 => {
                        format!(
                            "{}  (faces {})",
                            measure::format_mm(d.distance()),
                            measure::format_mm(n)
                        )
                    }
                    _ => measure::format_mm(d.distance()),
                };
                label(a + (b - a) * 0.5, text);
            }
        }
        // Where a placed point would go (#25): a marker, and what it snapped to.
        if let Some(ed) = l.edit.as_ref().filter(|ed| ed.placing.is_some())
            && let Some((q, kind)) = self.place_snap
        {
            let o = l.scene.origin_mm;
            let z = ed.storey_elevation() - o[2];
            if let Some(m) = p(glam::Vec3::new(
                (q[0] - o[0]) as f32,
                (q[1] - o[1]) as f32,
                z as f32,
            )) {
                let s = egui::Stroke::new(2.0, col);
                match kind {
                    Some(snap::SnapTo::AxisCrossing | snap::SnapTo::ColumnCentre) => {
                        painter.circle_stroke(m, 6.0, s);
                        painter.line_segment([m - Vec2::X * 9.0, m + Vec2::X * 9.0], s);
                        painter.line_segment([m - Vec2::Y * 9.0, m + Vec2::Y * 9.0], s);
                    }
                    Some(snap::SnapTo::Corner) => {
                        painter.rect_stroke(
                            egui::Rect::from_center_size(m, Vec2::splat(10.0)),
                            0.0,
                            s,
                            egui::StrokeKind::Middle,
                        );
                    }
                    Some(snap::SnapTo::Axis | snap::SnapTo::Edge) => {
                        let pts = vec![
                            m + Vec2::new(0.0, -6.0),
                            m + Vec2::new(6.0, 5.0),
                            m + Vec2::new(-6.0, 5.0),
                        ];
                        painter.add(egui::Shape::closed_line(pts, s));
                    }
                    None => {
                        painter.circle_filled(m, 3.0, col);
                    }
                }
                let text = match kind {
                    Some(k) => format!("{} · {:.0}, {:.0}", k.label(), q[0], q[1]),
                    None => format!("{:.0}, {:.0} (Shift: no snap)", q[0], q[1]),
                };
                label(m + Vec2::new(0.0, 22.0), text);
            }
        }
        if !self.measure_on {
            return;
        }
        if let (Some(a), Some(h)) = (self.measure_first, self.hover_snap)
            && let (Some(pa), Some(ph)) = (p(a.point), p(h.point))
        {
            painter.line_segment([pa, ph], egui::Stroke::new(1.0, col));
            label(
                pa + (ph - pa) * 0.5,
                measure::format_mm((h.point - a.point).length()),
            );
        }
        if let Some(a) = self.measure_first.and_then(|a| p(a.point)) {
            painter.circle_filled(a, 3.5, col);
        }
        // Snap marker: square = corner, triangle = edge, circle = face.
        if let Some(h) = self.hover_snap
            && let Some(m) = p(h.point)
        {
            let s = egui::Stroke::new(2.0, col);
            match h.kind {
                measure::SnapKind::Vertex => {
                    painter.rect_stroke(
                        egui::Rect::from_center_size(m, Vec2::splat(10.0)),
                        0.0,
                        s,
                        egui::StrokeKind::Middle,
                    );
                }
                measure::SnapKind::Edge => {
                    let pts = vec![
                        m + Vec2::new(0.0, -6.0),
                        m + Vec2::new(6.0, 5.0),
                        m + Vec2::new(-6.0, 5.0),
                    ];
                    painter.add(egui::Shape::closed_line(pts, s));
                }
                measure::SnapKind::Face => {
                    painter.circle_stroke(m, 5.0, s);
                }
            }
        }
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
        // 2D views are seen square on: no cube, no Home.
        let flat = self.in_2d();
        let action = if flat {
            viewcube::Action::None
        } else {
            viewcube::show(ui, cube, self.camera.yaw, self.camera.pitch, accent)
        };
        match action {
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
        let mut y = if flat { cube.min.y } else { cube.max.y + 4.0 };
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
        if !flat && button(ui, "Home", "Default 3D view of everything visible") {
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

    fn plan_panel(&mut self, ui: &mut egui::Ui) {
        use bimble_drawing::sheet::Paper;
        let Some(l) = &self.loaded else {
            ui.weak("Open a model first.");
            return;
        };
        if l.scene.storeys.is_empty() {
            ui.weak("This model has no storeys.");
            return;
        }
        let p = &mut self.plan;
        if !l.scene.storeys.iter().any(|s| s.name == p.storey) {
            // Default: the lowest storey at or above 0, else the first.
            p.storey = l
                .scene
                .storeys
                .iter()
                .find(|s| s.elevation_mm >= -1.0)
                .unwrap_or(&l.scene.storeys[0])
                .name
                .clone();
        }
        // The view drawn, if any (it may have been deleted).
        if p.view_id
            .is_some_and(|id| !self.views.iter().any(|v| v.id == id && v.is_2d()))
        {
            p.view_id = None;
        }
        let view = p
            .view_id
            .and_then(|id| self.views.iter().find(|v| v.id == id))
            .cloned();
        let views = &self.views;
        egui::Grid::new("plan-settings")
            .num_columns(2)
            .spacing([12.0, 6.0])
            .show(ui, |ui| {
                ui.label("Draw");
                egui::ComboBox::from_id_salt("plan-view")
                    .selected_text(view.as_ref().map_or("Storey plan", |v| v.name.as_str()))
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut p.view_id, None, "Storey plan");
                        for v in views.iter().filter(|v| v.is_2d()) {
                            ui.selectable_value(&mut p.view_id, Some(v.id), &v.name);
                        }
                    });
                ui.end_row();
                // A view brings its storey and range; the storey plan needs them here.
                if view.is_none() {
                    ui.label("Storey");
                    egui::ComboBox::from_id_salt("plan-storey")
                        .selected_text(p.storey.clone())
                        .show_ui(ui, |ui| {
                            for s in l.scene.storeys.iter().rev() {
                                ui.selectable_value(
                                    &mut p.storey,
                                    s.name.clone(),
                                    format!("{}  ({:+.3} m)", s.name, s.elevation_mm / 1000.0),
                                );
                            }
                        });
                    ui.end_row();
                }
                ui.label("Scale 1:");
                ui.horizontal(|ui| {
                    ui.add(egui::DragValue::new(&mut p.scale).range(1..=5000));
                    for s in [50, 100, 200, 500] {
                        if ui.small_button(format!("{s}")).clicked() {
                            p.scale = s;
                        }
                    }
                });
                ui.end_row();
                ui.label("Paper");
                egui::ComboBox::from_id_salt("plan-paper")
                    .selected_text(p.paper.map_or("Smallest that fits", |x| x.name()))
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut p.paper, None, "Smallest that fits");
                        for x in Paper::ALL {
                            ui.selectable_value(&mut p.paper, Some(x), x.name());
                        }
                    });
                ui.end_row();
                if view.is_none() {
                    ui.label("Cut height");
                    ui.add(
                        egui::DragValue::new(&mut p.cut_mm)
                            .range(-10_000.0..=10_000.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                }
            });
        ui.horizontal(|ui| {
            let t = &mut p.template;
            let mut changed = false;
            changed |= ui.checkbox(&mut t.seen_fills, "Seen fills").changed();
            let mut axes = t.axes != bimble_drawing::template::Axes::Off;
            if ui.checkbox(&mut axes, "Axes").changed() {
                t.axes = if axes {
                    bimble_drawing::template::Axes::ModelOrColumns
                } else {
                    bimble_drawing::template::Axes::Off
                };
                changed = true;
            }
            changed |= ui.checkbox(&mut t.dimensions, "Dimensions").changed();
            changed |= ui.checkbox(&mut t.legend.show, "Legend").changed();
            changed |= ui.checkbox(&mut t.slab_levels, "Levels").changed();
            if changed {
                p.template_rev += 1;
            }
        });
        ui.horizontal(|ui| {
            let t = &mut p.template;
            let mut changed = false;
            changed |= ui
                .checkbox(&mut t.element_labels, "Sections of columns, beams")
                .changed();
            changed |= ui.checkbox(&mut t.opening_dimensions, "Openings").changed();
            changed |= ui
                .checkbox(&mut t.section_markers, "Section marks")
                .changed();
            if changed {
                p.template_rev += 1;
            }
        });
        ui.horizontal(|ui| {
            if ui.button("Template…").clicked() {
                self.files.pick(files::Purpose::PlanTemplate, ui.ctx());
            }
            if p.template_path.is_some() && ui.small_button("Default").clicked() {
                p.template = Default::default();
                p.template_path = None;
                p.template_rev += 1;
            }
            ui.weak(match &p.template_path {
                Some(path) => path
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default(),
                None => "Default template".into(),
            });
        });
        ui.weak("Colours, hatches and pens come from the graphic style (Style…).");
        let export = ui.horizontal(|ui| {
            if ui.button("Export PDF…").clicked() {
                Some(false)
            } else if ui.button("Export SVG…").clicked() {
                Some(true)
            } else {
                None
            }
        });
        if let Some(svg) = export.inner {
            let opts = plan_options(
                p,
                &l.path,
                self.opts.style.clone(),
                view.clone(),
                &self.views,
            );
            p.message = Some(match bimble_drawing::plan::plan(&l.scene, &opts) {
                Err(e) => e,
                Ok((sheet, info)) => {
                    let (bytes, ext) = if svg {
                        (sheet.to_svg().into_bytes(), "svg")
                    } else {
                        (sheet.to_pdf(), "pdf")
                    };
                    let what = view.as_ref().map_or(p.storey.as_str(), |v| v.name.as_str());
                    let name = format!("plan-{what}-1-{}.{ext}", p.scale);
                    let filter: (&str, &[&str]) = if svg {
                        ("SVG", &["svg"])
                    } else {
                        ("PDF", &["pdf"])
                    };
                    match files::save(&name, &[filter], &bytes) {
                        Ok(Some(to)) => format!(
                            "Saved {to} ({}{})",
                            info.paper.name(),
                            if info.portrait { " portrait" } else { "" }
                        ),
                        Ok(None) => String::new(),
                        Err(e) => e,
                    }
                }
            });
        }
        if let Some(m) = &p.message {
            ui.label(m);
        }
        ui.separator();
        ui.horizontal(|ui| {
            ui.checkbox(&mut p.preview, "Preview");
            if p.preview {
                ui.add(
                    egui::Slider::new(&mut p.zoom, 1.0..=8.0)
                        .logarithmic(true)
                        .text("zoom"),
                );
            }
        });
        if !p.preview {
            return;
        }
        // Width of the image: the window width times the zoom, in screen pixels.
        let avail = ui.available_width().max(200.0);
        let ppp = ui.ctx().pixels_per_point();
        let px_w = (avail * p.zoom * ppp).round() as u32;
        let key = (
            p.storey.clone(),
            p.scale,
            p.paper,
            p.cut_mm.to_bits(),
            self.opts.style.clone(),
            px_w / 50 * 50,
            p.template_rev,
            view.as_ref()
                .map(|v| serde_json::to_string(v).unwrap_or_default())
                .unwrap_or_default(),
        );
        if p.preview_image.as_ref().is_none_or(|i| i.key != key) {
            let opts = plan_options(
                p,
                &l.path,
                self.opts.style.clone(),
                view.clone(),
                &self.views,
            );
            let mut img = PlanPreview {
                key,
                texture: None,
                error: None,
                size_mm: (0.0, 0.0),
            };
            match bimble_drawing::plan::plan(&l.scene, &opts) {
                Err(e) => img.error = Some(e),
                Ok((sheet, _)) => {
                    img.size_mm = (sheet.width_mm, sheet.height_mm);
                    match sheet.rasterize(px_w as f64 / sheet.width_mm) {
                        Err(e) => img.error = Some(e),
                        Ok(r) => {
                            let ci = egui::ColorImage::from_rgba_unmultiplied(
                                [r.width as usize, r.height as usize],
                                &r.rgba,
                            );
                            img.texture = Some(ui.ctx().load_texture(
                                "plan-preview",
                                ci,
                                egui::TextureOptions::LINEAR,
                            ));
                        }
                    }
                }
            }
            p.preview_image = Some(img);
        }
        let Some(img) = &p.preview_image else { return };
        if let Some(e) = &img.error {
            ui.colored_label(Color32::from_rgb(220, 80, 60), e);
            return;
        }
        if let Some(t) = &img.texture {
            let w = avail * p.zoom;
            let h = w * (img.size_mm.1 / img.size_mm.0) as f32;
            egui::ScrollArea::both()
                .max_height(ui.ctx().content_rect().height() * 0.7)
                .show(ui, |ui| {
                    ui.add(egui::Image::from_texture((t.id(), Vec2::new(w, h))));
                });
        }
    }

    fn style_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui.button("Load…").clicked() {
                self.files.pick(files::Purpose::Style, ui.ctx());
            }
            if ui
                .button("Save…")
                .on_hover_text("Writes only what differs from the default style")
                .clicked()
            {
                let name = self
                    .style_path
                    .as_ref()
                    .and_then(|p| p.file_name())
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_else(|| "style.json".into());
                let json = self.opts.style.to_json_diff();
                match files::save(&name, &[("Bimble style", &["json"])], json.as_bytes()) {
                    Ok(Some(to)) => {
                        self.style_message = Some(format!("Saved {to}"));
                        self.style_path = Some(PathBuf::from(to));
                    }
                    Ok(None) => {}
                    Err(e) => self.style_message = Some(e),
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
        if style_editor::edit(ui, &mut self.opts.style, &mut self.standards_query) {
            self.needs_render = true;
        }
    }

    /// Under the navigator: links (models and reference plans), measurements, warnings.
    fn side_extras(&mut self, ui: &mut egui::Ui) {
        if self.loaded.is_none() {
            return;
        }
        let mut changed = false;
        let mut unlink_requested = None;
        let mut link_requested = false;
        egui::CollapsingHeader::new(egui::RichText::new("Links").strong())
            .default_open(!self.links.is_empty())
            .show(ui, |ui| {
                ui.weak("Linked models are read-only; this one is the host.");
                let mut compare = self.compare_with;
                for (n, k) in self.links.iter_mut().enumerate() {
                    ui.push_id(n, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(k.name());
                            if ui.small_button("Unlink").clicked() {
                                unlink_requested = Some(n);
                            }
                        });
                        let mut mode = k.mode;
                        ui.horizontal(|ui| {
                            ui.radio_value(&mut mode, LinkMode::Solid, "Solid");
                            ui.radio_value(&mut mode, LinkMode::Edges, "Edges");
                            ui.radio_value(&mut mode, LinkMode::Hidden, "Hidden");
                        });
                        if mode != k.mode {
                            k.mode = mode;
                            self.needs_render = true;
                        }
                        let (only_here, only_there) = (
                            k.matched_here.iter().filter(|m| !**m).count(),
                            k.matched_there.iter().filter(|m| !**m).count(),
                        );
                        let mut cmp = compare == Some(n);
                        let r = ui.checkbox(&mut cmp, "Show differences").on_hover_text(
                            "Matched by position (bounding boxes, 20 mm tolerance, 80 % covered), not by id",
                        );
                        if r.changed() {
                            compare = cmp.then_some(n);
                            changed = true;
                        }
                        ui.weak(format!(
                            "{only_here} only here · {only_there} only in this link"
                        ));
                        ui.separator();
                    });
                }
                self.compare_with = compare;
                link_requested = ui
                    .button(if self.linking.is_some() { "Linking…" } else { "Link model…" })
                    .on_hover_text("Load another model as a read-only reference (e.g. the architect's IFC)")
                    .clicked();
                ui.separator();
                self.reference_links(ui);
            });
        if !self.dimensions.is_empty() {
            egui::CollapsingHeader::new(
                egui::RichText::new(format!("Measurements ({})", self.dimensions.len())).strong(),
            )
            .default_open(true)
            .show(ui, |ui| {
                let mut remove = None;
                for (k, d) in self.dimensions.iter().enumerate() {
                    ui.horizontal(|ui| {
                        if ui.small_button("x").on_hover_text("Delete").clicked() {
                            remove = Some(k);
                        }
                        ui.label(format!("{}. {}", k + 1, measure::format_mm(d.distance())));
                    });
                    let v = d.delta();
                    let mut detail = format!(
                        "ΔX {}\nΔY {}\nΔZ {}",
                        measure::format_mm(v.x),
                        measure::format_mm(v.y),
                        measure::format_mm(v.z)
                    );
                    if let Some(n) = d.perpendicular() {
                        detail.push_str(&format!("\nbetween faces {}", measure::format_mm(n)));
                    }
                    ui.weak(detail);
                }
                if let Some(k) = remove {
                    self.dimensions.remove(k);
                }
                if ui.small_button("Clear all").clicked() {
                    self.dimensions.clear();
                }
            });
        }
        if let Some(l) = &self.loaded
            && !l.scene.warnings.is_empty()
        {
            egui::CollapsingHeader::new(format!("Warnings ({})", l.scene.warnings.len())).show(
                ui,
                |ui| {
                    for w in &l.scene.warnings {
                        ui.label(w);
                    }
                },
            );
        }
        if changed {
            self.needs_upload = true;
        }
        if link_requested {
            self.files.pick(files::Purpose::Link, ui.ctx());
        }
        if let Some(i) = unlink_requested {
            self.unlink(i);
        }
    }

    fn properties(&mut self, ui: &mut egui::Ui) {
        let (Some(i), Some(l)) = (self.selected, self.loaded.as_mut()) else {
            return;
        };
        if let Some(ed) = l.edit.as_mut()
            && let Ok(id) = l.scene.elements[i]
                .global_id
                .parse::<bimble_core::GlobalId>()
        {
            ed.element_panel(ui, id);
            if let Some(m) = &ed.message {
                ui.label(m);
            }
            if let Some(cmds) = ed.pending.take() {
                self.apply_commands(cmds);
            }
            return;
        }
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
            ui.label(self.status_hint());
            ui.separator();
            if let Some(l) = &self.loaded {
                let visible = (0..l.scene.elements.len()).filter(|&i| self.visible(i)).count();
                ui.weak(format!(
                    "{visible}/{} elements · {} triangles · loaded in {:.0?} · last draw {:.1?} (CPU)",
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
        if self.loaded.is_some()
            && let Some(p) = self.pending_link.take()
        {
            self.link(p, &ctx);
        }
        self.poll_linking();

        // Files dropped on the window.
        if let Some(f) = ctx.input(|i| i.raw.dropped_files.first().cloned()) {
            self.files.dropped(f, &ctx);
        }
        self.handle_picked(&ctx);
        self.run_parse_later(&ctx);

        let bar = |fill: egui::Color32| {
            egui::Frame::new()
                .fill(fill)
                .inner_margin(egui::Margin::symmetric(ui_theme::GAP_M as i8, 4))
        };
        egui::Panel::top("title")
            .frame(bar(ui_theme::TITLE_BG))
            .show(ui, |ui| self.title_row(ui));
        egui::Panel::top("ribbon-tabs")
            .frame(bar(ui_theme::TABS_BG).inner_margin(egui::Margin {
                left: ui_theme::GAP_M as i8,
                right: 0,
                top: 2,
                bottom: 0,
            }))
            .show(ui, |ui| self.ribbon_tabs(ui));
        egui::Panel::top("ribbon")
            .frame(
                bar(ui_theme::RIBBON_BG)
                    .inner_margin(egui::Margin::symmetric(ui_theme::GAP_M as i8, 6)),
            )
            .show(ui, |ui| self.ribbon_panel(ui));
        egui::Panel::bottom("status").show(ui, |ui| self.status_bar(ui));
        // Left: the model navigator above, the properties (of the selected element, else of
        // the view) below.
        egui::Panel::left("browser")
            .resizable(true)
            .default_size(290.0)
            .min_size(220.0)
            .show(ui, |ui| {
                egui::Panel::top("navigator")
                    .resizable(true)
                    .default_size(ui.available_height() * 0.45)
                    .min_size(120.0)
                    .show(ui, |ui| self.navigator(ui));
                egui::CentralPanel::default().show(ui, |ui| {
                    egui::ScrollArea::vertical()
                        .id_salt("properties")
                        .auto_shrink([false, false])
                        .show(ui, |ui| {
                            if self.selected.is_some() {
                                if ui.small_button("← View properties").clicked() {
                                    self.selected = None;
                                    self.needs_upload = true;
                                } else {
                                    self.properties(ui);
                                }
                            } else if self.layout.is_some() {
                                self.layout_properties(ui);
                            } else {
                                self.view_properties(ui);
                            }
                        });
                });
            });
        if self.plan_window {
            let mut open = true;
            egui::Window::new("On paper")
                .open(&mut open)
                .default_width(300.0)
                .resizable(true)
                .show(&ctx, |ui| self.plan_panel(ui));
            self.plan_window = open;
        }
        if self.edit_window && self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) {
            let mut open = true;
            egui::Window::new("Place elements")
                .open(&mut open)
                .default_width(260.0)
                .show(&ctx, |ui| {
                    if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
                        ed.place_panel(ui);
                        if let Some(m) = &ed.message {
                            ui.label(m);
                        }
                    }
                });
            if !open && let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
                ed.placing = None;
            }
            self.edit_window = open;
        }
        if self.history_window && self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) {
            let mut open = true;
            egui::Window::new("History")
                .open(&mut open)
                .default_width(340.0)
                .show(&ctx, |ui| self.history_panel(ui));
            self.history_window = open;
        }
        // Right: the graphic standards, edited live (ADR 0027).
        if self.standards_open {
            egui::Panel::right("standards")
                .resizable(true)
                .default_size(400.0)
                .min_size(300.0)
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.heading("Standards");
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if ui.small_button("×").on_hover_text("Close").clicked() {
                                self.standards_open = false;
                            }
                        });
                    });
                    // Wide tables scroll; the panel keeps the width the user gave it.
                    egui::ScrollArea::both()
                        .id_salt("standards-scroll")
                        .auto_shrink([false, false])
                        .show(ui, |ui| self.style_panel(ui));
                });
        }
        if self.loaded.is_some() {
            egui::Panel::top("view-tabs")
                .frame(
                    egui::Frame::new()
                        .fill(ui_theme::TITLE_BG)
                        .inner_margin(egui::Margin {
                            left: 6,
                            right: 6,
                            top: 4,
                            bottom: 0,
                        }),
                )
                .show(ui, |ui| self.view_tabs(ui));
        }
        egui::CentralPanel::no_frame().show(ui, |ui| {
            let rect = ui.available_rect_before_wrap();
            let aspect = rect.width() / rect.height().max(1.0);
            self.last_aspect = aspect;
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
            // A sheet open: it takes the main view.
            if self.layout.is_some() {
                self.layout_ui(ui, rect);
                if let Some(err) = &self.error {
                    ui.colored_label(Color32::RED, err);
                }
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
                if let (true, false, Some(mut b), Some(l)) =
                    (self.section_on, self.in_2d(), self.section_box, &self.loaded)
                {
                    let vp = self.camera.view_proj(aspect, &l.bounds);
                    let a = self.opts.style.selection.0;
                    let accent = Color32::from_rgb(a[0], a[1], a[2]);
                    if section::overlay(ui, rect, &vp, &mut b, accent) {
                        self.section_box = Some(b);
                        self.needs_render = true;
                    }
                }
                if self.in_2d() {
                    self.draw_references(ui, rect, aspect);
                    self.draw_annotations(ui, rect, aspect);
                }
                self.draw_cut_lines(ui, rect, aspect);
                self.draw_dimensions(ui, rect, aspect);
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
        self.palette_ui(&ctx);
    }
}

/// Today's date (UTC) as YYYY-MM-DD, for title blocks.
fn today() -> String {
    bimble_drawing::today()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// With several links, the depth range covers every one where it is placed (#39).
    #[test]
    fn bounds_cover_every_link() {
        use bimble_core::demo::{FrameSpec, concrete_frame};
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let link = |name: &str, x: f32| Linked {
            path: PathBuf::from(name),
            scene: scene.clone(),
            gpu: None,
            offset: glam::Vec3::new(x, 0.0, 0.0),
            mode: LinkMode::Solid,
            matched_here: Vec::new(),
            matched_there: Vec::new(),
        };
        let links = vec![link("a.ifc", 100_000.0), link("dir/b.ifc", -100_000.0)];
        assert_eq!(links[1].name(), "b.ifc");
        let b = bounds_with_links(&scene, &links);
        let own = bimble_render::Sphere::of(&scene);
        for x in [100_000.0f32, -100_000.0] {
            let c = own.center + glam::Vec3::new(x, 0.0, 0.0);
            assert!((c - b.center).length() + own.radius <= b.radius * 1.0001);
        }
    }

    /// What a browser gives (a name and bytes) loads like the file on disk.
    #[test]
    fn files_load_from_bytes_like_from_disk() {
        let ifc = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../bimble-ifc/tests/fixtures/curves.ifc"
        );
        let bytes = std::fs::read(ifc).unwrap();
        let (a, m) = load_bytes("curves.IFC", &bytes).unwrap();
        let (b, _) = load_any(Path::new(ifc)).unwrap();
        assert!(m.is_none());
        assert_eq!(a.elements.len(), b.elements.len());
        let model = bimble_core::demo::concrete_frame(&Default::default());
        let text = bimble_core::file::to_string(&model);
        let (s, m) = load_bytes("frame.bimble", text.as_bytes()).unwrap();
        assert_eq!(m.unwrap(), model);
        assert_eq!(s.elements.len(), model.elements.len());
        // Not UTF-8: an error, not a panic.
        assert!(load_bytes("x.bimble", &[0xff, 0xfe, 0x00]).is_err());
    }
}
