//! Bimble viewer: a window around `bimble-render`.
//!
//! The 3D view is rendered offscreen by the same renderer as `bimble render`, then shown as an
//! egui image. It is only redrawn when something changes (camera, visibility, selection, size),
//! which keeps old laptops cool and quiet.

use bimble_core::MaterialCategory;
use bimble_geom::Scene;
use bimble_render::{
    Camera, ClipBox, DrawOptions, GpuScene, GraphicStyle, Overlay, Renderer, Style, Target,
};
use eframe::egui::{self, Color32, PointerButton, Sense, Vec2};
use eframe::egui_wgpu;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, channel};
use std::time::Duration;
// std's Instant panics in browsers; web-time is std's on the desktop.
use bimble_ifc::read::Progress;
use web_time::Instant;

mod big;
mod collab;
mod coords;
mod cutlines;
mod edit;
mod files;
mod home;
mod icons;
mod layout;
mod links;
mod materials_page;
mod measure;
mod project_page;
mod ribbon;
mod schedules;
mod section;
mod settings;
mod share;
mod snap;
mod state;
mod style_editor;
mod ui_theme;
mod viewcube;
mod viewing;
mod views;
mod visibility;

const SAMPLES: u32 = 4;
/// While moving on a slow machine, a far cell's stand-in is drawn when its error spans at most
/// this many pixels (ADR 0036 #9: the 415 MB model's whole frame 29 → 1.3 ms at 3 px).
const MOVING_PROXY_PX: f32 = 3.0;
/// Models smaller than this (drawn triangles) never build stand-ins.
const PROXY_MIN_TRIANGLES: usize = 1_000_000;
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

/// The project information (ADR 0030): the model's, or an IFC's side file's.
fn project_of(
    loaded: &Option<Loaded>,
    side: &Option<viewing::SideViews>,
) -> BTreeMap<String, String> {
    if let Some(ed) = loaded.as_ref().and_then(|l| l.edit.as_ref()) {
        return ed.model.project.clone();
    }
    side.as_ref().map(|s| s.project.clone()).unwrap_or_default()
}

/// The plan settings as drawing options; the project name is the model's file name.
fn plan_options(
    p: &PlanSettings,
    path: &std::path::Path,
    style: bimble_core::style::GraphicStyle,
    view: Option<bimble_core::view::View>,
    views: &[bimble_core::view::View],
    project: &BTreeMap<String, String>,
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
        template: p.template.with_project(project),
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
    /// Its elements' bounds as a tree, for picking.
    bvh: bimble_geom::bvh::Bvh,
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

/// A question the user must answer before Bimble goes on (ADR 0028).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Confirm {
    /// The window is closing with unsaved work.
    Close,
    /// A model or views file is opening over unsaved work (`pending_open`).
    Open,
    /// Reset every standard to the default.
    ResetStandards,
}

struct Turn {
    from: (f32, f32),
    to: (f32, f32),
    start: Instant,
}

/// Start the viewer in a browser page, drawing into `canvas` (WebGPU, or WebGL2 where the
/// browser has no WebGPU). Files are opened with the file picker or by dropping them on the page,
/// or named in the address: `?url=<model.ifc>` opens that model, `&gl=1` forces WebGL2. The page
/// start-up is told in `window.bimbleStates` (see `tell`), for benchmarks.
#[cfg(target_arch = "wasm32")]
pub async fn start_web(canvas: web_sys::HtmlCanvasElement) -> Result<(), String> {
    let query = web_sys::window()
        .and_then(|w| w.location().search().ok())
        .unwrap_or_default();
    let param = |k: &str| {
        web_sys::UrlSearchParams::new_with_str(&query)
            .ok()
            .and_then(|p| p.get(k))
    };
    let open_url = param("url");
    let mut options = eframe::WebOptions::default();
    // Browsers can expose WebGPU without an adapter behind it (Chrome on many Linux machines):
    // wgpu would pick WebGPU and fail, so ask for an adapter first and use WebGL2 without one.
    if (param("gl").is_some() || !has_webgpu_adapter().await)
        && let eframe::egui_wgpu::WgpuSetup::CreateNew(c) = &mut options.wgpu_options.wgpu_setup
    {
        c.instance_descriptor.backends = eframe::wgpu::Backends::GL;
    }
    eframe::WebRunner::new()
        .start(
            canvas,
            options,
            Box::new(move |cc| {
                let rs = cc
                    .wgpu_render_state
                    .as_ref()
                    .ok_or("the viewer needs the wgpu renderer")?;
                settings::Settings::load().apply_look(&cc.egui_ctx);
                state::guard_page_unload();
                let app = Viewer::new(rs);
                let backend = format!("{:?}", rs.adapter.get_info().backend);
                tell("started", Some(&backend));
                if let Some(url) = open_url {
                    open_url_later(url, app.files.sender(), cc.egui_ctx.clone());
                }
                Ok(Box::new(app))
            }),
        )
        .await
        .map_err(|e| format!("{e:?}"))
}

/// Fetch the model at `url` and open it as if picked.
#[cfg(target_arch = "wasm32")]
fn open_url_later(url: String, tx: std::sync::mpsc::Sender<files::Picked>, ctx: egui::Context) {
    wasm_bindgen_futures::spawn_local(async move {
        match fetch_bytes(&url).await {
            Ok(bytes) => {
                tell("fetched", None);
                let path = url.split(['?', '#']).next().unwrap_or_default();
                let _ = tx.send(files::Picked {
                    purpose: files::Purpose::Open,
                    name: path.rsplit('/').next().unwrap_or_default().to_owned(),
                    path: None,
                    bytes: Some(bytes),
                });
                ctx.request_repaint();
            }
            Err(e) => tell("failed", Some(&e)),
        }
    });
}

#[cfg(target_arch = "wasm32")]
async fn fetch_bytes(url: &str) -> Result<Vec<u8>, String> {
    use wasm_bindgen::JsCast;
    use wasm_bindgen_futures::JsFuture;
    let err = |e: wasm_bindgen::JsValue| format!("{url}: {e:?}");
    let window = web_sys::window().ok_or("no window")?;
    let resp: web_sys::Response = JsFuture::from(window.fetch_with_str(url))
        .await
        .map_err(err)?
        .dyn_into()
        .map_err(err)?;
    if !resp.ok() {
        return Err(format!("{url}: HTTP {}", resp.status()));
    }
    let buf = JsFuture::from(resp.array_buffer().map_err(err)?)
        .await
        .map_err(err)?;
    Ok(js_sys::Uint8Array::new(&buf).to_vec())
}

