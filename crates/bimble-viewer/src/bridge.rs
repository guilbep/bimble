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
