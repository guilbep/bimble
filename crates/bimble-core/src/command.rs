//! Every model mutation is a `Command`. Applying a command returns its inverse, which gives
//! undo/redo for free and a replayable operation log for history and agents.

use crate::version::canonical;
use crate::{Element, Generator, GlobalId, Material, Model, Placement, Storey};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum Command {
    AddStorey(Storey),
    RemoveStorey {
        id: GlobalId,
    },
    AddMaterial(Material),
    RemoveMaterial {
        id: GlobalId,
    },
    AddElement(Element),
    RemoveElement {
        id: GlobalId,
    },
    SetGenerator {
        id: GlobalId,
        generator: Generator,
    },
    SetPlacement {
        id: GlobalId,
        placement: Placement,
    },
    Rename {
        id: GlobalId,
        name: String,
    },
    AddView(crate::view::View),
    RemoveView {
        id: GlobalId,
    },
    /// Replace a view (name, range, visibility, references…); its id stays.
    SetView(crate::view::View),
    AddSheet(crate::sheet::Sheet),
    RemoveSheet {
        id: GlobalId,
    },
    /// Replace a sheet (number, name, paper, placed views, texts…); its id stays.
    SetSheet(crate::sheet::Sheet),
    /// Replace the project information (ADR 0030).
    SetProject {
        project: std::collections::BTreeMap<String, String>,
    },
}

#[derive(Debug, thiserror::Error, PartialEq)]
pub enum CommandError {
    #[error("no such object: {0}")]
    NotFound(GlobalId),
    #[error("object already exists: {0}")]
    Exists(GlobalId),
    #[error("{0} is still used by elements")]
    InUse(GlobalId),
    #[error("invalid: {0}")]
    Invalid(String),
}

impl Command {
    /// Apply to the model. Returns the inverse command. On error the model is unchanged.
    pub fn apply(self, m: &mut Model) -> Result<Command, CommandError> {
        use Command::*;
        match self {
            AddStorey(s) => {
                if m.storeys.contains_key(&s.id) {
                    return Err(CommandError::Exists(s.id));
                }
                if !s.elevation.is_finite() {
                    return Err(CommandError::Invalid("storey elevation".into()));
                }
                // Validate first: canonical() goes through JSON, where NaN is not representable.
                let s = canonical(s);
                let id = s.id;
                m.storeys.insert(id, s);
                Ok(RemoveStorey { id })
            }
            RemoveStorey { id } => {
                let key = id.to_string();
                let in_view = m.views.values().any(|v| {
                    matches!(&v.kind, crate::view::ViewKind::Plan { storey, .. } if *storey == key)
                });
                if in_view || m.elements.values().any(|e| e.storey == id) {
                    return Err(CommandError::InUse(id));
                }
                let s = m.storeys.remove(&id).ok_or(CommandError::NotFound(id))?;
                Ok(AddStorey(s))
            }
            AddMaterial(mat) => {
                if m.materials.contains_key(&mat.id) {
                    return Err(CommandError::Exists(mat.id));
                }
                let id = mat.id;
                m.materials.insert(id, mat);
                Ok(RemoveMaterial { id })
            }
            RemoveMaterial { id } => {
                if m.elements.values().any(|e| e.material == id) {
                    return Err(CommandError::InUse(id));
                }
                let mat = m.materials.remove(&id).ok_or(CommandError::NotFound(id))?;
                Ok(AddMaterial(mat))
            }
            AddElement(e) => {
                if m.elements.contains_key(&e.id) {
                    return Err(CommandError::Exists(e.id));
                }
                if !m.storeys.contains_key(&e.storey) {
                    return Err(CommandError::NotFound(e.storey));
                }
                if !m.materials.contains_key(&e.material) {
                    return Err(CommandError::NotFound(e.material));
                }
                e.generator.validate().map_err(CommandError::Invalid)?;
                e.placement.validate().map_err(CommandError::Invalid)?;
                let e = canonical(e);
                let id = e.id;
                m.elements.insert(id, e);
                Ok(RemoveElement { id })
            }
            RemoveElement { id } => {
                let e = m.elements.remove(&id).ok_or(CommandError::NotFound(id))?;
                Ok(AddElement(e))
            }
            SetGenerator { id, generator } => {
                generator.validate().map_err(CommandError::Invalid)?;
                let generator = canonical(generator);
                let e = m.elements.get_mut(&id).ok_or(CommandError::NotFound(id))?;
                let old = std::mem::replace(&mut e.generator, generator);
                Ok(SetGenerator { id, generator: old })
            }
            SetPlacement { id, placement } => {
                placement.validate().map_err(CommandError::Invalid)?;
                let placement = canonical(placement);
                let e = m.elements.get_mut(&id).ok_or(CommandError::NotFound(id))?;
                let old = std::mem::replace(&mut e.placement, placement);
                Ok(SetPlacement { id, placement: old })
            }
            Rename { id, name } => {
                let e = m.elements.get_mut(&id).ok_or(CommandError::NotFound(id))?;
                let old = std::mem::replace(&mut e.name, name);
                Ok(Rename { id, name: old })
            }
            AddView(v) => {
                if m.views.contains_key(&v.id) {
                    return Err(CommandError::Exists(v.id));
                }
                v.validate(&m.levels()).map_err(CommandError::Invalid)?;
                let v = canonical(v);
                let id = v.id;
                m.views.insert(id, v);
                Ok(RemoveView { id })
            }
            RemoveView { id } => {
                // A view placed on a sheet stays (a default view is reset, not removed: it
                // keeps its id, so it stays placed).
                let defaults = crate::view::default_views(&m.levels());
                let is_default = defaults.iter().any(|d| d.id == id);
                if !is_default
                    && m.sheets
                        .values()
                        .any(|s| s.placed.iter().any(|p| p.view == id))
                {
                    return Err(CommandError::InUse(id));
                }
                let v = m.views.remove(&id).ok_or(CommandError::NotFound(id))?;
                Ok(AddView(v))
            }
            SetView(v) => {
                if !m.views.contains_key(&v.id) {
                    return Err(CommandError::NotFound(v.id));
                }
                v.validate(&m.levels()).map_err(CommandError::Invalid)?;
                let v = canonical(v);
                let old = m.views.insert(v.id, v).expect("checked above");
                Ok(SetView(old))
            }
            AddSheet(s) => {
                if m.sheets.contains_key(&s.id) {
                    return Err(CommandError::Exists(s.id));
                }
                check_sheet(m, &s)?;
                let s = canonical(s);
                let id = s.id;
                m.sheets.insert(id, s);
                Ok(RemoveSheet { id })
            }
            RemoveSheet { id } => {
                let s = m.sheets.remove(&id).ok_or(CommandError::NotFound(id))?;
                Ok(AddSheet(s))
            }
            SetSheet(s) => {
                if !m.sheets.contains_key(&s.id) {
                    return Err(CommandError::NotFound(s.id));
                }
                check_sheet(m, &s)?;
                let s = canonical(s);
                let old = m.sheets.insert(s.id, s).expect("checked above");
                Ok(SetSheet(old))
            }
            SetProject { project } => {
                crate::project::validate(&project).map_err(CommandError::Invalid)?;
                let old = std::mem::replace(&mut m.project, project);
                Ok(SetProject { project: old })
            }
        }
    }
}

