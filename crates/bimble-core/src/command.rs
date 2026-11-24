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
    /// Give an element another material of the model; its id stays.
    SetElementMaterial {
        id: GlobalId,
        material: GlobalId,
    },
    /// Set one property of an element ("Pset_Name/Property", or a bare name such as "Note"), or
    /// remove it when there is no value (ADR 0064). Text only, one line.
    SetProperty {
        id: GlobalId,
        key: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        value: Option<String>,
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
    /// Replace a material (name, family, properties); its id stays (ADR 0031).
    SetMaterial(Material),
    /// Replace the model's schedules (ADR 0029): the whole list, small.
    SetSchedules {
        schedules: Vec<crate::schedule::ScheduleDef>,
    },
    /// Replace the project information (ADR 0030).
    SetProject {
        project: std::collections::BTreeMap<String, String>,
    },
}

/// Longest property name and value a `SetProperty` accepts, in characters. Files are untrusted
/// input, and so is what a person types.
pub const PROPERTY_KEY_MAX: usize = 128;
pub const PROPERTY_VALUE_MAX: usize = 2000;

/// A property name or value typed by a person: not blank, not padded, one line, bounded.
fn property_text(what: &str, s: &str, max: usize) -> Result<(), CommandError> {
    let bad = |why: &str| Err(CommandError::Invalid(format!("{what} {why}")));
    if s.trim().is_empty() {
        return bad("is empty");
    }
    if s.trim() != s {
        return bad("starts or ends with a space");
    }
    if s.chars().any(char::is_control) {
        return bad("has a line break or a control character");
    }
    if s.chars().count() > max {
        return bad(&format!("is longer than {max} characters"));
    }
    Ok(())
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
    /// The element this command creates or changes, if it is about one (ADR 0038: what is
    /// borrowed on a first edit; a new element belongs to whoever made it until they sync).
    pub fn element(&self) -> Option<GlobalId> {
        match self {
            Command::AddElement(e) => Some(e.id),
            Command::RemoveElement { id }
            | Command::SetGenerator { id, .. }
            | Command::SetPlacement { id, .. }
            | Command::Rename { id, .. }
            | Command::SetElementMaterial { id, .. }
            | Command::SetProperty { id, .. } => Some(*id),
            // Listed, not `_`: a new element command must say whether it borrows.
            Command::AddStorey(_)
            | Command::RemoveStorey { .. }
            | Command::AddMaterial(_)
            | Command::RemoveMaterial { .. }
            | Command::AddView(_)
            | Command::RemoveView { .. }
            | Command::SetView(_)
            | Command::AddSheet(_)
            | Command::RemoveSheet { .. }
            | Command::SetSheet(_)
            | Command::SetMaterial(_)
            | Command::SetSchedules { .. }
            | Command::SetProject { .. } => None,
        }
    }

    /// Apply to the model. Returns the inverse command. On error the model is unchanged.
    /// A new name must not be blank; an inverse applied by undo, redo or rollback goes through
    /// `apply_inverse`, which may restore a blank name that came in with an IFC import.
    pub fn apply(self, m: &mut Model) -> Result<Command, CommandError> {
        if let Command::Rename { name, .. } = &self
            && name.trim().is_empty()
        {
            return Err(CommandError::Invalid("the name is empty".into()));
        }
        if let Command::SetProperty { key, value, .. } = &self {
            property_text("the property name", key, PROPERTY_KEY_MAX)?;
            if let Some(v) = value {
                property_text("the value", v, PROPERTY_VALUE_MAX)?;
            }
        }
        self.apply_inverse(m)
    }

    /// Apply without the checks on user input that a model's own state may not pass (a blank
    /// name). Only for inverses returned by `apply`.
    fn apply_inverse(self, m: &mut Model) -> Result<Command, CommandError> {
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
                mat.props.validate().map_err(CommandError::Invalid)?;
                let mat = canonical(mat);
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
            SetElementMaterial { id, material } => {
                if !m.materials.contains_key(&material) {
                    return Err(CommandError::NotFound(material));
                }
                let e = m.elements.get_mut(&id).ok_or(CommandError::NotFound(id))?;
                let old = std::mem::replace(&mut e.material, material);
                Ok(SetElementMaterial { id, material: old })
            }
            SetProperty { id, key, value } => {
                let e = m.elements.get_mut(&id).ok_or(CommandError::NotFound(id))?;
                let old = match value {
                    Some(v) => e.properties.insert(key.clone(), v),
                    None => e.properties.remove(&key),
                };
                Ok(SetProperty {
                    id,
                    key,
                    value: old,
                })
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
            SetMaterial(x) => {
                if !m.materials.contains_key(&x.id) {
                    return Err(CommandError::NotFound(x.id));
                }
                if x.name.trim().is_empty() || x.name.chars().count() > 200 {
                    return Err(CommandError::Invalid(
                        "a material needs a name (at most 200 characters)".into(),
                    ));
                }
                x.props.validate().map_err(CommandError::Invalid)?;
                // Materials are shown, scheduled and selected by name: two of one name would be
                // one in every list.
                if m.materials
                    .values()
                    .any(|o| o.id != x.id && o.name.trim().eq_ignore_ascii_case(x.name.trim()))
                {
                    return Err(CommandError::Invalid(format!(
                        "there is already a material named {:?}",
                        x.name.trim()
                    )));
                }
                let x = canonical(x);
                let old = m.materials.insert(x.id, x).expect("checked above");
                Ok(SetMaterial(old))
            }
            SetSchedules { schedules } => {
                crate::schedule::validate_all(&schedules).map_err(CommandError::Invalid)?;
                let old = std::mem::replace(&mut m.schedules, schedules);
                Ok(SetSchedules { schedules: old })
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
            Command::SetElementMaterial { id, material } => format!(
                "set the material of {} to {}",
                el(id),
                m.materials.get(material).map_or("", |x| x.name.as_str())
            ),
            Command::SetProperty {
                id,
                key,
                value: Some(_),
            } => format!("set {key} of {}", el(id)),
            Command::SetProperty {
                id,
                key,
                value: None,
            } => format!("remove {key} from {}", el(id)),
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
            Command::SetSchedules { schedules } => {
                let old: Vec<&str> = m.schedules.iter().map(|s| s.name.as_str()).collect();
                let new: Vec<&str> = schedules.iter().map(|s| s.name.as_str()).collect();
                match (
                    new.iter().find(|n| !old.contains(n)),
                    old.iter().find(|n| !new.contains(n)),
                ) {
                    (Some(n), None) => format!("add the schedule {n}"),
                    (None, Some(n)) => format!("delete the schedule {n}"),
                    (Some(n), Some(o)) => format!("rename the schedule {o} to {n}"),
                    (None, None) => schedules
                        .iter()
                        .find(|s| !m.schedules.contains(s))
                        .map_or("change the schedules".into(), |s| {
                            format!("change the schedule {}", s.name)
                        }),
                }
            }
            Command::SetMaterial(x) => match m.materials.get(&x.id) {
                Some(old) if old.name != x.name => {
                    format!("rename the material {} to {}", old.name, x.name)
                }
                _ => format!("change the material {}", x.name),
            },
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
                        inv.apply_inverse(m)
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

    /// Everything Undo can take back, in words, newest first.
    pub fn undo_labels(&self) -> impl Iterator<Item = &str> {
        self.undo.iter().rev().map(|s| s.label.as_str())
    }

    /// Everything Redo can do again, in words, newest first.
    pub fn redo_labels(&self) -> impl Iterator<Item = &str> {
        self.redo.iter().rev().map(|s| s.label.as_str())
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
            .map(|c| c.apply_inverse(m).expect("history inverse must apply"))
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

    /// The whole undo and redo lists, newest first: three renames, one undone.
    #[test]
    fn history_lists_every_label_newest_first() {
        let mut m = concrete_frame(&FrameSpec::default());
        let mut h = History::default();
        let id = *m.elements.keys().next().unwrap();
        let old = m.elements[&id].name.clone();
        for n in ["A", "B", "C"] {
            let cmd = Command::Rename { id, name: n.into() };
            h.apply(&mut m, vec![cmd]).unwrap();
        }
        assert!(h.undo(&mut m));
        assert_eq!(
            h.undo_labels().collect::<Vec<_>>(),
            ["rename A to B".to_string(), format!("rename {old} to A")]
        );
        assert_eq!(h.redo_labels().collect::<Vec<_>>(), ["rename B to C"]);
    }

    /// A rename to a blank name is refused and changes nothing; an element imported without a
    /// name can still be renamed, and undo gives back its empty name.
    #[test]
    fn rename_refuses_a_blank_name_and_undo_restores_an_empty_one() {
        let mut m = concrete_frame(&FrameSpec::default());
        let id = *m.elements.keys().next().unwrap();
        let old = m.elements[&id].name.clone();
        let before = m.clone();
        let mut h = History::default();
        for blank in ["", "   ", " \t\n"] {
            let rename = || Command::Rename {
                id,
                name: blank.into(),
            };
            let e = rename().apply(&mut m).unwrap_err();
            assert_eq!(e.to_string(), "invalid: the name is empty");
            let e = h.apply(&mut m, vec![rename()]).unwrap_err();
            assert_eq!(e.to_string(), "invalid: the name is empty");
            let json = format!(r#"{{"op":"rename","id":"{id}","name":{blank:?}}}"#);
            let c: Command = serde_json::from_str(&json).unwrap();
            assert!(c.apply(&mut m).is_err());
            assert_eq!(m, before);
        }
        let p1 = || Command::Rename {
            id,
            name: "P1".into(),
        };
        h.apply(&mut m, vec![p1()]).unwrap();
        assert_eq!(m.elements[&id].name, "P1");
        assert!(h.undo(&mut m));
        assert_eq!(m.elements[&id].name, old);
        // As imported from IFC without a name: renaming works, undo and redo work both ways.
        m.elements.get_mut(&id).unwrap().name = String::new();
        h.apply(&mut m, vec![p1()]).unwrap();
        assert_eq!(m.elements[&id].name, "P1");
        assert!(h.undo(&mut m));
        assert_eq!(m.elements[&id].name, "");
        assert!(h.redo(&mut m));
        assert_eq!(m.elements[&id].name, "P1");
    }

    /// Two materials of one name would be one in every list (materials page, schedules): renaming
    /// into a taken name is refused; values out of range are refused when adding too.
    #[test]
    fn material_names_stay_unique_and_values_valid() {
        let mut m = crate::demo::concrete_frame(&crate::demo::FrameSpec::default());
        let taken = m.materials.values().next().unwrap().name.clone();
        let s = crate::library::to_material(&crate::library::lookup("S235").unwrap());
        Command::AddMaterial(s.clone()).apply(&mut m).unwrap();
        let mut x = s.clone();
        x.name = format!(" {} ", taken.to_lowercase());
        assert!(Command::SetMaterial(x).apply(&mut m).is_err());
        let mut bad = crate::library::to_material(&crate::library::lookup("S355").unwrap());
        bad.props.density_kg_m3 = Some(-1.0);
        assert!(Command::AddMaterial(bad).apply(&mut m).is_err());
        assert!(m.validate().is_ok());
    }

    /// An element's material: apply and undo on two elements, ids unchanged, the command's JSON
    /// round trips canonically.
    #[test]
    fn set_element_material_applies_and_undoes() {
        let mut m = concrete_frame(&FrameSpec::default());
        let s = crate::library::to_material(&crate::library::lookup("S235").unwrap());
        let sid = s.id;
        Command::AddMaterial(s).apply(&mut m).unwrap();
        let ids: Vec<GlobalId> = m.elements.keys().take(2).copied().collect();
        let before = m.clone();
        let cmds: Vec<Command> = ids
            .iter()
            .map(|&id| Command::SetElementMaterial { id, material: sid })
            .collect();
        let json = serde_json::to_string(&cmds[0]).unwrap();
        assert_eq!(
            json,
            format!(
                r#"{{"op":"set_element_material","id":"{}","material":"{sid}"}}"#,
                ids[0]
            )
        );
        let back: Command = serde_json::from_str(&json).unwrap();
        assert_eq!(back, cmds[0]);
        assert_eq!(serde_json::to_string(&canonical(back)).unwrap(), json);
        let mut h = History::default();
        h.apply(&mut m, cmds).unwrap();
        for id in &ids {
            assert_eq!(m.elements[id].material, sid);
            assert_eq!(m.elements[id].id, *id);
        }
        assert!(h.undo(&mut m));
        assert_eq!(m, before);
        // An unknown material is refused, nothing changes.
        let bad = Command::SetElementMaterial {
            id: ids[0],
            material: GlobalId::new(),
        };
        assert!(bad.apply(&mut m).is_err());
        assert_eq!(m, before);
    }

    /// A property of an element (ADR 0064): set, change and remove, each undone to the bytes
    /// before; the JSON is canonical; bad text is refused and nothing changes.
    #[test]
    fn set_property_applies_and_undoes() {
        let mut m = concrete_frame(&FrameSpec::default());
        let id = *m.elements.keys().next().unwrap();
        let before = m.clone();
        let set = |v: Option<&str>| Command::SetProperty {
            id,
            key: "Note".into(),
            value: v.map(str::to_owned),
        };
        let json = serde_json::to_string(&set(Some("check cover"))).unwrap();
        assert_eq!(
            json,
            format!(r#"{{"op":"set_property","id":"{id}","key":"Note","value":"check cover"}}"#)
        );
        let back: Command = serde_json::from_str(&json).unwrap();
        assert_eq!(serde_json::to_string(&canonical(back)).unwrap(), json);
        assert_eq!(
            serde_json::to_string(&set(None)).unwrap(),
            format!(r#"{{"op":"set_property","id":"{id}","key":"Note"}}"#)
        );
        let mut h = History::default();
        h.apply(&mut m, vec![set(Some("check cover"))]).unwrap();
        assert_eq!(m.elements[&id].properties["Note"], "check cover");
        h.apply(&mut m, vec![set(Some("cover 35 mm"))]).unwrap();
        assert_eq!(m.elements[&id].properties["Note"], "cover 35 mm");
        h.apply(&mut m, vec![set(None)]).unwrap();
        assert!(!m.elements[&id].properties.contains_key("Note"));
        assert_eq!(
            m, before,
            "removing the only property leaves the element as it was"
        );
        assert!(h.undo(&mut m));
        assert_eq!(m.elements[&id].properties["Note"], "cover 35 mm");
        assert!(h.undo(&mut m) && h.undo(&mut m));
        assert_eq!(m, before);
        // Refused, and the model is unchanged.
        let long = "x".repeat(PROPERTY_VALUE_MAX + 1);
        for (key, value) in [
            ("Note", Some("")),
            ("Note", Some(" padded")),
            ("Note", Some("two\nlines")),
            ("Note", Some(long.as_str())),
            ("", Some("x")),
        ] {
            let c = Command::SetProperty {
                id,
                key: key.into(),
                value: value.map(str::to_owned),
            };
            assert!(c.apply(&mut m).is_err(), "{key:?} = {value:?}");
            assert_eq!(m, before);
        }
        let gone = Command::SetProperty {
            id: GlobalId::new(),
            key: "Note".into(),
            value: None,
        };
        assert!(gone.apply(&mut m).is_err());
    }
}
