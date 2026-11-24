//! The page calls the running viewer (ADR 0048): JSON in, JSON out, edits through the viewer's
//! own undo history. All logic stays in Rust; a Vue page only calls these exports.

use eframe::egui;
use std::cell::RefCell;
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
    // ponytail: `Status` is Serialize on wasm only (lib.rs), so its query stays here.
    if name == "status" {
        // The status bar's counts and its text; null without a model.
        return with_viewer(|v| crate::calls::json(v.status()));
    }
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
    with_viewer(|v| {
        if let Some(ed) = v.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
            ed.belongs = None;
        }
        Ok(())
    })
}

/// The style panel's buttons: "revert" (back to the file as loaded or last saved), "reset" (asks
/// first, as egui does: ADR 0028), "save" (asks where, writes what differs from the default).
#[wasm_bindgen]
pub fn standards_action(name: &str) -> Result<(), JsValue> {
    with_viewer(|v| {
        match name {
            "revert" => {
                v.opts.style = v.style_baseline.clone();
                v.needs_render = true;
            }
            "reset" => v.confirm = Some(crate::Confirm::ResetStandards),
            "save" => {
                v.save_style();
            }
            _ => return Err(format!("unknown standards action {name:?}")),
        }
        Ok(())
    })
}

/// Change what is placed: `{kind?, storey?, material?, sizes?: {column?, beam_width?,
/// beam_height?, wall?, slab?}, top_offset_mm?}` (sizes within `bimble_api::verbs::SIZE_MM`;
/// `top_offset_mm`, a new slab's top relative to the level above, within ±`MAX_MM`). All or nothing.
/// `kind` while not placing starts placing, as `action("place_<kind>")`; `material` is a model material's name
/// or a library grade, as the `place` verb's.
#[wasm_bindgen]
pub fn set_placing(args_json: &str) -> Result<(), JsValue> {
    let patch = crate::edit::PlacingPatch::parse(args_json).map_err(|e| JsValue::from_str(&e))?;
    let start = with_viewer(|v| {
        v.loaded
            .as_mut()
            .and_then(|l| l.edit.as_mut())
            .ok_or_else(|| READ_ONLY.to_string())?
            .set_placing(patch)
    })?;
    match start {
        Some(k) => action(&format!("place_{}", k.label().to_lowercase())),
        None => Ok(()),
    }
}

/// Go to a tab or close it, as the tab row does: `{"op": "activate"|"close", "tab": <a tab of
/// query("open_views")>}`. Closing the last tab shows Home. `{"op": "open", "tab": {"type":
/// "view", "id"}}` opens any view (open_views.views), as a navigator row click does.
#[wasm_bindgen]
pub fn tab(args_json: &str) -> Result<(), JsValue> {
    #[derive(serde::Deserialize)]
    struct Args {
        op: String,
        tab: serde_json::Value,
    }
    let a: Args = serde_json::from_str(args_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let t = crate::ribbon::TabRef::from_json(&a.tab).map_err(|e| JsValue::from_str(&e))?;
    let close = match a.op.as_str() {
        "activate" | "open" => false,
        "close" => true,
        o => return Err(JsValue::from_str(&format!("unknown op {o:?}"))),
    };
    let open = a.op == "open";
    with_viewer(|v| {
        if open {
            // Any view of query("open_views").views, as a navigator row click: it gets a tab.
            match &t {
                crate::ribbon::TabRef::View(id) if v.views.iter().any(|w| w.id == *id) => {}
                crate::ribbon::TabRef::View(_) => return Err("no such view".into()),
                _ => return Err("op \"open\" takes a view".into()),
            }
        } else if !close && v.sync_tabs().as_ref() != Some(&t) && !v.open_tabs.contains(&t) {
            return Err("no such open tab".into());
        }
        v.tab_verb(close, t);
        Ok(())
    })
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
        "view": [
            v.section_on as usize,
            v.temporary.as_ref().map_or(0, |t| t.elements.len()),
            v.temporary.as_ref().is_some_and(|t| t.isolate) as usize,
            v.hidden_ids.len(),
            v.isolated_ids.len(),
            v.hidden_in_view() as usize,
            v.on_paper() as usize,
            v.open_tabs.len(),
        ],
        "tab": v.current_tab().map(|t| t.to_json()),
        // A click adds a point, the mode ends, a size changes: refresh query("placing").
        "placing": v.loaded.as_ref().and_then(|l| l.edit.as_ref()).map(|ed| ed.placing_json()),
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