/// A sheet is valid on its own, places only views of the model, and has its own number.
fn check_sheet(m: &Model, s: &crate::sheet::Sheet) -> Result<(), CommandError> {
    let ids = m.view_ids();
    s.validate(|v| ids.contains(&v))
        .map_err(CommandError::Invalid)?;
    if m.sheets
        .values()
        .any(|o| o.id != s.id && o.number == s.number)
    {
        return Err(CommandError::Invalid(format!(
            "sheet number {} is already used",
            s.number
        )));
    }
    Ok(())
}

impl Command {
    /// What applying it to `m` does, in a few words for the Undo / Redo buttons: "move P1",
    /// "change the sizes of B3", "add the sheet S-101". Names are read from `m` before applying.
    pub fn describe(&self, m: &Model) -> String {
        let el = |id: &GlobalId| {
            m.elements
                .get(id)
                .map_or_else(|| "an element".to_string(), |e| e.name.clone())
        };
        let view = |id: &GlobalId| {
            m.views
                .get(id)
                .map_or_else(|| "a view".to_string(), |v| format!("the view {}", v.name))
        };
        let sheet = |id: &GlobalId| {
            m.sheets.get(id).map_or_else(
                || "a sheet".to_string(),
                |s| format!("the sheet {}", s.number),
            )
        };
        match self {
            Command::AddStorey(s) => format!("add the storey {}", s.name),
            Command::RemoveStorey { id } => format!(
                "delete the storey {}",
                m.storeys.get(id).map_or("", |s| s.name.as_str())
            ),
            Command::AddMaterial(x) => format!("add the material {}", x.name),
            Command::RemoveMaterial { id } => format!(
                "delete the material {}",
                m.materials.get(id).map_or("", |x| x.name.as_str())
            ),
            Command::AddElement(e) => format!("add {}", e.name),
            Command::RemoveElement { id } => format!("delete {}", el(id)),
            Command::SetGenerator { id, .. } => format!("change the sizes of {}", el(id)),
            Command::SetPlacement { id, .. } => format!("move {}", el(id)),
            Command::Rename { id, name } => format!("rename {} to {name}", el(id)),
            Command::AddView(v) => format!("add the view {}", v.name),
            Command::RemoveView { id } => format!("delete {}", view(id)),
            Command::SetView(v) => match m.views.get(&v.id) {
                Some(old) if old.name != v.name => {
                    format!("rename the view {} to {}", old.name, v.name)
                }
                _ => format!("change the view {}", v.name),
            },
            Command::AddSheet(x) => format!("add the sheet {}", x.number),
            Command::RemoveSheet { id } => format!("delete {}", sheet(id)),
            Command::SetSheet(x) => format!("change the sheet {}", x.number),
            Command::SetProject { .. } => "change the project information".into(),
        }
    }
}

