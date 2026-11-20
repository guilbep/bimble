//! The page calls the running viewer (ADR 0048): JSON in, JSON out, edits through the viewer's
//! own undo history. All logic stays in Rust; a Vue page only calls these exports.

use std::cell::RefCell;

use bimble_api::query;
use bimble_core::Command;
use eframe::egui;
use wasm_bindgen::prelude::*;

use crate::{READ_ONLY, Viewer, state};

thread_local! {
    static RUNNER: RefCell<Option<(eframe::WebRunner, egui::Context)>> = const { RefCell::new(None) };
}

/// Called once by `start_web`, inside the app creator.
pub fn set(runner: eframe::WebRunner, ctx: egui::Context) {
    RUNNER.with(|r| *r.borrow_mut() = Some((runner, ctx)));
}

/// Run `f` on the viewer, then repaint. "busy" when called while the viewer runs a frame.
fn with_viewer<T>(f: impl FnOnce(&mut Viewer) -> Result<T, String>) -> Result<T, JsValue> {
    let (runner, ctx) = RUNNER
        .with(|r| r.borrow().clone())
        .ok_or_else(|| JsValue::from_str("not started"))?;
    let out = {
        let mut app = runner
            .app_mut::<Viewer>()
            .ok_or_else(|| JsValue::from_str("busy"))?;
        f(&mut app)
    };
    ctx.request_repaint();
    out.map_err(|e| JsValue::from_str(&e))
}

fn model(v: &Viewer) -> Result<&bimble_core::Model, String> {
    v.loaded
        .as_ref()
        .and_then(|l| l.edit.as_ref())
        .map(|ed| &ed.model)
        .ok_or_else(|| READ_ONLY.to_string())
}

fn json(v: impl serde::Serialize) -> Result<String, String> {
    serde_json::to_string(&v).map_err(|e| e.to_string())
}

/// Read data: "state", "storeys", "materials", "elements" (an `ElementFilter`, "" for the
/// default) or "element" (`{"id": "<GlobalId>"}`).
#[wasm_bindgen]
pub fn query(name: &str, args_json: &str) -> Result<String, JsValue> {
    with_viewer(|v| match name {
        "state" => json(serde_json::json!({
            "file": v.loaded.as_ref().and_then(|l| l.path.file_name()).map(|n| n.to_string_lossy()),
            "editable": v.loaded.as_ref().is_some_and(|l| l.edit.is_some()),
            "unsaved": !state::unsaved(&v.snapshot()).is_empty(),
        })),
        "storeys" => json(query::storeys(model(v)?)),
        "materials" => json(query::materials(model(v)?)),
        "elements" => {
            let filter: query::ElementFilter = if args_json.trim().is_empty() {
                Default::default()
            } else {
                serde_json::from_str(args_json).map_err(|e| e.to_string())?
            };
            json(query::elements(model(v)?, &filter))
        }
        "element" => {
            #[derive(serde::Deserialize)]
            struct Args {
                id: bimble_core::GlobalId,
            }
            let a: Args = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            let d = query::element(model(v)?, a.id).ok_or("no element with this id")?;
            json(d)
        }
        _ => Err(format!("unknown query {name:?}")),
    })
}

/// Apply commands (a JSON array) as one undoable step.
#[wasm_bindgen]
pub fn apply(cmds_json: &str) -> Result<(), JsValue> {
    let cmds: Vec<Command> =
        serde_json::from_str(cmds_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    with_viewer(|v| v.try_apply(cmds))
}

#[wasm_bindgen]
pub fn undo() -> Result<(), JsValue> {
    undo_redo(true)
}

#[wasm_bindgen]
pub fn redo() -> Result<(), JsValue> {
    undo_redo(false)
}

fn undo_redo(undo: bool) -> Result<(), JsValue> {
    with_viewer(|v| {
        model(v)?;
        v.undo_redo(undo);
        Ok(())
    })
}

thread_local! {
    static ON_CHANGE: RefCell<Option<js_sys::Function>> = const { RefCell::new(None) };
    static LAST_SENT: RefCell<String> = const { RefCell::new(String::new()) };
}

/// Register the change callback. It gets one argument, a JSON string:
/// `{"selection": [GlobalId, primary first], "rev": n, "unsaved": bool, "file": name or null}`.
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
    let ids: Vec<&str> = v
        .loaded
        .as_ref()
        .map(|l| {
            v.selected
                .into_iter()
                .chain(v.also_selected.iter().copied())
                .filter_map(|i| l.scene.elements.get(i).map(|e| e.global_id.as_str()))
                .collect()
        })
        .unwrap_or_default();
    let now = serde_json::json!({
        "selection": ids,
        "rev": v.scene_rev,
        "unsaved": state::UNSAVED.load(std::sync::atomic::Ordering::Relaxed),
        "file": v.loaded.as_ref().and_then(|l| l.path.file_name()).map(|n| n.to_string_lossy()),
    })
    .to_string();
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
    let ids: Vec<String> =
        serde_json::from_str(ids_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    with_viewer(|v| {
        let l = v.loaded.as_ref().ok_or("no model open")?;
        // ponytail: linear scan per id, a GlobalId index if pages select thousands at once.
        let found: Vec<Option<usize>> = ids
            .iter()
            .map(|g| l.scene.elements.iter().position(|e| e.global_id == *g))
            .collect();
        let unknown: Vec<&str> = ids
            .iter()
            .zip(&found)
            .filter(|(_, i)| i.is_none())
            .map(|(g, _)| g.as_str())
            .collect();
        if !unknown.is_empty() {
            return Err(format!("unknown GlobalIds: {}", unknown.join(", ")));
        }
        let mut idx = found.into_iter().flatten();
        v.clear_selection();
        v.selected = idx.next();
        v.also_selected.extend(idx);
        Ok(())
    })
}

/// Open a model from bytes, as if picked (same path as `?url=`). Refuses more than
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
