//! The bridge calls' bodies, without any `cfg(target_arch)` (exp2): the wasm exports in
//! `bridge.rs` and a native server run the same code. JSON in, JSON out.
//! `query` is not here yet: its body derives `serde::Deserialize`, and `serde` is a
//! wasm-only dependency of this crate.

use bimble_core::Command;
use eframe::egui;

use crate::{READ_ONLY, Viewer};

/// The ops `Viewer::call` serves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Op {
    Apply,
    Undo,
    Redo,
    Select,
}

/// The op for a name, or "unknown op <name>".
pub(crate) fn dispatch_op(op: &str) -> Result<Op, String> {
    Ok(match op {
        "apply" => Op::Apply,
        "undo" => Op::Undo,
        "redo" => Op::Redo,
        "select" => Op::Select,
        _ => return Err(format!("unknown op {op}")),
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
        _ctx: &egui::Context,
        op: &str,
        _name: &str,
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
        }
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
    }
}