/// What a group of commands does, from its first one: "move P1", "add P2 and 3 more".
fn describe_group(cmds: &[Command], m: &Model) -> String {
    match cmds {
        [] => "nothing".into(),
        [c] => c.describe(m),
        [c, rest @ ..] => format!(
            "{} and {} more change{}",
            c.describe(m),
            rest.len(),
            if rest.len() == 1 { "" } else { "s" }
        ),
    }
}

/// One step of the history: what it did, in words, and the commands that take it back (or
/// forward again).
#[derive(Debug)]
struct Step {
    label: String,
    cmds: Vec<Command>,
}

/// Undo/redo stacks. Each entry is a group of inverses applied together (one user action), with
/// what that action did in words.
#[derive(Default, Debug)]
pub struct History {
    undo: Vec<Step>,
    redo: Vec<Step>,
}

impl History {
    /// Apply a group of commands as one transaction: all succeed or none are kept.
    pub fn apply(&mut self, m: &mut Model, cmds: Vec<Command>) -> Result<(), CommandError> {
        let label = describe_group(&cmds, m);
        let mut inverses = Vec::with_capacity(cmds.len());
        for c in cmds {
            match c.apply(m) {
                Ok(inv) => inverses.push(inv),
                Err(e) => {
                    for inv in inverses.into_iter().rev() {
                        inv.apply(m)
                            .expect("rollback of a just-applied command cannot fail");
                    }
                    return Err(e);
                }
            }
        }
        inverses.reverse();
        self.undo.push(Step {
            label,
            cmds: inverses,
        });
        self.redo.clear();
        Ok(())
    }

    /// What Undo would take back, in words; None when there is nothing to undo.
    pub fn undo_label(&self) -> Option<&str> {
        self.undo.last().map(|s| s.label.as_str())
    }

    /// What Redo would do again, in words; None when there is nothing to redo.
    pub fn redo_label(&self) -> Option<&str> {
        self.redo.last().map(|s| s.label.as_str())
    }

    pub fn undo(&mut self, m: &mut Model) -> bool {
        Self::swap(m, &mut self.undo, &mut self.redo)
    }

    pub fn redo(&mut self, m: &mut Model) -> bool {
        Self::swap(m, &mut self.redo, &mut self.undo)
    }

    fn swap(m: &mut Model, from: &mut Vec<Step>, to: &mut Vec<Step>) -> bool {
        let Some(step) = from.pop() else {
            return false;
        };
        let mut inv: Vec<Command> = step
            .cmds
            .into_iter()
            .map(|c| c.apply(m).expect("history inverse must apply"))
            .collect();
        inv.reverse();
        to.push(Step {
            label: step.label,
            cmds: inv,
        });
        true
    }
}

#[cfg(test)]
mod describe_tests {
    use super::*;
    use crate::demo::{FrameSpec, concrete_frame};

    /// Undo and Redo say what they would do, in words, and nothing when there is nothing.
    #[test]
    fn history_says_what_undo_and_redo_would_do() {
        let mut m = concrete_frame(&FrameSpec::default());
        let mut h = History::default();
        assert_eq!(h.undo_label(), None);
        let e = m.elements.values().next().unwrap().clone();
        let mut p = e.placement.clone();
        p.origin[0] += 100.0;
        h.apply(
            &mut m,
            vec![Command::SetPlacement {
                id: e.id,
                placement: p,
            }],
        )
        .unwrap();
        let moved = format!("move {}", e.name);
        assert_eq!(h.undo_label(), Some(moved.as_str()));
        assert_eq!(h.redo_label(), None);
        assert!(h.undo(&mut m));
        assert_eq!(h.undo_label(), None);
        assert_eq!(h.redo_label(), Some(moved.as_str()));
        // A rename, then several changes in one step.
        h.apply(
            &mut m,
            vec![Command::Rename {
                id: e.id,
                name: "X1".into(),
            }],
        )
        .unwrap();
        assert_eq!(
            h.undo_label(),
            Some(format!("rename {} to X1", e.name).as_str())
        );
        assert_eq!(
            h.redo_label(),
            None,
            "a new change drops what could be redone"
        );
        let ids: Vec<GlobalId> = m.elements.keys().take(3).copied().collect();
        let names: Vec<String> = ids.iter().map(|i| m.elements[i].name.clone()).collect();
        h.apply(
            &mut m,
            ids.iter()
                .map(|id| Command::RemoveElement { id: *id })
                .collect(),
        )
        .unwrap();
        assert_eq!(
            h.undo_label(),
            Some(format!("delete {} and 2 more changes", names[0]).as_str())
        );
    }
}
