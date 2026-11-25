//! The page calls the running viewer (ADR 0048): JSON in, JSON out, edits through the viewer's
//! own undo history. All logic stays in Rust; a Vue page only calls these exports.

use eframe::egui;
use std::cell::RefCell;
use wasm_bindgen::prelude::*;

use crate::Viewer;

thread_local! {
    static RUNNER: RefCell<Option<(eframe::WebRunner, egui::Context)>> = const { RefCell::new(None) };
}

/// Called once by `start_web`, inside the app creator.
pub fn set(runner: eframe::WebRunner, ctx: egui::Context) {
    RUNNER.with(|r| *r.borrow_mut() = Some((runner, ctx)));
}

/// Run `f` on the viewer, then repaint. "busy" when called while the viewer runs a frame.
fn with_viewer<T>(f: impl FnOnce(&mut Viewer) -> Result<T, String>) -> Result<T, JsValue> {
    with_viewer_ctx(|v, _| f(v))
}

/// `with_viewer` with the egui context, for `Viewer::call`.
fn with_viewer_ctx<T>(
    f: impl FnOnce(&mut Viewer, &egui::Context) -> Result<T, String>,
) -> Result<T, JsValue> {
    let (runner, ctx) = RUNNER
        .with(|r| r.borrow().clone())
        .ok_or_else(|| JsValue::from_str("not started"))?;
    let out = {
        let mut app = runner
            .app_mut::<Viewer>()
            .ok_or_else(|| JsValue::from_str("busy"))?;
        f(&mut app, &ctx)
    };
    ctx.request_repaint();
    out.map_err(|e| JsValue::from_str(&e))
}

/// One op through `Viewer::call` (calls.rs).
fn call(op: &str, name: &str, body: &str) -> Result<String, JsValue> {
    with_viewer_ctx(|v, ctx| v.call(ctx, op, name, body))
}

/// Read data: "state", "storeys", "materials", "elements" (an `ElementFilter`, "" for the
/// default) or "element" (`{"id": "<GlobalId>"}`).
#[wasm_bindgen]
pub fn query(name: &str, args_json: &str) -> Result<String, JsValue> {
    call("query", name, args_json)
}

/// Run an API verb (`query("verbs")`, `query("describe", {"name"})`) as one undoable step.
#[wasm_bindgen]
pub fn verb(name: &str, args_json: &str) -> Result<(), JsValue> {
    call("verb", name, args_json).map(|_| ())
}

/// Run one ribbon action by its `name()` (see query "actions"), as the ribbon, the palette and
/// the keys do. Interactive ones (place, measure, sheet tools) start their mode.
#[wasm_bindgen]
pub fn action(name: &str) -> Result<(), JsValue> {
    call("action", name, "").map(|_| ())
}

/// Close the "belongs to someone else" alert without synchronizing (its Cancel, as egui's).
#[wasm_bindgen]
pub fn dismiss_belongs() -> Result<(), JsValue> {
    call("dismiss_belongs", "", "").map(drop)
}

/// The style panel's buttons: "revert" (back to the file as loaded or last saved), "reset" (asks
/// first, as egui does: ADR 0028), "save" (asks where, writes what differs from the default).
#[wasm_bindgen]
pub fn standards_action(name: &str) -> Result<(), JsValue> {
    call("standards_action", name, "").map(drop)
}

/// Change what is placed: `{kind?, storey?, material?, sizes?: {column?, beam_width?,
/// beam_height?, wall?, slab?}, top_offset_mm?}` (sizes within `bimble_api::verbs::SIZE_MM`;
/// `top_offset_mm`, a new slab's top relative to the level above, within ±`MAX_MM`). All or nothing.
/// `kind` while not placing starts placing, as `action("place_<kind>")`; `material` is a model material's name
/// or a library grade, as the `place` verb's.
#[wasm_bindgen]
pub fn set_placing(args_json: &str) -> Result<(), JsValue> {
    call("set_placing", "", args_json).map(drop)
}

/// Go to a tab or close it, as the tab row does: `{"op": "activate"|"close", "tab": <a tab of
/// query("open_views")>}`. Closing the last tab shows Home. `{"op": "open", "tab": {"type":
/// "view", "id"}}` opens any view (open_views.views), as a navigator row click does.
#[wasm_bindgen]
pub fn tab(args_json: &str) -> Result<(), JsValue> {
    call("tab", "", args_json).map(drop)
}

/// Apply commands (a JSON array) as one undoable step.
#[wasm_bindgen]
pub fn apply(cmds_json: &str) -> Result<(), JsValue> {
    call("apply", "", cmds_json).map(drop)
}

#[wasm_bindgen]
pub fn undo() -> Result<(), JsValue> {
    call("undo", "", "").map(drop)
}

#[wasm_bindgen]
pub fn redo() -> Result<(), JsValue> {
    call("redo", "", "").map(drop)
}

thread_local! {
    static ON_CHANGE: RefCell<Option<js_sys::Function>> = const { RefCell::new(None) };
    static LAST_SENT: RefCell<String> = const { RefCell::new(String::new()) };
}

/// Register the change callback. It gets one argument, a JSON string:
/// `{"selection": [GlobalId, primary first], "rev": n, "unsaved": bool, "file": name or null,
/// "view": a signature of what the view shows}`: section box, hidden/isolated counts, On paper,
/// the current tab and the open tabs' count change it (refresh "visibility", "open_views").
/// It runs inside a frame: a callback that calls back into an export gets "busy", so defer
/// such calls (`queueMicrotask`).
#[wasm_bindgen]
pub fn on_change(cb: js_sys::Function) {
    ON_CHANGE.with(|c| *c.borrow_mut() = Some(cb));
    // The next frame sends the current state to the new callback.
    LAST_SENT.with(|s| s.borrow_mut().clear());
    // The viewer draws only on change (ADR 0006): ask for that next frame now.
    if let Some((_, ctx)) = RUNNER.with(|r| r.borrow().clone()) {
        ctx.request_repaint();
    }
}

/// Called each frame from `Viewer::ui`: tell the page when the state it shows changed.
pub(crate) fn notify(v: &Viewer) {
    let Some(cb) = ON_CHANGE.with(|c| c.borrow().clone()) else {
        return;
    };
    // `unsaved` there is `state::UNSAVED`'s value: `ui` stores it from the same expression just
    // before calling this, in the same frame.
    let now = v.change_json();
    if LAST_SENT.with(|s| *s.borrow() == now) {
        return;
    }
    if let Err(e) = cb.call1(&JsValue::NULL, &JsValue::from_str(&now)) {
        log::warn!("on_change callback failed: {e:?}");
    }
    LAST_SENT.with(|s| *s.borrow_mut() = now);
}

/// Select elements by GlobalId (a JSON array, primary first); `[]` clears the selection.
#[wasm_bindgen]
pub fn select(ids_json: &str) -> Result<(), JsValue> {
    call("select", "", ids_json).map(drop)
}
/// `bimble_core::file::MAX_FILE_BYTES`.
#[wasm_bindgen]
pub fn open(name: String, bytes: Vec<u8>) -> Result<(), JsValue> {
    if bytes.len() > bimble_core::file::MAX_FILE_BYTES {
        return Err(JsValue::from_str(&format!(
            "file too large: {} bytes",
            bytes.len()
        )));
    }
    with_viewer(|v| {
        v.files
            .sender()
            .send(crate::files::Picked {
                purpose: crate::files::Purpose::Open,
                name,
                path: None,
                bytes: Some(bytes),
            })
            .map_err(|e| e.to_string())
    })
}
