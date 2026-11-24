//! The bridge calls' bodies, without any target-specific cfg (exp2): the wasm exports in
//! `bridge.rs` and a native server run the same code. JSON in, JSON out.

use bimble_api::query;
use bimble_core::Command;
use eframe::egui;

use crate::{READ_ONLY, Viewer, state};

/// The ops `Viewer::call` serves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Op {
    Apply,
    Undo,
    Redo,
    Select,
    Query,
    Action,
    Verb,
}

/// The op for a name, or "unknown op <name>".
pub(crate) fn dispatch_op(op: &str) -> Result<Op, String> {
    Ok(match op {
        "apply" => Op::Apply,
        "undo" => Op::Undo,
        "redo" => Op::Redo,
        "select" => Op::Select,
        "query" => Op::Query,
        "action" => Op::Action,
        "verb" => Op::Verb,
        _ => return Err(format!("unknown op {op}")),
    })
}

/// What `call("query", "status")` says outside the wasm bridge (see `call_query`).
pub(crate) const STATUS_WASM_ONLY: &str = "query status is served by the wasm bridge only";

pub(crate) fn json(v: impl serde::Serialize) -> Result<String, String> {
    serde_json::to_string(&v).map_err(|e| e.to_string())
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

pub(crate) fn model(v: &Viewer) -> Result<&bimble_core::Model, String> {
    v.loaded
        .as_ref()
        .and_then(|l| l.edit.as_ref())
        .map(|ed| &ed.model)
        .ok_or_else(|| READ_ONLY.to_string())
}

impl Viewer {
    /// One bridge call by op name; the same bodies serve wasm (bridge.rs) and a native server.
    /// Ok(text) is what the wasm export returns (empty for unit); Err(text) is what it throws.
    pub(crate) fn call(
        &mut self,
        ctx: &egui::Context,
        op: &str,
        name: &str,
        body: &str,
    ) -> Result<String, String> {
        match dispatch_op(op)? {
            Op::Apply => {
                let cmds: Vec<Command> = serde_json::from_str(body).map_err(|e| e.to_string())?;
                self.try_apply(cmds)?;
                Ok(String::new())
            }
            Op::Undo | Op::Redo => {
                model(self)?;
                self.undo_redo(op == "undo");
                Ok(String::new())
            }
            Op::Select => {
                let ids: Vec<String> = serde_json::from_str(body).map_err(|e| e.to_string())?;
                self.call_select(&ids)?;
                Ok(String::new())
            }
            Op::Query => self.call_query(name, body),
            Op::Action => {
                self.call_action(ctx, name)?;
                Ok(String::new())
            }
            Op::Verb => {
                self.call_verb(name, body)?;
                Ok(String::new())
            }
        }
    }

    /// Read data: "state", "storeys", "materials", "elements" (an `ElementFilter`, "" for the
    /// default) or "element" (`{"id": "<GlobalId>"}`).
    fn call_query(&mut self, name: &str, args_json: &str) -> Result<String, String> {
        let v = self;
        match name {
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
        // Home's recent files, newest first, as egui lists them (settings.recent). The settings
        // keep paths only: opened_unix is null; schema is ".bimble" by extension, else null.
        "recent_files" => json(
            v.settings
                .recent
                .iter()
                .map(|p| {
                    let path = std::path::Path::new(p);
                    serde_json::json!({
                        "name": path.file_name().map_or_else(|| p.clone(), |n| n.to_string_lossy().into_owned()),
                        "path": p,
                        "folder": path.parent().map(|d| d.display().to_string()).unwrap_or_default(),
                        "opened_unix": null,
                        "schema": path.extension().is_some_and(|e| e == "bimble").then_some(".bimble"),
                        "exists": std::fs::metadata(path).is_ok(),
                    })
                })
                .collect::<Vec<_>>(),
        ),
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
        // The indices of `items` matching `q`, as the egui palette filters (case- and accent-insensitive).
        "palette_match" => {
            #[derive(serde::Deserialize)]
            struct Args {
                q: String,
                items: Vec<String>,
            }
            let a: Args = serde_json::from_str(args_json).map_err(|e| e.to_string())?;
            json(crate::ribbon::palette_match(&a.items, &a.q))
        }
        // The last 5 actions run (ribbon, palette, keys, bridge), newest first, each once: [name].
        "recent_actions" => json(v.recent_actions.iter().map(|a| a.name()).collect::<Vec<_>>()),
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
        // Linked models and reference plans, as the Links panel lists them: see `state::links_json`.
        "links" => {
            let models: Vec<(String, bool)> = v
                .links
                .iter()
                .map(|k| (k.name(), k.mode != crate::LinkMode::Hidden))
                .collect();
            let refs: Vec<_> = v.views.iter().flat_map(|w| &w.references).collect();
            json(state::links_json(&models, &refs))
        }
        // `[{id, label, value_mm}]`: see `state::measurements_json`.
        "measurements" => json(state::measurements_json(&v.dimensions)),
        // The status bar's counts and its text; null without a model. bridge.rs answers it:
        // `Status` derives `serde::Serialize` on wasm only (lib.rs), so not here yet.
        "status" => Err(STATUS_WASM_ONLY.to_string()),
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
    }
    }

    /// Run an API verb (`query("verbs")`, `query("describe", {"name"})`) as one undoable step.
    fn call_verb(&mut self, name: &str, args_json: &str) -> Result<(), String> {
        let args = if args_json.trim().is_empty() {
            serde_json::Value::Null
        } else {
            serde_json::from_str(args_json).map_err(|e| e.to_string())?
        };
        let v = self;
        let cmds = bimble_api::dispatch::run(model(v)?, name, args)?;
        v.try_apply(cmds)
    }

    /// Run one ribbon action by its `name()` (see query "actions"), as the ribbon, the palette
    /// and the keys do. Interactive ones (place, measure, sheet tools) start their mode.
    fn call_action(&mut self, ctx: &egui::Context, name: &str) -> Result<(), String> {
        let a = crate::ribbon::Action::from_name(name)
            .ok_or_else(|| format!("unknown action {name:?}"))?;
        let v = self;
        if !v.action_enabled(a) {
            return Err(format!("action {name:?} is not enabled now"));
        }
        v.run_action(a, ctx);
        Ok(())
    }

    /// Select elements by GlobalId, primary first; `[]` clears the selection.
    fn call_select(&mut self, ids: &[String]) -> Result<(), String> {
        let v = self;
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
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `Viewer::new` needs a wgpu RenderState, so this tests the op dispatch `call` runs first.
    #[test]
    fn unknown_op_is_an_error() {
        let e = dispatch_op("nope").unwrap_err();
        assert!(e.contains("unknown op"), "{e}");
        assert_eq!(dispatch_op("select"), Ok(Op::Select));
        assert_eq!(dispatch_op("query"), Ok(Op::Query));
        assert_eq!(dispatch_op("action"), Ok(Op::Action));
        assert_eq!(dispatch_op("verb"), Ok(Op::Verb));
    }
}
