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

/// Undo/redo stacks. Each entry is a group of inverses applied together (one user action).
#[derive(Default, Debug)]
pub struct History {
    undo: Vec<Vec<Command>>,
    redo: Vec<Vec<Command>>,
}

impl History {
    /// Apply a group of commands as one transaction: all succeed or none are kept.
    pub fn apply(&mut self, m: &mut Model, cmds: Vec<Command>) -> Result<(), CommandError> {
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
        self.undo.push(inverses);
        self.redo.clear();
        Ok(())
    }

    pub fn undo(&mut self, m: &mut Model) -> bool {
        Self::swap(m, &mut self.undo, &mut self.redo)
    }

    pub fn redo(&mut self, m: &mut Model) -> bool {
        Self::swap(m, &mut self.redo, &mut self.undo)
    }

    fn swap(m: &mut Model, from: &mut Vec<Vec<Command>>, to: &mut Vec<Vec<Command>>) -> bool {
        let Some(group) = from.pop() else {
            return false;
        };
        let mut inv: Vec<Command> = group
            .into_iter()
            .map(|c| c.apply(m).expect("history inverse must apply"))
            .collect();
        inv.reverse();
        to.push(inv);
        true
    }
}
