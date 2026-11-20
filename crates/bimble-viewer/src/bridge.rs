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
            // "IFC2X3", "IFC4", "IFC4X3" as the file says, ".bimble", or null.
            "schema": v.loaded.as_ref().and_then(|l| {
                if l.path.extension().is_some_and(|e| e == "bimble") {
                    Some(".bimble".to_string())
                } else {
                    Some(l.scene.schema.clone()).filter(|s| !s.is_empty())
                }
            }),
            "version": env!("CARGO_PKG_VERSION"),
            // Settings → You, null when no name is set; email null when unset.
            "user": crate::ribbon::trigram(&v.settings.user_name).map(|t| serde_json::json!({
                "name": v.settings.user_name.trim(),
                "trigram": t,
                "email": Some(v.settings.user_email.trim()).filter(|e| !e.is_empty()),
            })),
        })),
        "storeys" => json(query::storeys(model(v)?)),
        "materials" => json(query::materials(model(v)?)),
        // Graphic standards (D5): see `standards::json`, plus what the three buttons can do now.
        "standards" => {
            let source = v
                .style_path
                .as_ref()
                .and_then(|p| p.file_name())
                .map(|n| n.to_string_lossy().into_owned());
            let mut j = crate::standards::json(&v.opts.style, source);
            j["actions"] = serde_json::json!({
                "revert": v.opts.style != v.style_baseline,
                "reset": v.opts.style != bimble_core::style::GraphicStyle::default(),
                "save": true,
            });
            json(j)
        }
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
            let current = v.sync_tabs();
            let levels = v.levels();
            let defaults = bimble_core::view::default_views(&levels);
            let views: Vec<_> = v
                .views
                .iter()
                .map(|w| {
                    let mut j = serde_json::to_value(query::ViewSummary {
                        id: w.id,
                        name: w.name.clone(),
                        kind: w.kind.clone(),
                        scale: w.scale,
                        crop: w.crop,
                        true_north: w.true_north,
                        references: w.references.len(),
                        annotations: w.annotations.len(),
                    })
                    .unwrap_or_default();
                    // Still the default name: what it says, for the page's language.
                    j["auto_name"] = crate::viewing::auto_name(w, &defaults, &levels)
                        .unwrap_or(serde_json::Value::Null);
                    j
                })
                .collect();
            let tabs: Vec<_> = v.open_tabs.iter().map(|t| t.to_json()).collect();
            json(serde_json::json!({
                "views": views,
                "active": v.active_view,
                "tabs": tabs,
                // What the main view shows: one of `tabs`, or null (Home).
                "current": current.map(|t| t.to_json()),
                // Model or On paper: offered for a plan or section (ADR 0046).
                "paper": {"offered": v.two_pictures(), "on": v.on_paper()},
            }))
        }
        // The schedules offered (built-in ones as changed, and the model's own), by name for
        // "schedule_rows".
        "schedule_list" => json(v.schedule_defs()),
        // Where the pointer is, as the status bar says it: `{"world_mm": [x, y, z], "text"}`
        // or null. Cheap (the last hover's result, no picking): poll it, it does not notify.
        "pointer" => json(
            v.pointer()
                .map(|(p, text)| serde_json::json!({"world_mm": p, "text": text})),
        ),
        // What is being placed (B1): `Editor::placing_json`; null without an editable model.
        "placing" => json(
            v.loaded
                .as_ref()
                .and_then(|l| l.edit.as_ref())
                .map(|ed| ed.placing_json()),
        ),
        // The status bar's counts and its text; null without a model.
        "status" => json(v.status()),
        // Working together (ADR 0038): see `collab::collab_json`. Null fields on the web (no
        // shared folder there yet).
        "collab" => {
            let ed = v.loaded.as_ref().and_then(|l| l.edit.as_ref());
            let others = ed
                .and_then(|e| e.borrowing.as_ref())
                .and_then(|b| bimble_core::repo::Repo::open_dir(&b.remote).ok())
                .and_then(|r| bimble_core::remote::borrowed(&r).ok())
                .unwrap_or_default();
            let last_sync = v
                .synced_at
                .map(|t| bimble_core::date::now_secs() - t.elapsed().as_secs() as i64);
            json(crate::collab::collab_json(ed, last_sync, &others))
        }
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
            json(bimble_drawing::schedule::table_json(
                &bimble_drawing::schedule::schedule(&l.scene, &def),
                &l.scene,
                MAX_LIST,
            ))
        }
        // A schedule as CSV (`to_csv`, as the egui export writes it), every row: `{"csv"}`.
        "schedule_csv" => {
            let a: NameArgs = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            let def = v
                .schedule_defs()
                .into_iter()
                .find(|d| d.name == a.name)
                .ok_or("no schedule with this name")?;
            let l = v.loaded.as_ref().ok_or("no model open")?;
            let t = bimble_drawing::schedule::schedule(&l.scene, &def);
            json(serde_json::json!({ "csv": bimble_drawing::schedule::to_csv(&t) }))
        }
        "sheet_svg" => {
            #[derive(serde::Deserialize)]
            struct Args {
                id: String,
            }
            let a: Args = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            let sh = v
                .sheets()
                .into_iter()
                .find(|s| s.id.to_string() == a.id)
                .ok_or_else(|| format!("no sheet with id {:?}", a.id))?;
            let l = v.loaded.as_ref().ok_or("no model open")?;
            // The options the egui sheet export uses (viewing.rs `export_sheet`).
            let o = bimble_drawing::sheetdraw::SheetOptions {
                style: v.opts.style.clone(),
                template: v.plan.template.with_project(&v.project_info()),
                project: l
                    .path
                    .file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default(),
                date: bimble_drawing::today(),
            };
            let (sheet, _) = bimble_drawing::sheetdraw::draw(&l.scene, &sh, &v.views, &o)?;
            json(serde_json::json!({ "svg": sheet.to_svg() }))
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
/// beam_height?, wall?, slab?}}` (mm, within `bimble_api::verbs::SIZE_MM`). All or nothing.
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