/// Append `{state, at, message}` to `window.bimbleStates` (`at`: epoch milliseconds, so a parent
/// page can put it on its own clock even while parsing blocks the page's only thread) and log it.
/// States: "started" (message: the graphics backend), "fetched", "first-frame", "failed".
#[cfg(target_arch = "wasm32")]
fn tell(state: &str, message: Option<&str>) {
    use wasm_bindgen::{JsCast, JsValue};
    let Some(w) = web_sys::window() else { return };
    let at = w.performance().map_or(0.0, |p| p.time_origin() + p.now());
    let entry = js_sys::Object::new();
    let set = |o: &JsValue, k: &str, v: JsValue| {
        let _ = js_sys::Reflect::set(o, &k.into(), &v);
    };
    set(&entry, "state", state.into());
    set(&entry, "at", at.into());
    if let Some(m) = message {
        set(&entry, "message", m.into());
    }
    let list = js_sys::Reflect::get(&w, &"bimbleStates".into())
        .ok()
        .and_then(|v| v.dyn_into::<js_sys::Array>().ok())
        .unwrap_or_else(|| {
            let a = js_sys::Array::new();
            set(&w, "bimbleStates", a.clone().into());
            a
        });
    list.push(&entry);
    log::info!("bimble: {state} {}", message.unwrap_or_default());
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
            settings::Settings::load().apply_look(&cc.egui_ctx);
            let mut app = Viewer::new(rs);
            app.opts.style = initial_style;
            app.style_baseline = app.opts.style.clone();
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

/// The next "leave out what is smaller than this many pixels" while moving, after a frame of `dt`
/// seconds, aiming at `fps`: up by half while slower than 15 % over the target, down by a third
/// while 30 % under it, to 0 (everything) at the bottom. A gap over 0.25 s is a pause, not a
/// slow frame.
///
/// Frames are timed between presents, so the screen's refresh caps them: `floor` is the
/// shortest frame seen while moving. Above 60 fps the target is no faster than that floor (a
/// 120 fps target on a 60 Hz screen means 60), and a frame at the floor counts as fast: it is
/// waiting for the screen, not drawing, and without this one slow frame would raise the
/// threshold for good.
fn next_moving_px(px: f32, dt: f32, fps: u32, floor: f32) -> f32 {
    let want = 1.0 / fps.max(1) as f32;
    let target = want.max(floor.min(1.0 / 60.0));
    // Waiting for the screen: the floor looks like a refresh (50 Hz or faster) that the target
    // asks more than.
    let screen_bound = floor <= 0.02 && floor * 1.1 >= want && dt <= floor * 1.1;
    if dt >= 0.25 {
        px
    } else if dt > target * 1.15 {
        (px.max(0.5) * 1.5).min(48.0)
    } else if dt < target * 0.7 || screen_bound {
        let p = px / 1.5;
        if p < 0.5 { 0.0 } else { p }
    } else {
        px
    }
}

/// Load a .ifc or .bimble file as a scene.
pub fn load_scene(path: &Path) -> Result<Scene, String> {
    load_any(path).map(|(s, _)| s)
}

/// `load_scene`, an IFC file reporting each step to `progress`.
#[cfg(not(target_arch = "wasm32"))]
fn load_scene_progress(path: &Path, progress: &Progress) -> Result<Scene, String> {
    if path
        .extension()
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"))
    {
        bimble_ifc::read::read_file_progress(path, progress).map_err(|e| e.to_string())
    } else {
        load_scene(path)
    }
}

/// A scene, and for a .bimble file the model itself (it can then be edited).
fn load_any(path: &Path) -> Result<(Scene, Option<bimble_core::Model>), String> {
    if bimble_geom::viewfile::is_named(&path.to_string_lossy()) {
        let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
        return load_bytes(&path.to_string_lossy(), bytes);
    }
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
fn load_bytes(name: &str, bytes: Vec<u8>) -> Result<(Scene, Option<bimble_core::Model>), String> {
    // A prepared viewing file (ADR 0040): straight to a scene, read-only.
    if bimble_geom::viewfile::is_named(name) {
        return bimble_geom::viewfile::read_bytes(&bytes)
            .map(|s| (s, None))
            .map_err(|e| e.to_string());
    }
    let is_ifc = name.to_ascii_lowercase().ends_with(".ifc");
    if is_ifc {
        bimble_ifc::read::read_bytes(bytes)
            .map(|s| (s, None))
            .map_err(|e| e.to_string())
    } else {
        let m = bimble_core::file::from_bytes(&bytes).map_err(|e| e.to_string())?;
        Ok((Scene::from_model(&m), Some(m)))
    }
}

/// The editable .bimble copy next to an IFC file: same stem (ADR 0032), or
/// "<stem> (imported).bimble", "<stem> (imported 2).bimble"… when that model already exists, so
/// Save never overwrites one.
fn editable_copy_path(ifc: &Path) -> PathBuf {
    let stem = ifc
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "model".into());
    [
        format!("{stem}.bimble"),
        format!("{stem} (imported).bimble"),
    ]
    .into_iter()
    .chain((2u64..).map(|n| format!("{stem} (imported {n}).bimble")))
    .map(|n| ifc.with_file_name(n))
    .find(|p| !p.exists())
    .expect("an unused name")
}

/// An IFC file imported as an editable model (ADR 0032). Ctrl S then saves the copy.
#[cfg(not(target_arch = "wasm32"))]
fn import_any(path: &Path) -> Result<(Scene, bimble_core::Model, Vec<String>), String> {
    let text = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let (m, rep) = bimble_ifc::import::import_str(&text)?;
    let mut m = m;
    m.project_name = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("Imported model")
        .to_owned();
    m.validate()
        .map_err(|e| format!("imported model invalid: {e}"))?;
    Ok((Scene::from_model(&m), m, rep.lines()))
}

/// An import's outcome: the model, and its report to show.
fn imported(scene: Scene, m: bimble_core::Model, t: Instant, report: Vec<String>) -> big::Outcome {
    big::Outcome::Ready {
        scene,
        model: Some(m),
        time: t.elapsed(),
        partial: None,
        report,
    }
}

/// An IFC file's bytes imported as an editable model (browser).
fn import_bytes(
    name: &str,
    bytes: &[u8],
) -> Result<(Scene, bimble_core::Model, Vec<String>), String> {
    let text = String::from_utf8_lossy(bytes);
    let (m, rep) = bimble_ifc::import::import_str(&text)?;
    let mut m = m;
    m.project_name = name.strip_suffix(".ifc").unwrap_or(name).to_owned();
    m.validate()
        .map_err(|e| format!("imported model invalid: {e}"))?;
    Ok((Scene::from_model(&m), m, rep.lines()))
}

/// A file's content to load on a later frame, so "Loading…" is drawn before the browser's only
/// thread is busy parsing.
struct ParseLater {
    name: String,
    bytes: Vec<u8>,
    link: bool,
    /// An IFC file imported as an editable .bimble copy (ADR 0032).
    import: bool,
    /// Read even though reading alone exceeds the memory budget (ADR 0033).
    read_anyway: bool,
    frames: u8,
    tx: std::sync::mpsc::Sender<LoadResult>,
}

type LoadResult = Result<big::Outcome, String>;

struct Loading {
    path: PathBuf,
    rx: Receiver<LoadResult>,
    /// An IFC imported as an editable copy: not on disk yet, so the editor starts dirty.
    imported: bool,
    /// How far it has come, written by the loading thread.
    progress: std::sync::Arc<Progress>,
    started: Instant,
    /// Each step as the UI saw it begin, since `started` (to within a frame): the log's timings.
    steps: Vec<(bimble_ifc::read::Stage, Duration)>,
}

impl Loading {
    fn new(path: PathBuf, rx: Receiver<LoadResult>, imported: bool) -> Loading {
        log::info!("loading {}", path.display());
        Loading {
            path,
            rx,
            imported,
            progress: Default::default(),
            started: Instant::now(),
            steps: Vec::new(),
        }
    }

    /// Note a step that has begun since the last frame.
    fn observe(&mut self) {
        let (_, total) = self.progress.counts();
        let stage = self.progress.stage();
        if total == 0 || self.steps.last().is_some_and(|(s, _)| *s == stage) {
            return;
        }
        let t = self.started.elapsed();
        log::debug!(
            "{}: {} ({:.2} s)",
            self.path.display(),
            stage.label(),
            t.as_secs_f64()
        );
        self.steps.push((stage, t));
    }

    /// How long it took, and each step: "3.21 s (Reading the file 1.40 s, …)".
    fn timing(&self) -> String {
        let end = self.started.elapsed();
        let mut s = format!("{:.2} s", end.as_secs_f64());
        if !self.steps.is_empty() {
            let ends = self.steps.iter().skip(1).map(|(_, t)| *t).chain([end]);
            let parts: Vec<String> = (self.steps.iter().zip(ends))
                .map(|((stage, a), b)| format!("{} {:.2} s", stage.label(), (b - *a).as_secs_f64()))
                .collect();
            s += &format!(" ({})", parts.join(", "));
        }
        s
    }

    /// What the status line says while it loads: the file, the step, how far, how long.
    /// `None` for its first `PROGRESS_AFTER`: a quick load shows nothing.
    fn shown(&self, verb: &str) -> Option<LoadShown> {
        let t = self.started.elapsed();
        if t < PROGRESS_AFTER {
            return None;
        }
        let name = self.path.file_name().map_or_else(
            || self.path.display().to_string(),
            |n| n.to_string_lossy().into_owned(),
        );
        let (_, total) = self.progress.counts();
        // Nothing reported (a .bimble file, an import): no step and no percentage to show.
        let fraction = (total > 0).then(|| self.progress.overall());
        let secs = t.as_secs();
        let mut text = format!("{verb} {name}");
        if let Some(f) = fraction {
            text += &format!(" · {} {:.0} %", self.progress.stage().label(), f * 100.0);
        } else {
            text += "…";
        }
        text += &format!("  {}:{:02}", secs / 60, secs % 60);
        Some(LoadShown { text, fraction })
    }
}

/// The load, shown big over the 3D view: a card with the file, the step, how far and how long,
/// and a bar. Without a fraction a segment sweeps across: something is happening, how much is
/// left is not known. In a browser the page's one thread reads the file, so the card can't move
/// while it reads: it says so.
fn progress_card(ctx: &egui::Context, view: egui::Rect, shown: &LoadShown) {
    let w = (view.width() - 48.0).clamp(160.0, 520.0);
    egui::Area::new(egui::Id::unique("load progress"))
        .order(egui::Order::Foreground)
        .fixed_pos(egui::pos2(
            view.center().x - w / 2.0,
            view.center().y - 60.0,
        ))
        .interactable(false)
        .show(ctx, |ui| {
            egui::Frame::new()
                .fill(ui_theme::panel_bg())
                .stroke(egui::Stroke::new(1.0, ui_theme::accent()))
                .corner_radius(8.0)
                .inner_margin(18.0)
                .show(ui, |ui| {
                    ui.set_width(w - 36.0);
                    ui.label(
                        egui::RichText::new(&shown.text)
                            .size(ui_theme::text_body() + 4.0)
                            .color(ui_theme::text())
                            .strong(),
                    );
                    ui.add_space(10.0);
                    let (track, _) = ui.allocate_exact_size(
                        egui::vec2(ui.available_width(), 10.0),
                        egui::Sense::hover(),
                    );
                    let painter = ui.painter();
                    painter.rect_filled(track, 5.0, ui_theme::border());
                    let (a, b) = match shown.fraction {
                        Some(f) => (0.0, f.clamp(0.0, 1.0)),
                        None => {
                            let t = ctx.input(|i| i.time) as f32;
                            let x = (t * 0.6).fract() * 1.25 - 0.25;
                            (x.max(0.0), (x + 0.25).min(1.0))
                        }
                    };
                    let fill = egui::Rect::from_x_y_ranges(
                        track.left() + a * track.width()..=track.left() + b * track.width(),
                        track.y_range(),
                    );
                    painter.rect_filled(fill, 5.0, ui_theme::accent());
                    if cfg!(target_arch = "wasm32") {
                        ui.add_space(8.0);
                        ui.label(
                            egui::RichText::new(
                                "The browser reads the file on this page's one thread: the \
                                 page holds still until it is done.",
                            )
                            .size(ui_theme::text_small())
                            .color(ui_theme::text_muted()),
                        );
                    }
                });
        });
}

/// A load is shown once it takes longer than this: a quick one would only flash. In a browser
/// at once: the page can't redraw once the file is being read.
const PROGRESS_AFTER: Duration = if cfg!(target_arch = "wasm32") {
    Duration::ZERO
} else {
    Duration::from_millis(250)
};

/// A load in progress as shown: a card over the 3D view and a line in the status bar.
struct LoadShown {
    text: String,
    /// Done, 0 to 1; `None` when the load reports no progress (the bar then sweeps).
    fraction: Option<f32>,
}

struct Loaded {
    path: PathBuf,
    scene: Scene,
    /// Its elements' bounds as a tree (picking, snapping; later culling and clashes). Rebuilt
    /// with the scene: 15 ms for 30 000 elements.
    bvh: bimble_geom::bvh::Bvh,
    load_time: Duration,
    /// Part of the file shown, when it was too large to show whole (ADR 0033).
    partial: Option<String>,
    bounds: bimble_render::Sphere,
    classes: BTreeMap<String, usize>,
    storeys: BTreeMap<String, usize>,
    categories: BTreeMap<MaterialCategory, usize>,
    /// Elements per family and material name (the Visibility panel splits a family by these).
    materials: BTreeMap<(MaterialCategory, String), usize>,
    /// Each element's family as read, before the standards move materials to another family;
    /// and the moves applied to `scene` (None: none yet).
    imported: Vec<MaterialCategory>,
    families_applied: Option<BTreeMap<String, MaterialCategory>>,
    /// The model of a .bimble file, editable.
    edit: Option<edit::Editor>,
    /// Simplified stand-ins for far cells (ADR 0036 #9), built the first time this machine
    /// misses the frame-rate target while moving; dropped when the scene changes.
    proxies: Option<std::sync::Arc<bimble_render::GpuProxies>>,
}

type Counts = (
    BTreeMap<String, usize>,
    BTreeMap<String, usize>,
    BTreeMap<MaterialCategory, usize>,
    BTreeMap<(MaterialCategory, String), usize>,
);

/// Elements per IFC class, storey and material family.
fn counts(scene: &Scene) -> Counts {
    let mut classes = BTreeMap::new();
    let mut storeys = BTreeMap::new();
    let mut categories = BTreeMap::new();
    let mut materials = BTreeMap::new();
    for e in &scene.elements {
        *classes.entry(e.class.clone()).or_default() += 1;
        *storeys.entry(e.storey.clone()).or_default() += 1;
        *categories.entry(e.category).or_default() += 1;
        *materials
            .entry((e.category, e.material.clone()))
            .or_default() += 1;
    }
    (classes, storeys, categories, materials)
}

impl Loaded {
    /// Put the elements in the families the standards say (a material moved to another family),
    /// when that changed: true if it did (then upload and draw again).
    fn apply_families(&mut self, style: &GraphicStyle) -> bool {
        if self.families_applied.as_ref() == Some(&style.material_families) {
            return false;
        }
        // Elements added since (none yet: a scene is replaced whole) are as imported.
        let n = self.scene.elements.len();
        self.imported.truncate(n);
        let k = self.imported.len();
        self.imported
            .extend(self.scene.elements[k..].iter().map(|e| e.category));
        let mut changed = false;
        for (e, &c) in self.scene.elements.iter_mut().zip(&self.imported) {
            let f = style.family(&e.material, c);
            changed |= e.category != f;
            e.category = f;
        }
        if changed {
            (self.classes, self.storeys, self.categories, self.materials) = counts(&self.scene);
        }
        self.families_applied = Some(style.material_families.clone());
        changed
    }
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
    /// Highlights (the element under the cursor, a selected linked element), drawn over the
    /// kept picture: changing them doesn't draw the model again (ADR 0036 #3).
    overlay: Option<(Overlay, egui::TextureId)>,
    /// Pictures drawn while moving on a machine that can't keep the frame-rate target:
    /// without MSAA (ADR 0036 #4), by a single-sample renderer into their own target. Made
    /// when first needed; `lean_shown` while the picture on screen is one.
    lean: Option<(Renderer, Target, egui::TextureId)>,
    lean_shown: bool,
    needs_upload: bool,
    needs_render: bool,
    needs_overlay: bool,
    /// When the last pictures were drawn (the last second's), for the frame rate in the footer.
    drawn_at: std::collections::VecDeque<Instant>,
    /// The memory share the open model was read with (Settings shows a change and offers to
    /// reopen with it).
    opened_share: f64,
    last_render: Duration,

    camera: Camera,
    opts: DrawOptions,
    hidden_categories: BTreeSet<MaterialCategory>,
    /// Materials hidden, by name (a family split up in the Visibility panel).
    hidden_materials: BTreeSet<String>,
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
    /// The point under the pointer (world mm): on what it shows, or on a plan's cut (ADR 0045).
    pointer_world: Option<[f64; 3]>,
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
    /// The wheel's last zoom pivot: the pointer position, the camera after that zoom, the pivot.
    zoom_pivot: Option<(egui::Pos2, Camera, glam::Vec3)>,
    /// What the current drag turns the view about (picked when it started).
    orbit_pivot: Option<glam::Vec3>,
    /// Navigation (ADR 0035): the camera last drawn, when the last moving frame was drawn,
    /// how small (pixels) a thing may be and still be drawn while moving, and whether the
    /// picture on screen left things out (redrawn whole once the view stops).
    drawn_camera: Option<Camera>,
    moving_frame_at: Option<Instant>,
    moving_px: f32,
    /// The shortest frame seen while moving (s): the screen's refresh, at best.
    frame_floor: f32,
    simplified: bool,
    /// This movement has been slower than the target: its frames drop MSAA until it stops.
    moving_lean: bool,
    /// Answers of GPU occlusion queries may still change the last picture.
    occlusion_waiting: bool,
    /// The backend handles occlusion queries (native ones only: ADR 0036 #1).
    gpu_queries: bool,
    /// A movement has been slower than the target since the viewer started: the GPU is
    /// short, so occlusion culling (CPU time for GPU time) is worth it (ADR 0036 #5).
    slow_gpu: bool,
    /// The first GPU error caught (wgpu would panic): set from wgpu's error callback.
    gpu_error: std::sync::Arc<std::sync::Mutex<Option<String>>>,
    /// After a GPU error: the optional GPU paths are off (screen edges, GPU queries and culling).
    gpu_safe: bool,

    /// Linked (reference) models, read-only (#39), one being loaded, and the one whose
    /// differences with ours are coloured, if any.
    links: Vec<Linked>,
    linking: Option<Loading>,
    compare_with: Option<usize>,
    /// Linked models hidden in the active view, by file name.
    hidden_links: BTreeSet<String>,
    /// An element of a linked model selected (#39): the link, the element, and its edges
    /// uploaded for the highlight. Read-only.
    link_selected: Option<(usize, usize)>,
    link_selected_gpu: Option<GpuScene>,
    /// Model to link once ours is loaded (`bimble view --link`).
    pending_link: Option<PathBuf>,

    /// Plan export window and its settings.
    plan_window: bool,
    plan: PlanSettings,

    /// The Standards panel (graphic style) on the right (ADR 0027).
    standards_open: bool,
    /// The standards as last loaded or saved (or the default): what "changed" is measured
    /// against, and what Revert goes back to (ADR 0028).
    style_baseline: GraphicStyle,
    /// The plan template as last loaded (or the default), likewise for the plan options.
    template_baseline: bimble_drawing::template::PlanTemplate,
    /// A question waiting for an answer: close with unsaved work, reset the standards.
    confirm: Option<Confirm>,
    /// The user answered "close" to the question: let the window close.
    allow_close: bool,
    /// The file waiting for the answer to `Confirm::Open`.
    pending_open: Option<files::Picked>,
    standards_query: String,
    /// The ribbon's open tab, the command palette, tabs of open views and sheets.
    ribbon_tab: ribbon::Tab,
    palette: Option<ribbon::Palette>,
    open_tabs: Vec<ribbon::TabRef>,
    /// A schedule open in the main view, and the schedules' row counts per scene revision.
    schedule: Option<schedules::OpenSchedule>,
    /// The project information page, when open in the main view (ADR 0030).
    project_page: Option<project_page::ProjectPage>,
    /// The material browser, when open in the main view (ADR 0031).
    materials_page: Option<materials_page::MaterialsPage>,
    schedule_counts: Option<(u64, Vec<(String, usize)>)>,
    /// File the style was loaded from or saved to.
    style_path: Option<PathBuf>,
    style_message: Option<String>,

    /// Editing panel (for .bimble models) and the history window.
    history_window: bool,
    history_view: Option<HistoryView>,

    files: files::Files,
    parse_later: Option<ParseLater>,
    /// A model too large for this machine, waiting for the user to choose (ADR 0033).
    ask: Option<(PathBuf, Box<big::Ask>)>,
    /// The user's settings (kept between sessions), and whether their window is open.
    settings: settings::Settings,
    settings_window: bool,
    about_window: bool,
    /// Manage links (#58).
    links_window: bool,
    /// Home (#52) over the main view.
    home_open: bool,
    /// Recent files' previews, read once (#52); None: none on disk.
    previews: std::collections::HashMap<String, Option<egui::TextureHandle>>,
    /// The file whose preview to keep once its first picture is drawn.
    preview_for: Option<String>,
    /// When this model last synchronized (or was opened shared): the reminder (ADR 0038).
    synced_at: Option<Instant>,
    /// What the last Synchronize or Shared folder… said.
    collab_message: Option<String>,
    /// Elements added to the selection with Ctrl/Cmd (#54), besides `selected`.
    also_selected: BTreeSet<usize>,
    /// Hidden in the active view by GlobalId, or all but these (#54; saved with the view).
    hidden_ids: BTreeSet<String>,
    isolated_ids: BTreeSet<String>,
    /// Hidden or isolated for now (#54).
    temporary: Option<visibility::Temporary>,
    /// Reveal hidden: what is hidden by element is drawn, marked, and can be picked (#54).
    reveal_hidden: bool,
    /// A reference plan being reloaded from another file: its id (the new lines replace it).
    reference_replace: Option<bimble_core::GlobalId>,
    /// A linked model being reloaded from another file: its index (the new one replaces it).
    relink: Option<usize>,
    /// What a load had to say (an IFC import's report), shown until closed.
    report: Option<(PathBuf, Vec<String>)>,
    /// Browser: meshing the chosen part, on a later frame (after "Loading…" is drawn).
    #[cfg_attr(not(target_arch = "wasm32"), allow(dead_code))]
    mesh_later: Option<(u8, Box<dyn FnOnce()>)>,

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
            // The browser's WebGPU names no adapter: the backend alone then.
            adapter: if info.name.is_empty() {
                format!("{:?}", info.backend)
            } else {
                format!("{} ({:?})", info.name, info.backend)
            },
            renderer: Renderer::new(&rs.device, SAMPLES),
            loading: None,
            loaded: None,
            error: None,
            gpu_scene: None,
            target: None,
            texture: None,
            overlay: None,
            lean: None,
            lean_shown: false,
            needs_upload: false,
            needs_render: true,
            needs_overlay: false,
            drawn_at: Default::default(),
            opened_share: 0.0,
            last_render: Duration::ZERO,
            camera: Camera::default(),
            opts: DrawOptions::default(),
            hidden_categories: BTreeSet::new(),
            hidden_materials: BTreeSet::new(),
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
            pointer_world: None,
            place_targets: None,
            place_snap: None,
            cut_cache: None,
            hovered: None,
            hover_gpu: None,
            hover_at: None,
            zoom_pivot: None,
            orbit_pivot: None,
            drawn_camera: None,
            moving_frame_at: None,
            moving_px: 0.0,
            frame_floor: f32::INFINITY,
            simplified: false,
            moving_lean: false,
            // A software renderer (llvmpipe, SwiftShader: no GPU driver) is slow from the
            // start: no need to wait for slow frames. Any real GPU is judged by its frames.
            slow_gpu: info.device_type == eframe::wgpu::DeviceType::Cpu,
            gpu_error: bimble_render::catch_gpu_errors(&rs.device),
            gpu_safe: false,
            occlusion_waiting: false,
            // Native backends only: wgpu 30's GL backend reads results too early, and its
            // browser-WebGPU backend drops the pass's occlusion query set (the pass is then
            // invalid and the view black).
            gpu_queries: matches!(
                rs.adapter.get_info().backend,
                eframe::wgpu::Backend::Metal
                    | eframe::wgpu::Backend::Vulkan
                    | eframe::wgpu::Backend::Dx12
            ),
            links: Vec::new(),
            linking: None,
            compare_with: None,
            hidden_links: BTreeSet::new(),
            link_selected: None,
            link_selected_gpu: None,
            pending_link: None,
            plan_window: false,
            plan: PlanSettings::default(),
            standards_open: false,
            style_baseline: GraphicStyle::default(),
            template_baseline: Default::default(),
            confirm: None,
            allow_close: false,
            pending_open: None,
            standards_query: String::new(),
            ribbon_tab: ribbon::Tab::default(),
            palette: None,
            open_tabs: Vec::new(),
            schedule: None,
            project_page: None,
            materials_page: None,
            schedule_counts: None,
            style_path: None,
            style_message: None,
            history_window: false,
            history_view: None,
            files: files::Files::default(),
            parse_later: None,
            ask: None,
            settings: settings::Settings::load(),
            settings_window: false,
            about_window: false,
            links_window: false,
            home_open: false,
            previews: Default::default(),
            preview_for: None,
            synced_at: None,
            collab_message: None,
            also_selected: BTreeSet::new(),
            hidden_ids: BTreeSet::new(),
            isolated_ids: BTreeSet::new(),
            temporary: None,
            reveal_hidden: false,
            reference_replace: None,
            relink: None,
            report: None,
            mesh_later: None,
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
        let loading = Loading::new(PathBuf::from(&p.name), rx, false);
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
            import: false,
            read_anyway: false,
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
        let is_ifc = job.name.to_ascii_lowercase().ends_with(".ifc");
        let r = if job.import {
            import_bytes(&job.name, &job.bytes).map(|(s, m, report)| imported(s, m, t, report))
        } else if is_ifc && !job.link {
            big::load_ifc(
                big::Source::Bytes(job.bytes),
                big::memory_budget(self.settings.memory_share, self.held()),
                job.read_anyway,
                &Progress::default(),
            )
        } else {
            load_bytes(&job.name, job.bytes).map(|(s, m)| {
                let m = if job.link { None } else { m };
                big::Outcome::ready(s, m, t.elapsed())
            })
        };
        let _ = job.tx.send(r);
        ctx.request_repaint();
    }

    /// Files picked in dialogs (or dropped), by what they are for. Opening a model or a views
    /// file over unsaved work asks first (ADR 0028), as closing does.
    fn handle_picked(&mut self, ctx: &egui::Context) {
        while self.confirm.is_none() {
            let Some(p) = self.files.poll() else { break };
            // A value typed on a page but not yet stored is unsaved work too.
            self.commit_pages();
            let replaces = (p.purpose == files::Purpose::Open
                || p.purpose == files::Purpose::ImportIfc)
                && ![".dxf", ".pdf", ".dwg"]
                    .iter()
                    .any(|x| p.name.to_ascii_lowercase().ends_with(x));
            let s = self.snapshot();
            if replaces && (s.model_dirty || s.views_dirty) {
                self.pending_open = Some(p);
                self.confirm = Some(Confirm::Open);
                break;
            }
            self.handle_one(p, ctx);
        }
    }

    fn handle_one(&mut self, p: files::Picked, ctx: &egui::Context) {
        {
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
                return;
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
                return;
            }
            match p.purpose {
                files::Purpose::Reference
                | files::Purpose::ReferenceDxf
                | files::Purpose::ReferencePdf => self.add_reference(p),
                files::Purpose::Open | files::Purpose::Link => self.open_picked(p, ctx),
                files::Purpose::ImportIfc => self.import_picked(p, ctx),
                files::Purpose::PlanTemplate => {
                    let r = p.content().and_then(|b| {
                        bimble_drawing::template::PlanTemplate::from_json(&String::from_utf8_lossy(
                            &b,
                        ))
                        .map_err(|e| format!("{}: {e}", p.name))
                    });
                    match r {
                        Ok(t) => {
                            self.template_baseline = t.clone();
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
                            self.style_baseline = s.clone();
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

    /// Import an IFC file as an editable .bimble copy (ADR 0032): from its path on a thread
    /// (desktop), or from its bytes on a later frame (browser). The copy is saved next to the
    /// file, with the same stem.
    fn import_picked(&mut self, p: files::Picked, ctx: &egui::Context) {
        if let (Some(path), None) = (&p.path, &p.bytes) {
            self.import_ifc(path.clone(), ctx);
            return;
        }
        let Some(bytes) = p.bytes else { return };
        let (tx, rx) = channel();
        self.loading = Some(Loading::new(
            editable_copy_path(Path::new(&p.name)),
            rx,
            true,
        ));
        self.error = None;
        self.parse_later = Some(ParseLater {
            name: p.name,
            bytes,
            link: false,
            import: true,
            read_anyway: false,
            frames: 2,
            tx,
        });
        ctx.request_repaint();
    }

    #[cfg(target_arch = "wasm32")]
    fn import_ifc(&mut self, _path: PathBuf, _ctx: &egui::Context) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn import_ifc(&mut self, path: PathBuf, ctx: &egui::Context) {
        let (tx, rx) = channel();
        let p = path.clone();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let r = import_any(&p).map(|(s, m, report)| imported(s, m, t, report));
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.loading = Some(Loading::new(editable_copy_path(&path), rx, true));
        self.error = None;
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
        let share = self.settings.memory_share;
        self.opened_share = share;
        let loading = Loading::new(path, rx, false);
        let progress = loading.progress.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let is_ifc = p.extension().is_some_and(|x| x.eq_ignore_ascii_case("ifc"));
            let r = if is_ifc {
                big::load_ifc(
                    big::Source::File(p.clone()),
                    big::memory_budget(share, 0),
                    false,
                    &progress,
                )
            } else {
                load_any(&p).map(|(s, m)| big::Outcome::ready(s, m, t.elapsed()))
            };
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.loading = Some(loading);
        self.error = None;
    }

    /// Load another model as a reference, next to ours.
    #[cfg(not(target_arch = "wasm32"))]
    fn link(&mut self, path: PathBuf, ctx: &egui::Context) {
        let (tx, rx) = channel();
        let p = path.clone();
        let ctx = ctx.clone();
        let linking = Loading::new(path, rx, false);
        let progress = linking.progress.clone();
        std::thread::spawn(move || {
            let t = Instant::now();
            let r = load_scene_progress(&p, &progress)
                .map(|s| big::Outcome::ready(s, None, t.elapsed()));
            let _ = tx.send(r);
            ctx.request_repaint();
        });
        self.linking = Some(linking);
    }

    /// The load to show, if one is running long enough: opening first, then linking.
    fn load_shown(&self) -> Option<LoadShown> {
        let opening = self
            .loading
            .as_ref()
            .and_then(|l| l.shown(if l.imported { "Importing" } else { "Opening" }));
        opening.or_else(|| self.linking.as_ref().and_then(|l| l.shown("Linking")))
    }

    fn poll_linking(&mut self) {
        let Some(l) = &self.linking else { return };
        let Ok(result) = l.rx.try_recv() else { return };
        let l = self.linking.take().unwrap();
        match &result {
            Ok(big::Outcome::Ready { scene, .. }) => log::info!(
                "linked {}: {} elements, {} triangles, {}",
                l.path.display(),
                scene.elements.len(),
                scene.triangle_count(),
                l.timing()
            ),
            Ok(big::Outcome::Ask(_)) => {}
            Err(e) => log::error!("{}: {e}", l.path.display()),
        }
        let path = l.path;
        let Some(main) = &mut self.loaded else { return };
        match result {
            Ok(big::Outcome::Ask(_)) => {}
            Ok(big::Outcome::Ready { scene, .. }) => {
                let o = |k: usize| (scene.origin_mm[k] - main.scene.origin_mm[k]) as f32;
                let offset = glam::Vec3::new(o(0), o(1), o(2));
                let (matched_here, matched_there) = bimble_geom::compare::match_models(
                    &main.scene,
                    &scene,
                    bimble_geom::compare::Tolerance::default(),
                );
                let k = Linked {
                    path,
                    bvh: bimble_geom::bvh::Bvh::build(&bimble_geom::bvh::element_boxes(&scene)),
                    scene,
                    gpu: None,
                    offset,
                    mode: LinkMode::Solid,
                    matched_here,
                    matched_there,
                };
                // Reloaded from another file: in the old one's place. The same file linked
                // again replaces the old link.
                let name = k.name();
                let at = self
                    .relink
                    .take()
                    .filter(|&i| i < self.links.len())
                    .or_else(|| self.links.iter().position(|x| x.name() == name));
                match at {
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
        self.link_selected = None;
        self.link_selected_gpu = None;
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
        let l = self.loading.take().unwrap();
        let timing = l.timing();
        let Loading { path, imported, .. } = l;
        match &result {
            Ok(big::Outcome::Ask(_)) => log::warn!(
                "{}: more than this machine's memory budget, asking what to show ({timing})",
                path.display()
            ),
            Ok(big::Outcome::Ready { scene, partial, .. }) => log::info!(
                "{} {}: {} elements, {} triangles{}, {timing}",
                if imported { "imported" } else { "opened" },
                path.display(),
                scene.elements.len(),
                scene.triangle_count(),
                partial
                    .as_ref()
                    .map_or(String::new(), |p| format!(" (part: {p})")),
            ),
            Err(e) => log::error!("{}: {e}", path.display()),
        }
        match result {
            Ok(big::Outcome::Ask(a)) => {
                self.ask = Some((path, a));
            }
            Ok(big::Outcome::Ready {
                scene,
                model,
                time: load_time,
                partial,
                report,
            }) => {
                // Pages belong to the model being replaced: what they hold goes to it, not to
                // the new one when they are closed later.
                self.close_pages();
                if !report.is_empty() {
                    self.report = Some((path.clone(), report));
                }
                // Also in the browser's console, where they can be copied.
                log::info!(
                    "{}: {} elements read in {load_time:.2?}",
                    path.display(),
                    scene.elements.len()
                );
                for w in scene.warnings.iter().take(50) {
                    log::warn!("{w}");
                }
                let (classes, storeys, categories, materials) = counts(&scene);
                let bounds = bimble_render::Sphere::of(&scene);
                self.loaded = Some(Loaded {
                    path,
                    bvh: bimble_geom::bvh::Bvh::build(&bimble_geom::bvh::element_boxes(&scene)),
                    scene,
                    load_time,
                    partial,
                    bounds,
                    classes,
                    storeys,
                    categories,
                    materials,
                    imported: Vec::new(),
                    families_applied: None,
                    edit: model.map(if imported {
                        edit::Editor::imported
                    } else {
                        edit::Editor::new
                    }),
                    proxies: None,
                });
                // In the families the standards say, before the first upload.
                if let Some(l) = &mut self.loaded {
                    l.apply_families(&self.opts.style);
                }
                self.history_view = None;
                self.collab_message = None;
                self.home_open = false;
                self.start_borrowing();
                // Home lists it (the desktop: a file on disk).
                if let Some(l) = &self.loaded
                    && l.path.is_absolute()
                    && l.path.exists()
                {
                    self.settings.opened(&l.path.display().to_string());
                    self.settings.save();
                    self.preview_for = Some(l.path.display().to_string());
                }
                self.hidden_classes.clear();
                self.hidden_storeys.clear();
                self.hidden_categories.clear();
                self.hidden_materials.clear();
                self.clear_selection();
                self.temporary = None;
                self.reveal_hidden = false;
                self.fit_pending = true;
                self.set_hovered(None);
                // A linked model and a box from the previous model mean nothing for this one.
                self.links.clear();
                self.link_selected = None;
                self.link_selected_gpu = None;
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
        let before: Vec<String> = bimble_core::schedule::effective(&ed.model.schedules)
            .into_iter()
            .map(|d| d.name)
            .collect();
        if if undo { ed.undo() } else { ed.redo() } {
            self.rebuild_scene(None);
            let after: Vec<String> = self.schedule_defs().into_iter().map(|d| d.name).collect();
            if let Some(o) = &mut self.schedule
                && !after.contains(&o.name)
            {
                // Undoing a rename: follow it; undoing "New schedule": nothing left to show.
                match schedules::follow_rename(&before, &after) {
                    Some(n) => o.name = n,
                    None => self.schedule = None,
                }
            }
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
        l.proxies = None;
        l.bvh = bimble_geom::bvh::Bvh::build(&bimble_geom::bvh::element_boxes(&l.scene));
        (l.classes, l.storeys, l.categories, l.materials) = counts(&l.scene);
        // Families as read, then as the standards say (before this rebuild's upload).
        l.imported.clear();
        l.families_applied = None;
        l.apply_families(&self.opts.style);
        l.bounds = bounds_with_links(&l.scene, &self.links);
        self.selected = keep.and_then(|g| l.scene.elements.iter().position(|e| e.global_id == g));
        // Indices may have moved: the added ones are dropped (the clicked one is kept by id).
        self.also_selected.clear();
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
            &bimble_core::file::to_bytes(&ed.model),
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
        let author = self.settings.author();
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
                ui.label(format!("{} {name}  {}", if current { "*" } else { " " }, &h[..10]));
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

    /// Select an element of a linked model (or none); uploads its edges for the highlight.
    fn select_link_element(&mut self, s: Option<(usize, usize)>) {
        if s == self.link_selected {
            return;
        }
        self.link_selected = s;
        self.link_selected_gpu = s.and_then(|(n, i)| {
            let k = self.links.get(n)?;
            Some(self.renderer.upload_with(
                &self.device,
                &self.queue,
                &k.scene,
                |j, _| if j == i { Style::Normal } else { Style::Hidden },
                self.clipping(),
            ))
        });
        self.needs_overlay = true;
    }

    /// Close the pages that take the main view (schedule, project, materials): a view, a sheet
    /// or another page is shown instead.
    pub(crate) fn close_pages(&mut self) {
        // What was typed on a page and not yet stored is stored first, not lost.
        self.commit_pages();
        // Something else is shown: Home closes too.
        self.home_open = false;
        self.schedule = None;
        self.project_page = None;
        self.materials_page = None;
    }

    /// Store what pages hold and haven't stored yet (a field still being typed in).
    pub(crate) fn commit_pages(&mut self) {
        self.commit_project_page();
        self.commit_materials_page();
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
                &self.queue,
                &l.scene,
                |j, _| if j == i { Style::Normal } else { Style::Hidden },
                self.clipping(),
            )),
            _ => None,
        };
        self.needs_overlay = true;
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
            && !self.hidden_materials.contains(&e.material)
            && !self.hidden_classes.contains(&e.class)
            && !self.hidden_storeys.contains(&e.storey)
            && (self.reveal_hidden || !self.hidden_element(i))
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
                    &self.queue,
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
        let selection = self.selection();
        let style = |i: usize, _: &bimble_geom::SceneElement| {
            if !self.visible(i) {
                Style::Hidden
            } else if selection.contains(&i) {
                Style::Selected
            } else if self.reveal_hidden && self.hidden_element(i) {
                // Revealed: in the comparison's colour (a slot of its own would change the
                // graphic style's format).
                Style::OnlyHere
            } else if comparing && here.is_some_and(|m| !m.get(i).copied().unwrap_or(true)) {
                Style::OnlyHere
            } else {
                Style::Normal
            }
        };
        let Some(l) = &self.loaded else { return };
        // Edge normals only while the section box is on (they cost GPU memory).
        let mut gs =
            self.renderer
                .upload_with(&self.device, &self.queue, &l.scene, style, self.clipping());
        if let Some(p) = &l.proxies {
            gs.use_proxies(p.clone(), &l.scene, style);
        }
        self.gpu_scene = Some(gs);
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
            // Its scene-axis bounds: a turned section's box is in its own frame.
            let (cmin, cmax) = c.bounds();
            let (lo, hi) = (b.min.max(cmin), b.max.min(cmax));
            if lo.cmple(hi).all() {
                b = ClipBox {
                    min: lo,
                    max: hi,
                    yaw: 0.0,
                };
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
            let overlay = Overlay::new(&self.device, size_px, SAMPLES);
            let id = match self.overlay.take() {
                Some((_, id)) => {
                    r.update_egui_texture_from_wgpu_texture(
                        &self.device,
                        &overlay.color_view,
                        eframe::wgpu::FilterMode::Linear,
                        id,
                    );
                    id
                }
                None => r.register_native_texture(
                    &self.device,
                    &overlay.color_view,
                    eframe::wgpu::FilterMode::Linear,
                ),
            };
            self.overlay = Some((overlay, id));
            self.target = Some(target);
            self.needs_render = true;
        }
        // Proxies when this machine is short of GPU, once per model; big models only.
        if self.slow_gpu
            && self.settings.moving_proxies
            && let Some(l) = &mut self.loaded
            && l.proxies.is_none()
            && l.scene.triangle_count() > PROXY_MIN_TRIANGLES
        {
            let hlod = bimble_geom::hlod::build(&l.scene, &bimble_geom::hlod::Options::default());
            l.proxies = Some(std::sync::Arc::new(self.renderer.upload_proxies(
                &self.device,
                &self.queue,
                &l.scene,
                &hlod,
            )));
            self.needs_upload = true;
        }
        if self.needs_upload {
            self.upload();
        }
        if self.needs_render {
            self.render_model(size_px, ppp);
            self.needs_overlay = true;
        }
        if self.needs_overlay {
            self.render_highlights(ppp);
            self.needs_overlay = false;
        }
    }

    /// A GPU error was caught: draw without the optional GPU paths from now on, and say so.
    fn after_gpu_error(&mut self) {
        if self.gpu_safe {
            return;
        }
        let Some(e) = self.gpu_error.lock().ok().and_then(|mut e| e.take()) else {
            return;
        };
        self.gpu_safe = true;
        self.needs_render = true;
        // wgpu's message goes from general ("Validation Error") to specific: show the last line.
        let first = e
            .lines()
            .map(str::trim)
            .rfind(|l| !l.is_empty())
            .unwrap_or("");
        self.error = Some(format!(
            "The graphics card refused a drawing step, now off (edges while moving, GPU culling): \
             {first}"
        ));
    }

    /// The options a picture of the model is drawn with (the moving threshold aside).
    fn draw_options(&self, ppp: f32, min_radius: f32) -> DrawOptions {
        DrawOptions {
            hatch_scale: ppp,
            clip: self.clip(),
            min_radius,
            occlusion: self.slow_gpu,
            // GPU queries where they are free and work: native backends keeping the target
            // (ADR 0036 #1); slow machines use the CPU's (#5).
            gpu_occlusion: !self.slow_gpu
                && self.gpu_queries
                && !self.settings.gpu_cull
                && !self.gpu_safe,
            gpu_cull: self.settings.gpu_cull && !self.gpu_safe,
            // Far cells' stand-ins while moving on a machine that misses the target.
            proxy_px: if self.moving_lean && self.settings.moving_proxies {
                MOVING_PROXY_PX
            } else {
                0.0
            },
            // Edges from the depth instead of lines, in the single-sample pictures drawn while
            // moving on a slow machine (ADR 0036 #6).
            screen_edges: self.moving_lean && !self.gpu_safe,
            ..self.opts.clone()
        }
    }

    /// Draw the highlights over the kept picture of the model.
    fn render_highlights(&mut self, ppp: f32) {
        let (Some(target), Some((overlay, _)), Some(gs)) =
            (&self.target, &self.overlay, &self.gpu_scene)
        else {
            return;
        };
        let mut layers = Vec::new();
        if let Some(h) = &self.hover_gpu {
            layers.push(bimble_render::Layer::highlight(h, self.opts.style.hover));
        }
        if let (Some(g), Some((n, _))) = (&self.link_selected_gpu, self.link_selected)
            && let Some(k) = self.links.get(n)
        {
            layers.push(bimble_render::Layer {
                offset: k.offset,
                ..bimble_render::Layer::highlight(g, self.opts.style.selection)
            });
        }
        let bounds = self.loaded.as_ref().map_or(gs.bounds, |l| l.bounds);
        self.renderer.render_overlay(
            &self.device,
            &self.queue,
            target,
            overlay,
            &layers,
            &bounds,
            &self.camera,
            &self.draw_options(ppp, 0.0),
        );
    }

    /// Draw the model and its links into the target.
    fn render_model(&mut self, size_px: (u32, u32), ppp: f32) {
        let t = Instant::now();
        let min_radius = self.moving_detail(t, size_px.1);
        self.lean_shown = self.moving_lean;
        if self.lean_shown && self.lean.as_ref().is_none_or(|l| l.1.size != size_px) {
            let target = Target::new(&self.device, size_px, 1);
            let mut r = self.egui_renderer.write();
            self.lean = Some(match self.lean.take() {
                Some((renderer, _, id)) => {
                    r.update_egui_texture_from_wgpu_texture(
                        &self.device,
                        &target.color_view,
                        eframe::wgpu::FilterMode::Linear,
                        id,
                    );
                    (renderer, target, id)
                }
                None => {
                    let id = r.register_native_texture(
                        &self.device,
                        &target.color_view,
                        eframe::wgpu::FilterMode::Linear,
                    );
                    (Renderer::new(&self.device, 1), target, id)
                }
            });
        }
        let (renderer, target) = match (&self.lean, self.lean_shown) {
            (Some((r, t, _)), true) => (r, t),
            _ => match &self.target {
                Some(t) => (&self.renderer, t),
                None => return,
            },
        };
        let Some(gs) = &self.gpu_scene else {
            return;
        };
        let opts = self.draw_options(ppp, min_radius);
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
        let bounds = self.loaded.as_ref().map_or(gs.bounds, |l| l.bounds);
        renderer.render_layers(
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
        self.occlusion_waiting = true;
        self.drawn_at.push_back(t);
        while self
            .drawn_at
            .front()
            .is_some_and(|f| t.duration_since(*f) > Duration::from_secs(1))
        {
            self.drawn_at.pop_front();
        }
    }

    /// Once per page: "first-frame" when the previous frame drew the first model (uploaded and
    /// rendered, then presented before this frame began), or "failed" when opening it did not.
    #[cfg(target_arch = "wasm32")]
    fn tell_first_frame(&self, ctx: &egui::Context) {
        use std::sync::atomic::{AtomicBool, Ordering};
        static TOLD: AtomicBool = AtomicBool::new(false);
        if TOLD.load(Ordering::Relaxed) {
            return;
        }
        let failed = if self.ask.is_some() {
            Some("too large for the memory budget: asks which part to open")
        } else if self.loading.is_none() && self.parse_later.is_none() {
            self.error.as_deref()
        } else {
            None
        };
        if self.loaded.is_some()
            && self.gpu_scene.is_some()
            && !self.needs_upload
            && !self.needs_render
        {
            TOLD.store(true, Ordering::Relaxed);
            tell("first-frame", None);
        } else if let Some(e) = failed {
            TOLD.store(true, Ordering::Relaxed);
            tell("failed", Some(e));
        } else if self.loaded.is_some() {
            // Until the stamp, a frame follows the one that draws.
            ctx.request_repaint();
        }
    }

    /// Estimate of what the loaded model and its links hold (the browser's budget, ADR 0033).
    fn held(&self) -> u64 {
        self.loaded.as_ref().map_or(0, |l| big::held(&l.scene))
            + self.links.iter().map(|k| big::held(&k.scene)).sum::<u64>()
    }

    /// How small a thing may be and still be drawn this frame (mm, 0: everything). While the
    /// camera moves, the threshold follows the frame rate: up while frames are slower than the
    /// setting, down while faster; once it stops, everything (ADR 0035).
    fn moving_detail(&mut self, now: Instant, height_px: u32) -> f32 {
        let moving = self.drawn_camera.is_some_and(|c| c != self.camera);
        self.drawn_camera = Some(self.camera);
        let fps = self.settings.moving_fps;
        if !moving || fps == 0 {
            self.moving_frame_at = None;
            self.simplified = false;
            self.moving_lean = false;
            return 0.0;
        }
        if let Some(prev) = self.moving_frame_at {
            let dt = now.duration_since(prev).as_secs_f32();
            if dt > 0.002 {
                self.frame_floor = self.frame_floor.min(dt);
            }
            self.moving_px = next_moving_px(self.moving_px, dt, fps, self.frame_floor);
        }
        self.moving_frame_at = Some(now);
        // The first lever is MSAA: once the target is missed, it stays off until the view stops
        // (switching back and forth would make the controller swing).
        self.moving_lean |= self.moving_px > 0.0;
        self.slow_gpu |= self.moving_lean;
        self.simplified = self.moving_px > 0.0 || self.moving_lean;
        self.moving_px * self.camera.mm_per_pixel(height_px)
    }

    /// What a drag turns the view about: the geometry under the press, else the selected
    /// element's centre, else (None) the orbit target.
    fn orbit_pivot_at(&self, pos: egui::Pos2, rect: egui::Rect, aspect: f32) -> Option<glam::Vec3> {
        let l = self.loaded.as_ref()?;
        let ndc = [
            (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
            1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
        ];
        let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
        let clip = self.clip();
        bimble_render::pick_hit_bvh(&l.scene, &l.bvh, |i| self.visible(i), o, d, clip.as_ref())
            .map(|h| h.point)
            .or_else(|| {
                let s = self.selected?;
                ClipBox::around(&l.scene, |i| i == s).map(|b| (b.min + b.max) / 2.0)
            })
    }

    /// What the wheel zooms about: the geometry under the cursor in 3D, else (nothing there, or
    /// a 2D view, where depth changes nothing on screen) the plane through the orbit target.
    /// Picking a big model is slow, so the pivot is kept while only the wheel moves the camera:
    /// zooming about a point leaves it under the cursor.
    fn wheel_pivot(
        &mut self,
        pos: egui::Pos2,
        rect: egui::Rect,
        aspect: f32,
        flat: bool,
    ) -> Option<glam::Vec3> {
        if let Some((at, cam, p)) = self.zoom_pivot
            && at == pos
            && cam == self.camera
        {
            return Some(p);
        }
        let l = self.loaded.as_ref()?;
        let ndc = [
            (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
            1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
        ];
        let plane = self.camera.on_target_plane(ndc, aspect, &l.bounds);
        let p = if flat {
            plane
        } else {
            let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
            let clip = self.clip();
            bimble_render::pick_hit_bvh(&l.scene, &l.bvh, |i| self.visible(i), o, d, clip.as_ref())
                .map_or(plane, |h| h.point)
        };
        Some(p)
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
            if resp.drag_started() {
                let press = ui.input(|i| i.pointer.press_origin());
                self.orbit_pivot = press.and_then(|p| self.orbit_pivot_at(p, rect, aspect));
            }
            // Only redraw when the pointer actually moved: holding the button still must not
            // loop (a redraw request here would schedule the next frame, forever).
            let d = resp.drag_delta();
            if d != egui::Vec2::ZERO {
                match self.orbit_pivot {
                    Some(p) => self.camera.orbit_about(d.x * 0.008, d.y * 0.008, p),
                    None => self.camera.orbit(d.x * 0.008, d.y * 0.008),
                }
                self.needs_render = true;
            }
            // Where the view turns about.
            if let (Some(p), Some(l)) = (self.orbit_pivot, &self.loaded) {
                let q = self.camera.view_proj(aspect, &l.bounds).project_point3(p);
                if q.z > 0.0 && q.z < 1.0 {
                    let at = egui::pos2(
                        rect.min.x + (q.x + 1.0) / 2.0 * rect.width(),
                        rect.min.y + (1.0 - q.y) / 2.0 * rect.height(),
                    );
                    let c = ui.visuals().selection.bg_fill;
                    ui.painter()
                        .circle(at, 4.0, c, egui::Stroke::new(1.5, Color32::WHITE));
                }
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
        if resp.drag_stopped() {
            self.orbit_pivot = None;
        }
        if resp.drag_stopped() && self.moving_ref.is_some() {
            self.finish_reference_drag();
        }
        if resp.hovered() {
            let (scroll, zoom) = ui.input(|i| (i.smooth_scroll_delta().y, i.zoom_delta()));
            if scroll != 0.0 || zoom != 1.0 {
                let factor = (-scroll * 0.0015).exp() / zoom;
                let pos = resp.hover_pos();
                let pivot = pos.and_then(|pos| self.wheel_pivot(pos, rect, aspect, flat));
                match pivot {
                    Some(p) => self.camera.zoom_at(factor, p),
                    None => self.camera.zoom(factor),
                }
                self.zoom_pivot = pos.zip(pivot).map(|(pos, p)| (pos, self.camera, p));
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
            // Keys as in Revit and AutoCAD (see `measure`): Shift along an axis, Alt no snap,
            // Ctrl/Cmd + click goes on from there, right click or Enter ends.
            let (shift, alt, chain, enter) = ui.input(|i| {
                (
                    i.modifiers.shift,
                    i.modifiers.alt,
                    i.modifiers.command,
                    i.key_pressed(egui::Key::Enter),
                )
            });
            self.hover_snap = resp
                .hover_pos()
                .or(resp.interact_pointer_pos())
                .and_then(|p| self.snap_at(p, rect, aspect, alt))
                .map(|s| match self.measure_first {
                    Some(a) if shift => measure::orthogonal(a.point, s),
                    _ => s,
                });
            if resp.clicked()
                && let Some(s) = self.hover_snap
            {
                match self.measure_first.take() {
                    None => self.measure_first = Some(s),
                    Some(a) => {
                        self.dimensions.push(measure::Dimension { a, b: s });
                        if chain {
                            self.measure_first = Some(s);
                        }
                    }
                }
            }
            if resp.secondary_clicked() || enter {
                self.measure_first = None;
            }
            return;
        }
        // Hover highlight: pick again only when the pointer moved, never while dragging.
        let pointer = resp.hover_pos();
        if resp.dragged() || pointer.is_none() {
            if pointer.is_none() {
                self.hover_at = None;
                self.pointer_world = None;
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
            let h = bimble_render::pick_hit_bvh(
                &l.scene,
                &l.bvh,
                |i| self.visible(i),
                o,
                d,
                clip.as_ref(),
            )
            .map(|h| (h.element, h.distance));
            // Its coordinates for the status bar: on the element, else (a plan) on the cut.
            let on = match h {
                Some((_, t)) => Some(o + d * t),
                None if self.in_plan() && d.z.abs() > 1e-6 => clip.map(|c| {
                    let t = (c.max.z - o.z) / d.z;
                    o + d * t
                }),
                None => None,
            };
            let so = l.scene.origin_mm;
            self.pointer_world =
                on.map(|p| [p.x as f64 + so[0], p.y as f64 + so[1], p.z as f64 + so[2]]);
            self.set_hovered(h.map(|(i, _)| i));
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
            let clip = self.clip();
            let ours = bimble_render::pick_hit_bvh(
                &l.scene,
                &l.bvh,
                |i| self.visible(i),
                o,
                d,
                clip.as_ref(),
            );
            // The nearest hit among the shown links, if nearer than ours (#39).
            let theirs = self
                .links
                .iter()
                .enumerate()
                .filter(|(_, k)| {
                    k.mode != LinkMode::Hidden && !self.hidden_links.contains(&k.name())
                })
                .filter_map(|(n, k)| {
                    // In the link's coordinates: shifted by its offset, turned as ours.
                    let c = clip.map(|c| ClipBox {
                        min: c.min - c.to_frame(k.offset),
                        max: c.max - c.to_frame(k.offset),
                        yaw: c.yaw,
                    });
                    bimble_render::pick_hit_bvh(
                        &k.scene,
                        &k.bvh,
                        |_| true,
                        o - k.offset,
                        d,
                        c.as_ref(),
                    )
                    .map(|h| (n, h))
                })
                .min_by(|a, b| a.1.distance.total_cmp(&b.1.distance));
            let (hit, link_hit) = match (ours, theirs) {
                (Some(h), Some((_, t))) if h.distance <= t.distance => (Some(h.element), None),
                (_, Some((n, t))) => (None, Some((n, t.element))),
                (Some(h), None) => (Some(h.element), None),
                (None, None) => (None, None),
            };
            // Ctrl/Cmd adds to the selection or takes out of it (#54).
            if ui_add_to_selection(resp) {
                if let Some(h) = hit {
                    self.toggle_selected(h);
                }
            } else if hit != self.selected || !self.also_selected.is_empty() {
                self.selected = hit;
                self.also_selected.clear();
                self.needs_upload = true;
            }
            self.select_link_element(link_hit);
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
            // Delete: every selected element.
            if del && let Some(l) = &self.loaded {
                let ids: Vec<bimble_core::GlobalId> = self
                    .selection()
                    .iter()
                    .filter_map(|&k| l.scene.elements[k].global_id.parse().ok())
                    .collect();
                if !ids.is_empty() {
                    self.apply_commands(edit::delete(&ids));
                }
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
            self.camera.yaw = -std::f32::consts::FRAC_PI_2 + self.project_north();
            self.needs_render = true;
        }
        // Esc first stops placing, drops a half-made dimension, then the selection.
        let stopped_placing = esc
            && self
                .loaded
                .as_mut()
                .and_then(|l| l.edit.as_mut())
                .is_some_and(|ed| ed.placing.take().is_some());
        let mut handled = stopped_placing || (esc && self.measure_first.take().is_some());
        // A second Esc leaves the measure tool (Revit).
        if !handled && esc && self.measure_on {
            self.measure_on = false;
            self.hover_snap = None;
            handled = true;
        }
        if !handled && esc && self.selected.is_some() {
            self.clear_selection();
        }
        if !handled && esc {
            self.select_link_element(None);
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
    /// `free` (Alt): the surface point, no corner or edge snapping.
    fn snap_at(
        &self,
        pos: egui::Pos2,
        rect: egui::Rect,
        aspect: f32,
        free: bool,
    ) -> Option<measure::Snap> {
        let l = self.loaded.as_ref()?;
        let ndc = [
            (pos.x - rect.min.x) / rect.width() * 2.0 - 1.0,
            1.0 - (pos.y - rect.min.y) / rect.height() * 2.0,
        ];
        let (o, d) = self.camera.ray(ndc, aspect, &l.bounds);
        let clip = self.clip();
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let ours =
            bimble_render::pick_hit_bvh(&l.scene, &l.bvh, |i| self.visible(i), o, d, clip.as_ref())
                .map(|hit| {
                    let mesh = &*l.scene.elements[hit.element].world_mesh();
                    (
                        hit.distance,
                        if free {
                            measure::unsnapped(&hit)
                        } else {
                            measure::snap(mesh, &hit, (o, d), &vp, rect, pos)
                        },
                    )
                });
        // Linked models (#39): measured and snapped to like ours, in their place.
        let theirs = self.links.iter().filter_map(|k| {
            if k.mode == LinkMode::Hidden || self.hidden_links.contains(&k.name()) {
                return None;
            }
            let clip = clip.map(|c| ClipBox {
                min: c.min - c.to_frame(k.offset),
                max: c.max - c.to_frame(k.offset),
                yaw: c.yaw,
            });
            let hit = bimble_render::pick_hit_bvh(
                &k.scene,
                &k.bvh,
                |_| true,
                o - k.offset,
                d,
                clip.as_ref(),
            )?;
            let vpl = vp * glam::Mat4::from_translation(k.offset);
            let mesh = &*k.scene.elements[hit.element].world_mesh();
            let mut s = if free {
                measure::unsnapped(&hit)
            } else {
                measure::snap(mesh, &hit, (o - k.offset, d), &vpl, rect, pos)
            };
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
            clip.yaw.to_bits().hash(&mut h);
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
                self.template_baseline = Default::default();
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
                &project_of(&self.loaded, &self.side_views),
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
            // The view, and the project information its title block shows.
            format!(
                "{}|{}",
                view.as_ref()
                    .map(|v| serde_json::to_string(v).unwrap_or_default())
                    .unwrap_or_default(),
                serde_json::to_string(&project_of(&self.loaded, &self.side_views))
                    .unwrap_or_default()
            ),
        );
        if p.preview_image.as_ref().is_none_or(|i| i.key != key) {
            let opts = plan_options(
                p,
                &l.path,
                self.opts.style.clone(),
                view.clone(),
                &self.views,
                &project_of(&self.loaded, &self.side_views),
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
        // Which standards these are, and whether they are saved (ADR 0028).
        let source = self
            .style_path
            .as_ref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().into_owned());
        let changed = self.opts.style != self.style_baseline;
        ui.horizontal_wrapped(|ui| {
            ui.label(match &source {
                Some(n) => format!("From {n}"),
                None => "Bimble's default".to_string(),
            });
            if changed {
                ui.colored_label(ui.visuals().warn_fg_color, "· changed, not saved");
            } else {
                ui.weak("· saved");
            }
        });
        ui.horizontal(|ui| {
            if ui.button("Load…").clicked() {
                self.files.pick(files::Purpose::Style, ui.ctx());
            }
            if ui
                .button("Save…")
                .on_hover_text("Writes only what differs from the default style")
                .clicked()
            {
                self.save_style();
            }
            if ui
                .add_enabled(changed, egui::Button::new("Revert"))
                .on_hover_text(match &source {
                    Some(n) => format!("Back to {n} as loaded or last saved"),
                    None => "Back to Bimble's default".to_string(),
                })
                .clicked()
            {
                self.opts.style = self.style_baseline.clone();
                self.needs_render = true;
            }
            if ui
                .add_enabled(
                    self.opts.style != GraphicStyle::default(),
                    egui::Button::new("Reset to default…"),
                )
                .on_hover_text("Every standard back to Bimble's default (asks first)")
                .clicked()
            {
                self.confirm = Some(Confirm::ResetStandards);
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

    /// Save the standards to a file (asks where). True when saved.
    fn save_style(&mut self) -> bool {
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
                self.style_baseline = self.opts.style.clone();
                true
            }
            Ok(None) => false,
            Err(e) => {
                self.style_message = Some(e);
                false
            }
        }
    }

    /// What the viewer is doing, for the chips and the unsaved list (ADR 0028).
    fn snapshot(&self) -> state::Snapshot {
        let ed = self.loaded.as_ref().and_then(|l| l.edit.as_ref());
        let hidden_links = self
            .links
            .iter()
            .filter(|k| self.hidden_links.contains(&k.name()))
            .count();
        state::Snapshot {
            placing: ed.and_then(|e| e.placing.as_ref()).map(|p| p.kind.label()),
            measuring: self.measure_on,
            sheet_tool: self.layout.as_ref().and_then(|l| match l.tool {
                layout::Tool::Select => None,
                layout::Tool::Text => Some("Text"),
                layout::Tool::Dimension => Some("Dimension"),
            }),
            section_box: self.section_on && !self.in_2d(),
            edges: self.opts.edges,
            hatching: self.opts.hatching,
            orthographic: self.camera.orthographic && !self.in_2d(),
            hidden: [
                self.hidden_categories.len(),
                self.hidden_classes.len(),
                self.hidden_storeys.len(),
                hidden_links,
                self.hidden_ids.len(),
                self.isolated_ids.len(),
                self.hidden_materials.len(),
            ],
            temporary: self
                .temporary
                .as_ref()
                .map(|t| (t.isolate, t.elements.len())),
            revealing: self.reveal_hidden,
            sync_due: self.sync_due(),
            measurements: self.dimensions.len(),
            plan_options_changed: self.plan.template != self.template_baseline,
            model_dirty: ed.is_some_and(|e| e.dirty),
            views_dirty: self.side_views_dirty(),
            standards_changed: self.opts.style != self.style_baseline,
            partial: self.loaded.as_ref().and_then(|l| l.partial.clone()),
        }
    }

    /// Leave a state shown by a chip: back to the default.
    fn reset_chip(&mut self, c: state::Chip, ctx: &egui::Context) {
        use state::Chip;
        match c {
            Chip::Placing => {
                if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
                    ed.placing = None;
                }
            }
            Chip::Measuring => self.run_action(ribbon::Action::Measure, ctx),
            Chip::SheetTool => {
                if let Some(l) = &mut self.layout {
                    l.tool = layout::Tool::Select;
                }
            }
            Chip::SectionBox => self.toggle_section_box(),
            Chip::EdgesOff => self.run_action(ribbon::Action::Edges, ctx),
            Chip::HatchingOff => self.run_action(ribbon::Action::Hatching, ctx),
            Chip::Orthographic => self.run_action(ribbon::Action::Orthographic, ctx),
            Chip::Hidden => self.show_all_in_view(),
            Chip::Temporary => {
                self.temporary = None;
                self.needs_upload = true;
            }
            Chip::Revealing => self.run_action(ribbon::Action::RevealHidden, ctx),
            Chip::SyncDue => self.run_collab(ribbon::Action::Synchronize),
            Chip::Measurements => {
                self.dimensions.clear();
                self.measure_first = None;
            }
            Chip::PlanOptions => {
                self.plan.template = self.template_baseline.clone();
                self.plan.template_rev += 1;
            }
            // Open the file again, to choose another part: as a picked file, so unsaved work
            // is asked about first (ADR 0028).
            Chip::Partial => {
                if let Some(p) = self.loaded.as_ref().map(|l| l.path.clone()) {
                    self.files.push(files::Picked {
                        purpose: files::Purpose::Open,
                        name: p
                            .file_name()
                            .map_or(String::new(), |n| n.to_string_lossy().into_owned()),
                        path: Some(p),
                        bytes: None,
                    });
                }
            }
        }
        self.needs_render = true;
    }

    /// Run `job` (which sends its outcome on `tx`) off the UI: on a thread, or in a browser on
    /// a later frame, once "Loading…" is drawn.
    fn load_later(
        &mut self,
        path: PathBuf,
        job: impl FnOnce(std::sync::mpsc::Sender<LoadResult>, &Progress) + Send + 'static,
        ctx: &egui::Context,
    ) {
        let (tx, rx) = channel();
        let loading = Loading::new(path, rx, false);
        let progress = loading.progress.clone();
        self.loading = Some(loading);
        self.error = None;
        #[cfg(not(target_arch = "wasm32"))]
        {
            let ctx = ctx.clone();
            std::thread::spawn(move || {
                job(tx, &progress);
                ctx.request_repaint();
            });
        }
        #[cfg(target_arch = "wasm32")]
        {
            self.mesh_later = Some((2, Box::new(move || job(tx, &progress))));
            ctx.request_repaint();
        }
    }

    /// Browser: the job `load_later` left for a later frame.
    fn run_mesh_later(&mut self, ctx: &egui::Context) {
        let Some((frames, _)) = &mut self.mesh_later else {
            return;
        };
        if *frames > 0 {
            *frames -= 1;
            ctx.request_repaint();
            return;
        }
        let (_, job) = self.mesh_later.take().unwrap();
        job();
        ctx.request_repaint();
    }

    /// A model too large for this machine (ADR 0033): read it anyway, or choose a part of it
    /// (storeys, IFC classes, materials) whose estimate fits, or all of it anyway.
    fn ask_ui(&mut self, ctx: &egui::Context) {
        enum Do {
            ReadAnyway,
            Show { everything: bool },
            Cancel,
        }
        let Some((path, ask)) = &mut self.ask else {
            return;
        };
        let name = path
            .file_name()
            .map_or(String::new(), |n| n.to_string_lossy().into_owned());
        let mut act = None;
        egui::Modal::new(egui::Id::unique("bimble-too-large")).show(ctx, |ui| {
            ui.set_width(460.0);
            match ask.as_mut() {
                big::Ask::Read { cost, budget, .. } => {
                    ui.heading(format!("{name} is very large"));
                    ui.label(format!(
                        "Reading it needs about {}; Bimble keeps under {} on this computer \
                         ({}% of its memory). Reading it anyway may make the computer very slow \
                         or stop Bimble.",
                        big::size(cost.read()),
                        big::size(*budget),
                        (self.settings.memory_share * 100.0).round() as u32,
                    ));
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Read it anyway").clicked() {
                            act = Some(Do::ReadAnyway);
                        }
                        if ui.button("Cancel").clicked() {
                            act = Some(Do::Cancel);
                        }
                    });
                }
                big::Ask::Choose(c) => {
                    let all = c.cost.shown(c.all_triangles());
                    ui.heading(format!("{name} is too large to show whole"));
                    ui.label(format!(
                        "Showing all of it ({:.1} M triangles) needs about {}; Bimble keeps \
                         under {} on this computer. Choose a part to show:",
                        c.drawn_triangles() as f64 / 1e6,
                        big::size(all),
                        big::size(c.budget),
                    ));
                    ui.add_space(4.0);
                    ui.horizontal(|ui| {
                        for sp in big::Split::ALL {
                            if ui.selectable_label(c.split == sp, sp.label()).clicked()
                                && c.split != sp
                            {
                                c.split = sp;
                                c.chosen.clear();
                            }
                        }
                    });
                    egui::ScrollArea::vertical()
                        .max_height(280.0)
                        .show(ui, |ui| {
                            for g in big::groups(&c.census, c.split) {
                                let mut on = c.chosen.contains(&g.name);
                                let alone = c.cost.shown(g.triangles);
                                let text = format!(
                                    "{}  ·  {} elements, about {}",
                                    g.name,
                                    g.elements,
                                    big::size(alone)
                                );
                                let r = ui.add_enabled(
                                    on || alone <= c.budget,
                                    egui::Checkbox::new(&mut on, text),
                                );
                                if r.changed() {
                                    if on {
                                        c.chosen.push(g.name.clone());
                                    } else {
                                        c.chosen.retain(|x| *x != g.name);
                                    }
                                }
                            }
                        });
                    let need = c.cost.shown(c.chosen_triangles());
                    let fits = !c.chosen.is_empty() && need <= c.budget;
                    ui.add_space(4.0);
                    ui.label(if c.chosen.is_empty() {
                        "Nothing chosen.".to_owned()
                    } else if fits {
                        format!("Chosen: about {} — fits.", big::size(need))
                    } else {
                        format!("Chosen: about {} — too much.", big::size(need))
                    });
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui
                            .add_enabled(fits, egui::Button::new("Show the chosen part"))
                            .clicked()
                        {
                            act = Some(Do::Show { everything: false });
                        }
                        if ui.button("Show all of it anyway").clicked() {
                            act = Some(Do::Show { everything: true });
                        }
                        if ui.button("Cancel").clicked() {
                            act = Some(Do::Cancel);
                        }
                    });
                }
            }
        });
        let Some(act) = act else { return };
        let (path, ask) = self.ask.take().unwrap();
        match (act, *ask) {
            (Do::ReadAnyway, big::Ask::Read { source, budget, .. }) => {
                self.load_later(
                    path,
                    move |tx, progress| {
                        let _ = tx.send(big::load_ifc(source, budget, true, progress));
                    },
                    ctx,
                );
            }
            (Do::Show { everything }, big::Ask::Choose(c)) => {
                self.load_later(
                    path,
                    move |tx, progress| {
                        let _ = tx.send(Ok(c.scene(everything, progress)));
                    },
                    ctx,
                );
            }
            (_, big::Ask::Choose(c)) => drop(c),
            _ => {}
        }
    }

    /// Settings → Appearance: theme, font and text size of the interface (issue #53).
    /// Settings → You (#52): who you are, until there is a server to sign in to.
    fn you_ui(&mut self, ui: &mut egui::Ui) {
        ui.strong("You");
        egui::Grid::new("you").num_columns(2).show(ui, |ui| {
            ui.label("Name");
            ui.text_edit_singleline(&mut self.settings.user_name);
            ui.end_row();
            ui.label("E-mail");
            ui.text_edit_singleline(&mut self.settings.user_email);
            ui.end_row();
        });
        ui.weak(format!(
            "Written as the author of commits and on the elements you borrow: {}",
            self.settings.author()
        ));
        ui.add_space(8.0);
    }

    fn appearance_ui(&mut self, ui: &mut egui::Ui) {
        ui.strong("Appearance");
        egui::Grid::new("appearance").num_columns(2).show(ui, |ui| {
            ui.label("Theme");
            egui::ComboBox::from_id_salt("theme")
                .selected_text(ui_theme::by_id(&self.settings.theme).name)
                .show_ui(ui, |ui| {
                    for t in &ui_theme::THEMES {
                        ui.selectable_value(&mut self.settings.theme, t.id.to_owned(), t.name);
                    }
                });
            ui.end_row();
            ui.label("Font");
            ui.horizontal(|ui| {
                for f in ui_theme::UiFont::ALL {
                    ui.radio_value(&mut self.settings.ui_font, f.id().to_owned(), f.name());
                }
            });
            ui.end_row();
            ui.label("Text size");
            ui.add(
                egui::Slider::new(&mut self.settings.text_size, ui_theme::TEXT_SIZES)
                    .step_by(0.5)
                    .suffix(" pt"),
            );
            ui.end_row();
        });
        let d = settings::Settings::default();
        let is_default = self.settings.theme == d.theme
            && self.settings.ui_font == d.ui_font
            && self.settings.text_size == d.text_size;
        if !is_default && ui.button("Default look").clicked() {
            self.settings.theme = d.theme;
            self.settings.ui_font = d.ui_font;
            self.settings.text_size = d.text_size;
        }
    }

    /// About Bimble: the version, what draws it and where to read or report, with a copy for a
    /// bug report (issue #59 needed the graphics card and backend).
    fn about_ui(&mut self, ctx: &egui::Context) {
        if !self.about_window {
            return;
        }
        let system = if cfg!(target_arch = "wasm32") {
            "browser (WebAssembly)".to_owned()
        } else {
            format!("{} {}", std::env::consts::OS, std::env::consts::ARCH)
        };
        let info = format!(
            "Bimble {}\nGraphics: {}\nSystem: {system}",
            env!("CARGO_PKG_VERSION"),
            self.adapter
        );
        let mut open = true;
        egui::Window::new("About Bimble")
            .open(&mut open)
            .collapsible(false)
            .resizable(false)
            .show(ctx, |ui| {
                ui.heading(format!("Bimble {}", env!("CARGO_PKG_VERSION")));
                ui.label("Open-source structural BIM. MIT licence.");
                ui.add_space(6.0);
                ui.label(format!("Graphics: {}", self.adapter));
                ui.label(format!("System: {system}"));
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    ui.hyperlink_to("Manual", "https://guilbep.github.io/bimble-docs/");
                    ui.hyperlink_to(
                        "Report a problem",
                        "https://github.com/guilbep/bimble/issues",
                    );
                    ui.hyperlink_to("Source", "https://github.com/guilbep/bimble");
                });
                ui.add_space(6.0);
                if ui
                    .button("Copy for a bug report")
                    .on_hover_text("Version, graphics card and system, to paste in an issue")
                    .clicked()
                {
                    ui.ctx().copy_text(info.clone());
                }
            });
        self.about_window = open;
    }

    /// Settings, kept between sessions.
    fn settings_ui(&mut self, ctx: &egui::Context) {
        if !self.settings_window {
            return;
        }
        let mut open = true;
        let before = self.settings.clone();
        let mut reopen: Option<PathBuf> = None;
        egui::Window::new("Settings")
            .open(&mut open)
            .collapsible(false)
            .default_width(380.0)
            .show(ctx, |ui| {
                self.you_ui(ui);
                ui.separator();
                ui.strong("Coordinates");
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.settings.wgs84, false, "Survey system (LV95)");
                    ui.radio_value(&mut self.settings.wgs84, true, "WGS84 (latitude, longitude)");
                });
                ui.weak(
                    "Shown in the status bar for the point under the pointer. WGS84 is worked \
                     out from LV95 (swisstopo's formulas, about 1 m).",
                );
                self.appearance_ui(ui);
                ui.add_space(8.0);
                ui.strong("Memory for one model");
                if cfg!(target_arch = "wasm32") {
                    ui.label(
                        "In a browser a model may use all a tab can address: 4 GB. Larger \
                         models ask which part to show.",
                    );
                } else {
                    let mut pct = (self.settings.memory_share * 100.0).round();
                    ui.add(egui::Slider::new(&mut pct, 20.0..=100.0).suffix(" %"));
                    self.settings.memory_share = pct / 100.0;
                    if let Some(total) = big::total_memory() {
                        let limit = (total as f64 * self.settings.memory_share) as u64;
                        ui.label(format!(
                            "{} of this computer's {}. A model that needs more asks which part \
                             to show (storeys, IFC classes, materials) before anything is cut. \
                             Used when a model is opened.",
                            big::size(limit),
                            big::size(total)
                        ));
                        // What it means for the model open now, and a way to apply it.
                        if let Some(l) = &self.loaded {
                            let held = self.held();
                            let mut line = format!(
                                "The open model holds about {}{}.",
                                big::size(held),
                                match &l.partial {
                                    Some(p) => format!(", opened in part ({p})"),
                                    None => " (whole)".to_owned(),
                                }
                            );
                            if held > limit {
                                line += " That is over this limit: reopened, it would ask which part to show.";
                            }
                            ui.label(egui::RichText::new(line).strong());
                            let changed = (self.settings.memory_share - self.opened_share).abs() > 1e-6;
                            if changed && ui.button("Reopen with this limit").clicked() {
                                reopen = Some(l.path.clone());
                            }
                        }
                    }
                }
                ui.add_space(8.0);
                ui.strong("While the view moves");
                ui.horizontal(|ui| {
                    for (fps, label) in [
                        (0, "Draw everything"),
                        (15, "15 fps"),
                        (30, "30 fps"),
                        (60, "60 fps"),
                        (120, "120 fps"),
                        (240, "240 fps"),
                    ] {
                        ui.selectable_value(&mut self.settings.moving_fps, fps, label);
                    }
                });
                ui.label(
                    "Below this frame rate, what is smallest on screen is left out while the view \
                     turns, pans or zooms, and drawn again as soon as it stops.",
                );
                ui.checkbox(
                    &mut self.settings.moving_proxies,
                    "Simplified far parts while moving, on a machine below that rate",
                );
                ui.checkbox(
                    &mut self.settings.gpu_cull,
                    "Test shared shapes' copies on the GPU (experimental)",
                );
            });
        if self.settings != before {
            self.settings.save();
            let look = |s: &settings::Settings| (s.theme.clone(), s.ui_font.clone(), s.text_size);
            if look(&self.settings) != look(&before) {
                self.settings.apply_look(ctx);
            }
        }
        self.settings_window = open;
        #[cfg(not(target_arch = "wasm32"))]
        if let Some(p) = reopen {
            self.open(p, ctx);
        }
        #[cfg(target_arch = "wasm32")]
        let _ = reopen;
    }

    /// An import's report (ADR 0032: what didn't come in is said, not dropped).
    fn report_ui(&mut self, ctx: &egui::Context) {
        let Some((path, lines)) = &self.report else {
            return;
        };
        let mut open = true;
        let name = path
            .file_name()
            .map_or(String::new(), |n| n.to_string_lossy().into_owned());
        egui::Window::new(format!("Imported: {name}"))
            .open(&mut open)
            .collapsible(false)
            .default_width(420.0)
            .show(ctx, |ui| {
                egui::ScrollArea::vertical()
                    .max_height(320.0)
                    .show(ui, |ui| {
                        for l in lines {
                            ui.label(l);
                        }
                    });
            });
        if !open {
            self.report = None;
        }
    }

    /// The questions that need an answer before going on.
    fn confirm_ui(&mut self, ctx: &egui::Context) {
        let Some(c) = self.confirm else { return };
        let unsaved = state::unsaved(&self.snapshot());
        let mut answer: Option<bool> = None;
        let mut save_all = false;
        egui::Modal::new(egui::Id::unique("bimble-confirm")).show(ctx, |ui| {
            ui.set_width(380.0);
            match c {
                Confirm::Close => {
                    ui.heading("Close Bimble?");
                    ui.label(format!(
                        "Not saved: {}. Closing now loses these changes.",
                        unsaved.join(", ")
                    ));
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Save and close").clicked() {
                            save_all = true;
                        }
                        if ui.button("Close without saving").clicked() {
                            answer = Some(true);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(false);
                        }
                    });
                }
                Confirm::Open => {
                    let name = self.pending_open.as_ref().map_or("", |p| p.name.as_str());
                    ui.heading(format!("Open {name}?"));
                    ui.label(format!(
                        "Not saved: {}. Opening another file now loses these changes.",
                        unsaved.join(", ")
                    ));
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Save, then open").clicked() {
                            save_all = true;
                        }
                        if ui.button("Open without saving").clicked() {
                            answer = Some(true);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(false);
                        }
                    });
                }
                Confirm::ResetStandards => {
                    ui.heading("Reset the standards?");
                    ui.label(
                        "Every pen, line, text, dimension and hatch type, and every colour, goes \
                         back to Bimble's default. Your style file is not changed; Revert or Load… \
                         brings it back.",
                    );
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Reset to default").clicked() {
                            answer = Some(true);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(false);
                        }
                    });
                }
            }
        });
        if save_all {
            // Each unsaved kind saved in turn; close only when all of them are.
            let s = self.snapshot();
            if s.model_dirty || s.views_dirty {
                if self.side_views.is_some() {
                    self.save_side_views();
                } else {
                    self.save();
                }
            }
            if s.standards_changed {
                self.save_style();
            }
            let s = self.snapshot();
            let saved = match c {
                // Opening replaces the model and views, not the standards.
                Confirm::Open => !s.model_dirty && !s.views_dirty,
                _ => state::unsaved(&s).is_empty(),
            };
            if saved {
                answer = Some(true);
            } else {
                // The save's own message says why; the question stays open to choose again.
                let why = self
                    .error
                    .clone()
                    .or_else(|| {
                        self.loaded
                            .as_ref()
                            .and_then(|l| l.edit.as_ref()?.message.clone())
                    })
                    .or_else(|| self.view_message.clone())
                    .unwrap_or_default();
                self.error = Some(
                    format!("Not everything was saved. {why}")
                        .trim()
                        .to_string(),
                );
            }
        }
        match (c, answer) {
            (Confirm::Close, Some(true)) => {
                self.confirm = None;
                self.allow_close = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            (Confirm::Open, Some(true)) => {
                self.confirm = None;
                if let Some(p) = self.pending_open.take() {
                    self.handle_one(p, ctx);
                }
            }
            (Confirm::ResetStandards, Some(true)) => {
                self.confirm = None;
                self.opts.style = GraphicStyle::default();
                self.needs_render = true;
            }
            (_, Some(false)) => {
                self.confirm = None;
                self.pending_open = None;
            }
            _ => {}
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

    /// The properties of a selected element of a linked model: read only (#39).
    fn link_properties(&mut self, ui: &mut egui::Ui) {
        let Some((n, i)) = self.link_selected else {
            return;
        };
        let Some(k) = self.links.get(n) else { return };
        let Some(e) = k.scene.elements.get(i) else {
            return;
        };
        ui.heading(&e.class);
        ui.weak(format!("In the linked model {} (read-only)", k.name()));
        egui::Grid::new("link-props")
            .num_columns(2)
            .striped(true)
            .show(ui, |ui| {
                let mut row = |a: &str, v: &str| {
                    ui.label(a);
                    ui.add(egui::Label::new(v).selectable(true));
                    ui.end_row();
                };
                row("Name", &e.name);
                row("GlobalId", &e.global_id);
                row("Storey", &e.storey);
                row("Material", &e.material);
                row("Family", e.category.label());
                row(
                    "Volume",
                    &format!("{:.3} m³", e.world_mesh().volume() / 1e9),
                );
                let matched = k.matched_there.get(i).copied().unwrap_or(true);
                row(
                    "In the host",
                    if matched {
                        "something at the same place"
                    } else {
                        "nothing at this place"
                    },
                );
            });
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
            let group: Vec<bimble_core::GlobalId> = self
                .selected
                .into_iter()
                .chain(self.also_selected.iter().copied())
                .filter_map(|k| l.scene.elements[k].global_id.parse().ok())
                .collect();
            ed.element_panel(ui, id, &group);
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
        if bimble_geom::viewfile::is_named(&l.path.to_string_lossy()) {
            ui.weak(share::NOT_KEPT);
        }
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
                row("Triangles", &e.triangle_count().to_string());
                row(
                    "Volume",
                    &format!("{:.3} m³", e.world_mesh().volume() / 1e9),
                );
            });
    }

    fn status_bar(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        let mut reset = None;
        ui.horizontal(|ui| {
            if let Some(shown) = self.load_shown() {
                ui.label(egui::RichText::new(shown.text).color(ui_theme::accent()));
                ui.separator();
            }
            ui.label(self.status_hint());
            // Where the pointer is (or the measured point), in LV95 or WGS84 (ADR 0045).
            let at = self
                .hover_snap
                .zip(self.loaded.as_ref())
                .map(|(s, l)| {
                    let o = l.scene.origin_mm;
                    [s.point.x as f64 + o[0], s.point.y as f64 + o[1], s.point.z as f64 + o[2]]
                })
                .or(self.pointer_world);
            if let (Some(p), Some(l)) = (at, &self.loaded) {
                ui.separator();
                ui.label(coords::text(&l.scene.georef, p, self.settings.wgs84))
                    .on_hover_text("Settings → Coordinates: survey system (LV95) or WGS84");
            }
            ui.separator();
            // Every state that isn't the default, with its way back (ADR 0028).
            for (chip, label) in state::chips(&self.snapshot()) {
                let r = ui.add(
                    egui::Button::new(egui::RichText::new(format!("{label}  ×")).small())
                        .fill(ui_theme::selected_bg())
                        .corner_radius(10.0),
                );
                if r.on_hover_text(chip.reset_hint()).clicked() {
                    reset = Some(chip);
                }
            }
            if let Some(l) = &self.loaded {
                let visible = (0..l.scene.elements.len()).filter(|&i| self.visible(i)).count();
                // Pictures drawn in the last second; nothing drawn for half a second: idle.
                let now = Instant::now();
                let rate = match self.drawn_at.back() {
                    Some(b) if now.duration_since(*b) < Duration::from_millis(500) => format!(
                        "{} fps",
                        self.drawn_at
                            .iter()
                            .filter(|t| now.duration_since(**t) <= Duration::from_secs(1))
                            .count()
                    ),
                    _ => "idle".to_owned(),
                };
                if rate != "idle" {
                    // One more look once drawing stops, to say so.
                    ui.ctx().request_repaint_after(Duration::from_millis(600));
                }
                let app = big::in_use();
                ui.weak(format!(
                    "{visible}/{} elements · {} triangles · loaded in {:.0?} · model ≈ {}{} · {rate}",
                    l.scene.elements.len(),
                    self.gpu_scene.as_ref().map_or(0, |g| g.triangles()),
                    l.load_time,
                    big::size(self.held()),
                    if app > 0 {
                        format!(" · app {}", big::size(app))
                    } else {
                        String::new()
                    },
                ))
                .on_hover_text(
                    "Model: what its meshes hold, estimated (ADR 0033). App: what Bimble holds in \
                     memory now. fps: pictures drawn in the last second; idle when nothing moves \
                     (Bimble only draws on change).",
                );
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.weak(&self.adapter);
            });
        });
        if let Some(c) = reset {
            self.reset_chip(c, &ctx);
        }
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
        #[cfg(target_arch = "wasm32")]
        self.tell_first_frame(&ctx);
        self.poll_loading();
        self.after_gpu_error();
        if self.loaded.is_some()
            && let Some(p) = self.pending_link.take()
        {
            self.link(p, &ctx);
        }
        self.poll_linking();
        for l in [&mut self.loading, &mut self.linking].into_iter().flatten() {
            l.observe();
        }
        // The loading thread only wakes the UI when it is done: redraw for the bar meanwhile.
        if self.loading.is_some() || self.linking.is_some() {
            ctx.request_repaint_after(Duration::from_millis(100));
        }

        // Files dropped on the window.
        if let Some(f) = ctx.input(|i| i.raw.dropped_files.first().cloned()) {
            self.files.dropped(f, &ctx);
        }
        self.handle_picked(&ctx);
        self.run_parse_later(&ctx);
        self.run_mesh_later(&ctx);

        let bar = |fill: egui::Color32| {
            egui::Frame::new()
                .fill(fill)
                .inner_margin(egui::Margin::symmetric(ui_theme::GAP_M as i8, 4))
        };
        // One row: the File menu, the ribbon's tabs, quick access (#51).
        egui::Panel::top("title")
            .frame(bar(ui_theme::tabs_bg()).inner_margin(egui::Margin {
                left: ui_theme::GAP_M as i8,
                right: ui_theme::GAP_M as i8,
                top: 2,
                bottom: 0,
            }))
            .show(ui, |ui| self.title_row(ui));
        egui::Panel::top("ribbon")
            .frame(
                bar(ui_theme::ribbon_bg())
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
                            let placing = self
                                .loaded
                                .as_ref()
                                .and_then(|l| l.edit.as_ref())
                                .is_some_and(|e| e.placing.is_some());
                            if placing {
                                // While placing, what is placed: storey and sizes (ADR 0027).
                                if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut())
                                {
                                    ui.heading("Place");
                                    ed.place_panel(ui);
                                    if let Some(m) = &ed.message {
                                        ui.label(m);
                                    }
                                }
                            } else if self.link_selected.is_some() {
                                if ui.small_button("Back to the view").clicked() {
                                    self.select_link_element(None);
                                } else {
                                    self.link_properties(ui);
                                }
                            } else if self.selected.is_some() {
                                if ui.small_button("Back to the view").clicked() {
                                    self.clear_selection();
                                } else {
                                    self.properties(ui);
                                }
                            } else if self.schedule.is_some() {
                                self.schedule_properties(ui);
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
        // On Home, no tabs: leaving it brings back the view's.
        if self.loaded.is_some() && !self.home_open {
            egui::Panel::top("view-tabs")
                .frame(
                    egui::Frame::new()
                        .fill(ui_theme::title_bg())
                        .inner_margin(egui::Margin {
                            left: 6,
                            right: 6,
                            top: 4,
                            bottom: 0,
                        }),
                )
                .show(ui, |ui| self.view_tabs(ui));
        }
        let view = egui::CentralPanel::no_frame().show(ui, |ui| {
            let rect = ui.available_rect_before_wrap();
            let aspect = rect.width() / rect.height().max(1.0);
            self.last_aspect = aspect;
            self.handle_keys(&ctx, aspect);

            // While a model loads, the one shown stays (and can be moved around): the progress
            // is a card over the view and a line in the status bar.
            if self.loaded.is_none() && self.loading.is_some() {
                return;
            }
            // Home (#52): when nothing is open, or asked for.
            if self.loaded.is_none() || self.home_open {
                self.home_ui(ui);
                return;
            }
            // The material browser: it takes the main view.
            if self.materials_page.is_some() {
                self.materials_ui(ui);
                return;
            }
            // The project information page: it takes the main view.
            if self.project_page.is_some() {
                self.project_ui(ui);
                return;
            }
            // A schedule open: it takes the main view.
            if self.schedule.is_some() {
                self.schedule_ui(ui);
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
            // A picture drawn while moving left things out: draw it whole once the view has
            // been still a moment.
            if self.simplified {
                if self
                    .moving_frame_at
                    .is_none_or(|t| t.elapsed() > Duration::from_millis(150))
                {
                    self.needs_render = true;
                } else {
                    ctx.request_repaint_after(Duration::from_millis(160));
                }
            }
            let ppp = ctx.pixels_per_point();
            let size_px = (
                (rect.width() * ppp).round().max(1.0) as u32,
                (rect.height() * ppp).round().max(1.0) as u32,
            );
            self.render_view(size_px, ppp);
            let shown = match (&self.lean, self.lean_shown) {
                (Some((_, _, id)), true) => Some(*id),
                _ => self.texture,
            };
            if let Some(tex) = shown {
                let img = egui::Image::from_texture((tex, rect.size()))
                    .fit_to_exact_size(rect.size())
                    .sense(Sense::click_and_drag());
                let resp = ui.add(img);
                // Highlights go with the picture whose depth they were tested against.
                if let (Some((_, id)), false) = (&self.overlay, self.lean_shown) {
                    ui.painter().image(
                        *id,
                        rect,
                        egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
                        Color32::WHITE,
                    );
                }
                self.handle_view_input(ui, &resp);
                // Right click (not while measuring or placing): hide, isolate, reveal (#54). An
                // element right-clicked outside the selection becomes the selection (Revit).
                let placing = self
                    .loaded
                    .as_ref()
                    .and_then(|l| l.edit.as_ref())
                    .is_some_and(|e| e.placing.is_some());
                if !self.measure_on && !placing {
                    if resp.secondary_clicked()
                        && let Some(h) = self.hovered
                        && !self.selection().contains(&h)
                    {
                        self.selected = Some(h);
                        self.also_selected.clear();
                        self.needs_upload = true;
                    }
                    let mut run = None;
                    resp.context_menu(|ui| self.visibility_menu(ui, &mut run));
                    if let Some(a) = run {
                        self.run_action(a, &ctx);
                    }
                }
                if let (true, false, Some(mut b), Some(l)) = (
                    self.section_on,
                    self.in_2d(),
                    self.section_box,
                    &self.loaded,
                ) {
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
                if self.needs_render || self.needs_upload || self.needs_overlay {
                    ctx.request_repaint();
                }
                // GPU occlusion queries answer a frame late (ADR 0036 #1): look again until they
                // settle; draw again if they change what shows.
                if self.occlusion_waiting
                    && let Some(gs) = &self.gpu_scene
                {
                    match gs.gpu_occlusion_poll(&self.device) {
                        bimble_render::Settle::Waiting => {
                            ctx.request_repaint_after(Duration::from_millis(4));
                        }
                        bimble_render::Settle::Changed => {
                            self.needs_render = true;
                            ctx.request_repaint();
                        }
                        bimble_render::Settle::Settled => self.occlusion_waiting = false,
                    }
                }
            }
            if let Some(err) = &self.error {
                ui.colored_label(Color32::RED, err);
            }
        });
        if let Some(shown) = self.load_shown() {
            progress_card(&ctx, view.response.rect, &shown);
        }
        self.palette_ui(&ctx);
        #[cfg(target_arch = "wasm32")]
        state::UNSAVED.store(
            !state::unsaved(&self.snapshot()).is_empty(),
            std::sync::atomic::Ordering::Relaxed,
        );
        // Closing with unsaved work asks first (ADR 0028).
        if ctx.input(|i| i.viewport().close_requested()) && !self.allow_close {
            // A value typed but not yet stored counts too.
            self.commit_pages();
            if !state::unsaved(&self.snapshot()).is_empty() {
                ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
                // Another question open (reset the standards…) gives way to this one.
                self.confirm = Some(Confirm::Close);
                self.pending_open = None;
            }
        }
        self.confirm_ui(&ctx);
        self.ask_ui(&ctx);
        self.settings_ui(&ctx);
        self.about_ui(&ctx);
        self.links_ui(&ctx);
        self.belongs_ui(&ctx);
        if let Some(l) = &mut self.loaded
            && l.apply_families(&self.opts.style)
        {
            l.proxies = None;
            // Drawings and schedules made with the families as they were are made again.
            self.scene_rev += 1;
            self.needs_upload = true;
            self.needs_render = true;
        }
        self.keep_preview();
        self.report_ui(&ctx);
    }
}

/// Today's date (UTC) as YYYY-MM-DD, for title blocks.
fn today() -> String {
    bimble_drawing::today()
}

/// A click that adds to the selection, or takes out of it: with Ctrl (Cmd on macOS).
fn ui_add_to_selection(resp: &egui::Response) -> bool {
    resp.ctx.input(|i| i.modifiers.command)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Slow moving frames raise the size threshold, fast ones lower it back to everything, and
    /// a pause changes nothing.
    #[test]
    fn the_moving_threshold_follows_the_frame_rate() {
        let inf = f32::INFINITY;
        let mut px = 0.0;
        for _ in 0..4 {
            px = next_moving_px(px, 0.060, 30, inf);
        }
        assert!(px > 2.0, "{px}");
        assert_eq!(next_moving_px(px, 1.0, 30, inf), px);
        assert_eq!(next_moving_px(px, 0.033, 30, inf), px);
        for _ in 0..20 {
            px = next_moving_px(px, 0.010, 30, inf);
        }
        assert_eq!(px, 0.0);
        assert!(next_moving_px(40.0, 0.2, 30, inf) <= 48.0);
    }

    /// On a 60 Hz screen every frame takes 16.7 ms however fast the GPU: a 120 or 240 fps
    /// target must not leave things out for it, and a threshold raised by one slow frame must
    /// come back down. On a 120 Hz screen, 120 fps is held.
    #[test]
    fn the_screen_refresh_caps_the_target() {
        let hz60 = 1.0 / 60.0;
        for fps in [60, 120, 240] {
            let mut px = 0.0;
            for _ in 0..50 {
                px = next_moving_px(px, hz60, fps, hz60);
            }
            assert_eq!(px, 0.0, "{fps} fps");
            px = next_moving_px(px, 0.040, fps, hz60);
            assert!(px > 0.0);
            for _ in 0..20 {
                px = next_moving_px(px, hz60, fps, hz60);
            }
            assert_eq!(px, 0.0, "{fps} fps comes back");
        }
        let hz120 = 1.0 / 120.0;
        let px = next_moving_px(0.0, hz60, 120, hz120);
        assert!(px > 0.0, "120 Hz screen, 60 fps frames: thin");
    }

    /// With several links, the depth range covers every one where it is placed (#39).
    #[test]
    fn bounds_cover_every_link() {
        use bimble_core::demo::{FrameSpec, concrete_frame};
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let link = |name: &str, x: f32| Linked {
            path: PathBuf::from(name),
            scene: scene.clone(),
            bvh: Default::default(),
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
        let (a, m) = load_bytes("curves.IFC", bytes).unwrap();
        let (b, _) = load_any(Path::new(ifc)).unwrap();
        assert!(m.is_none());
        assert_eq!(a.elements.len(), b.elements.len());
        let model = bimble_core::demo::concrete_frame(&Default::default());
        let text = bimble_core::file::to_string(&model);
        let (s, m) = load_bytes("frame.bimble", text.into_bytes()).unwrap();
        assert_eq!(m.unwrap(), model);
        assert_eq!(s.elements.len(), model.elements.len());
        // Not UTF-8: an error, not a panic.
        assert!(load_bytes("x.bimble", vec![0xff, 0xfe, 0x00]).is_err());
    }

    /// The editable copy of an imported IFC never takes the name of an existing model, which
    /// Save would then overwrite.
    #[test]
    fn import_copy_never_overwrites_a_model() {
        let dir = std::env::temp_dir().join(format!("bimble-import-copy-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let ifc = dir.join("house.ifc");
        assert_eq!(editable_copy_path(&ifc), dir.join("house.bimble"));
        std::fs::write(dir.join("house.bimble"), "x").unwrap();
        assert_eq!(
            editable_copy_path(&ifc),
            dir.join("house (imported).bimble")
        );
        std::fs::write(dir.join("house (imported).bimble"), "x").unwrap();
        assert_eq!(
            editable_copy_path(&ifc),
            dir.join("house (imported 2).bimble")
        );
        std::fs::remove_dir_all(&dir).ok();
    }
}
