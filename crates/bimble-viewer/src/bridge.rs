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
        // Every ribbon action with its state now, for the page to draw (ADR 0048).
        "actions" => json(
            crate::ribbon::ALL_ACTIONS
                .iter()
                .map(|&a| {
                    serde_json::json!({
                        "name": a.name(),
                        "label": a.label(),
                        "tab": a.home(),
                        "enabled": v.action_enabled(a),
                        "pressed": v.action_on(a),
                        "shortcut": Some(a.keys()).filter(|k| !k.is_empty()),
                    })
                })
                .collect::<Vec<_>>(),
        ),
        "verbs" => json(bimble_api::dispatch::list()),
        "describe" => {
            let a: NameArgs = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            json(bimble_api::dispatch::describe(&a.name).ok_or("no verb with this name")?)
        }
        // Newest first, at most MAX_LIST each.
        "history" => {
            let h = v
                .loaded
                .as_ref()
                .and_then(|l| l.edit.as_ref())
                .map(|e| &e.history);
            let undo: Vec<&str> = h
                .map(|h| h.undo_labels().take(MAX_LIST).collect())
                .unwrap_or_default();
            let redo: Vec<&str> = h
                .map(|h| h.redo_labels().take(MAX_LIST).collect())
                .unwrap_or_default();
            json(serde_json::json!({"undo": undo, "redo": redo}))
        }
        // The views the viewer shows (stored over the defaults made on load), not only the
        // model's stored ones; the one shown; the tabs over the main view, in order.
        "open_views" => {
            use crate::ribbon::TabRef;
            let views: Vec<_> = v
                .views
                .iter()
                .map(|w| query::ViewSummary {
                    id: w.id,
                    name: w.name.clone(),
                    kind: w.kind.clone(),
                    scale: w.scale,
                    crop: w.crop,
                    true_north: w.true_north,
                    references: w.references.len(),
                    annotations: w.annotations.len(),
                })
                .collect();
            let tabs: Vec<_> = v
                .open_tabs
                .iter()
                .map(|t| match t {
                    TabRef::View(id) => serde_json::json!({"type": "view", "id": id}),
                    TabRef::Sheet(id) => serde_json::json!({"type": "sheet", "id": id}),
                    TabRef::Schedule(n) => serde_json::json!({"type": "schedule", "name": n}),
                    TabRef::Project => serde_json::json!({"type": "project"}),
                    TabRef::Materials => serde_json::json!({"type": "materials"}),
                })
                .collect();
            json(serde_json::json!({
                "views": views,
                "active": v.active_view,
                "tabs": tabs,
            }))
        }
        // The schedules offered (built-in ones as changed, and the model's own), by name for
        // "schedule_rows".
        "schedule_list" => json(v.schedule_defs()),
        "visibility" => {
            let t = v.temporary.as_ref();
            let now = |iso: bool| capped(t.filter(|t| t.isolate == iso).map(|t| &t.elements));
            let origin = v.loaded.as_ref().map(|l| l.scene.origin_mm);
            json(serde_json::json!({
                "hidden_now": now(false),
                "isolated_now": now(true),
                "hidden_in_view": capped(Some(&v.hidden_ids)),
                "isolated_in_view": capped(Some(&v.isolated_ids)),
                "section_box": {
                    "on": v.section_on,
                    // mm, relative to the scene origin; `origin_mm` adds back to model mm.
                    "box": v.section_box.map(|b| serde_json::json!({
                        "min": b.min.to_array(), "max": b.max.to_array(), "yaw": b.yaw,
                    })),
                    "origin_mm": origin,
                },
            }))
        }
        "schedule_rows" => {
            let a: NameArgs = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            let def = v
                .schedule_defs()
                .into_iter()
                .find(|d| d.name == a.name)
                .ok_or("no schedule with this name")?;
            let l = v.loaded.as_ref().ok_or("no model open")?;
            json(table_json(
                &bimble_drawing::schedule::schedule(&l.scene, &def),
                MAX_LIST,
            ))
        }
        _ => {
            let args = if args_json.trim().is_empty() {
                serde_json::Value::Null
            } else {
                serde_json::from_str(args_json).map_err(|e| e.to_string())?
            };
            json(bimble_api::dispatch::query(model(v)?, name, args)?)
        }
    })
}

/// At most this many ids or rows in one answer; the total is given beside.
const MAX_LIST: usize = 50;

#[derive(serde::Deserialize)]
struct NameArgs {
    name: String,
}

/// `{"ids": first MAX_LIST, "count": total}`.
fn capped(s: Option<&std::collections::BTreeSet<String>>) -> serde_json::Value {
    let s = s.into_iter().flatten();
    serde_json::json!({
        "ids": s.clone().take(MAX_LIST).collect::<Vec<_>>(),
        "count": s.count(),
    })
}

/// A schedule table as text cells, as the CSV writes them; at most `max` rows in all.
fn table_json(t: &bimble_drawing::schedule::Table, max: usize) -> serde_json::Value {
    use bimble_drawing::schedule::Cell;
    let cells = |c: &[Cell]| c.iter().map(Cell::text).collect::<Vec<_>>();
    let mut left = max;
    let groups: Vec<_> = t
        .groups
        .iter()
        .map(|g| {
            let n = g.rows.len().min(left);
            left -= n;
            serde_json::json!({
                "level": g.level,
                "rows": g.rows[..n].iter().map(|r| cells(&r.cells)).collect::<Vec<_>>(),
                "subtotal": cells(&g.subtotal),
            })
        })
        .collect();
    serde_json::json!({
        "name": t.name,
        "columns": t.fields.iter().map(|f| f.title()).collect::<Vec<_>>(),
        "groups": groups,
        "total": t.total.as_deref().map(cells),
        "count": t.count,
        "truncated": t.count > max,
    })
}

/// Run an API verb (`query("verbs")`, `query("describe", {"name"})`) as one undoable step.
#[wasm_bindgen]
pub fn verb(name: &str, args_json: &str) -> Result<(), JsValue> {
    let args = if args_json.trim().is_empty() {
        serde_json::Value::Null
    } else {
        serde_json::from_str(args_json).map_err(|e| JsValue::from_str(&e.to_string()))?
    };
    with_viewer(|v| {
        let cmds = bimble_api::dispatch::run(model(v)?, name, args)?;
        v.try_apply(cmds)
    })
}

/// Run one ribbon action by its `name()` (see query "actions"), as the ribbon, the palette and
/// the keys do. Interactive ones (place, measure, sheet tools) start their mode.
#[wasm_bindgen]
pub fn action(name: &str) -> Result<(), JsValue> {
    let a = crate::ribbon::Action::from_name(name)
        .ok_or_else(|| JsValue::from_str(&format!("unknown action {name:?}")))?;
    let ctx = RUNNER
        .with(|r| r.borrow().as_ref().map(|(_, c)| c.clone()))
        .ok_or_else(|| JsValue::from_str("not started"))?;
    with_viewer(|v| {
        if !v.action_enabled(a) {
            return Err(format!("action {name:?} is not enabled now"));
        }
        v.run_action(a, &ctx);
        Ok(())
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
