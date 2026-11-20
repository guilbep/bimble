//! Editing a .bimble model in the viewer: properties, move, copy, delete, placing new columns,
//! beams, walls and slabs, undo/redo, save and history (commit, branches).
//!
//! Every change is a group of `Command`s applied through `History`, so it can be undone and is
//! validated like any other mutation. The functions that build commands are plain functions of
//! the model (tested below); the panels only collect input.

pub use bimble_api::verbs::{Kind, Sizes, copy_by, delete, move_by, new_element};
use bimble_core::{Command, Element, Generator, GlobalId, History, Model};
use eframe::egui;

/// Editable copy of one element's properties, applied when a field is left.
#[derive(Clone, Debug, PartialEq)]
pub struct Form {
    pub id: GlobalId,
    pub name: String,
    /// Numeric generator parameters (key, value), in the generator's field order.
    pub params: Vec<(String, f64)>,
    pub origin: [f64; 3],
}

impl Form {
    pub fn of(e: &Element) -> Form {
        let g = serde_json::to_value(&e.generator).expect("generators serialize");
        let params = g
            .as_object()
            .map(|o| {
                o.iter()
                    .filter_map(|(k, v)| v.as_f64().map(|x| (k.clone(), x)))
                    .collect()
            })
            .unwrap_or_default();
        Form {
            id: e.id,
            name: e.name.clone(),
            params,
            origin: e.placement.origin,
        }
    }

    /// Commands turning the element into what the form says (none when nothing changed).
    pub fn commands(&self, m: &Model) -> Result<Vec<Command>, String> {
        let e = m.elements.get(&self.id).ok_or("the element is gone")?;
        let mut out = Vec::new();
        if self.name != e.name {
            out.push(Command::Rename {
                id: e.id,
                name: self.name.trim().to_owned(),
            });
        }
        let mut g = serde_json::to_value(&e.generator).expect("generators serialize");
        for (k, v) in &self.params {
            g[k.as_str()] = (*v).into();
        }
        let generator: Generator = serde_json::from_value(g).map_err(|x| x.to_string())?;
        if generator != e.generator {
            generator.validate()?;
            out.push(Command::SetGenerator {
                id: e.id,
                generator,
            });
        }
        if self.origin != e.placement.origin {
            let mut p = e.placement.clone();
            p.origin = self.origin;
            out.push(Command::SetPlacement {
                id: e.id,
                placement: p,
            });
        }
        Ok(out)
    }
}

/// Clicks collected for the element being placed.
#[derive(Clone, Debug, PartialEq)]
pub struct Placing {
    pub kind: Kind,
    /// Storey-relative points clicked so far.
    pub points: Vec<[f64; 2]>,
}

pub struct Editor {
    pub model: Model,
    pub history: History,
    /// Changed since loaded or saved.
    pub dirty: bool,
    pub form: Option<Form>,
    /// Move/copy vector, mm.
    pub offset: [f64; 3],
    pub kind: Kind,
    pub storey: Option<GlobalId>,
    pub sizes: Sizes,
    pub placing: Option<Placing>,
    pub commit_message: String,
    pub new_branch: String,
    pub message: Option<String>,
    /// Commands from the panel, applied by the viewer after drawing it.
    pub pending: Option<Vec<Command>>,
    /// Elements to select after the pending commands (copies).
    pub select_after: Option<GlobalId>,
    /// Working with others through a shared folder (ADR 0038): elements are borrowed there on
    /// their first change.
    pub borrowing: Option<Borrowing>,
    /// A change refused: these elements belong to someone else (Synchronize or Cancel).
    pub belongs: Option<Vec<String>>,
}

/// Where and as whom elements are borrowed, and those already borrowed this time.
#[derive(Clone, Debug)]
pub struct Borrowing {
    pub remote: std::path::PathBuf,
    pub who: String,
    pub mine: std::collections::BTreeSet<GlobalId>,
}

impl Borrowing {
    /// Borrow what isn't borrowed yet. Elements someone else has: "<class> <name> belongs to
    /// <who>". Can't reach the folder: an error (the edit goes on; Synchronize checks again).
    fn take(&mut self, model: &Model, ids: &[GlobalId]) -> Result<Vec<String>, String> {
        let new: Vec<GlobalId> = ids
            .iter()
            .filter(|id| !self.mine.contains(id))
            .copied()
            .collect::<std::collections::BTreeSet<_>>()
            .into_iter()
            .collect();
        if new.is_empty() {
            return Ok(Vec::new());
        }
        // Open, never create: a folder not mounted must not become a new, empty shared history.
        let remote = bimble_core::repo::Repo::open_dir(&self.remote).map_err(|e| e.to_string())?;
        let taken =
            bimble_core::remote::borrow(&remote, &self.who, &new, bimble_core::date::now_secs())
                .map_err(|e| e.to_string())?;
        if taken.is_empty() {
            self.mine.extend(new);
        }
        Ok(taken
            .into_iter()
            .map(|(id, who)| {
                let what = model.elements.get(&id).map_or_else(
                    || "A new element".into(),
                    |e| format!("{} {}", e.class_name(), e.name),
                );
                format!("{what} belongs to {who}")
            })
            .collect())
    }
}

impl Editor {
    pub fn new(model: Model) -> Editor {
        let storey = model
            .storeys
            .values()
            .filter(|s| s.elevation >= -1.0)
            .min_by(|a, b| a.elevation.total_cmp(&b.elevation))
            .or_else(|| model.storeys.values().next())
            .map(|s| s.id);
        Editor {
            model,
            history: History::default(),
            dirty: false,
            form: None,
            offset: [1000.0, 0.0, 0.0],
            kind: Kind::Column,
            storey,
            sizes: Sizes::default(),
            placing: None,
            commit_message: String::new(),
            new_branch: String::new(),
            message: None,
            pending: None,
            select_after: None,
            borrowing: None,
            belongs: None,
        }
    }

    /// An IFC imported as an editable copy (ADR 0032): nothing on disk yet, so it starts dirty —
    /// Save is enabled and closing or opening another file asks first.
    pub fn imported(model: Model) -> Editor {
        Editor {
            dirty: true,
            ..Editor::new(model)
        }
    }

    /// Apply a group of commands as one undoable step.
    pub fn apply(&mut self, cmds: Vec<Command>) -> Result<(), String> {
        if cmds.is_empty() {
            return Ok(());
        }
        // Working together: borrow the elements first; someone else's stop the change.
        let ids: Vec<GlobalId> = cmds.iter().filter_map(Command::element).collect();
        if let Some(b) = &mut self.borrowing
            && !ids.is_empty()
        {
            match b.take(&self.model, &ids) {
                Ok(taken) if !taken.is_empty() => {
                    let msg = taken.join("; ");
                    self.belongs = Some(taken);
                    return Err(msg);
                }
                Ok(_) => {}
                Err(e) => {
                    self.message = Some(format!(
                        "Not borrowed (the shared folder can't be reached: {e}); Synchronize \
                         will check"
                    ))
                }
            }
        }
        self.history
            .apply(&mut self.model, cmds)
            .map_err(|e| e.to_string())?;
        self.dirty = true;
        self.form = None;
        Ok(())
    }

    pub fn undo(&mut self) -> bool {
        let r = self.history.undo(&mut self.model);
        if r {
            self.dirty = true;
            self.form = None;
        }
        r
    }

    pub fn redo(&mut self) -> bool {
        let r = self.history.redo(&mut self.model);
        if r {
            self.dirty = true;
            self.form = None;
        }
        r
    }

    pub fn storey_elevation(&self) -> f64 {
        self.storey
            .and_then(|s| self.model.storeys.get(&s))
            .map_or(0.0, |s| s.elevation)
    }

    /// A click while placing (storey-relative x, y). Returns commands when the element is
    /// complete.
    pub fn click(&mut self, p: [f64; 2]) -> Option<Result<Vec<Command>, String>> {
        let placing = self.placing.as_mut()?;
        placing.points.push(p);
        if placing.points.len() < placing.kind.clicks() {
            return None;
        }
        let pts = std::mem::take(&mut placing.points);
        let b = *pts.last().unwrap_or(&pts[0]);
        let storey = self.storey?;
        Some(new_element(
            &self.model,
            placing.kind,
            storey,
            pts[0],
            b,
            &self.sizes,
            None,
        ))
    }

    /// The properties form of the selected element; queues commands when a field is left.
    /// The selected element's sizes and place; Move, Copy and Delete act on `group`, every
    /// selected element (#54: several selected with Ctrl/Cmd).
    pub fn element_panel(&mut self, ui: &mut egui::Ui, id: GlobalId, group: &[GlobalId]) {
        let Some(e) = self.model.elements.get(&id) else {
            return;
        };
        if self.form.as_ref().is_none_or(|f| f.id != id) {
            self.form = Some(Form::of(e));
        }
        let class = e.class_name();
        let storey = self
            .model
            .storeys
            .get(&e.storey)
            .map(|s| s.name.clone())
            .unwrap_or_default();
        let material = self
            .model
            .materials
            .get(&e.material)
            .map(|m| m.name.clone())
            .unwrap_or_default();
        let gen_name = e.generator.name();
        let Some(form) = self.form.as_mut() else {
            return;
        };
        let mut done = false;
        ui.heading(class);
        egui::Grid::new("edit-props")
            .num_columns(2)
            .striped(true)
            .show(ui, |ui| {
                ui.label("Name");
                let r = ui.text_edit_singleline(&mut form.name);
                done |= r.lost_focus();
                ui.end_row();
                ui.label("GlobalId");
                ui.add(egui::Label::new(id.as_str()).selectable(true));
                ui.end_row();
                ui.label("Storey");
                ui.label(storey);
                ui.end_row();
                ui.label("Material");
                ui.label(material);
                ui.end_row();
                ui.label("Generator");
                ui.label(gen_name);
                ui.end_row();
                for (k, v) in form.params.iter_mut() {
                    ui.label(k.as_str());
                    let r = ui.add(
                        egui::DragValue::new(v)
                            .speed(5.0)
                            .range(1.0..=1_000_000.0)
                            .suffix(" mm"),
                    );
                    done |= r.drag_stopped() || r.lost_focus();
                    ui.end_row();
                }
                for (k, axis) in ["X", "Y", "Z"].iter().enumerate() {
                    ui.label(format!("Origin {axis}"));
                    let r = ui.add(
                        egui::DragValue::new(&mut form.origin[k])
                            .speed(10.0)
                            .range(-1e7..=1e7)
                            .suffix(" mm"),
                    );
                    done |= r.drag_stopped() || r.lost_focus();
                    ui.end_row();
                }
            });
        if done {
            match form.commands(&self.model) {
                Ok(c) if !c.is_empty() => self.pending = Some(c),
                Ok(_) => {}
                Err(err) => {
                    self.message = Some(err);
                    self.form = None;
                }
            }
        }
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.label("Vector");
            for v in &mut self.offset {
                ui.add(egui::DragValue::new(v).speed(10.0).range(-1e6..=1e6));
            }
            ui.weak("mm");
        });
        let group: &[GlobalId] = if group.is_empty() { &[id] } else { group };
        let n = |verb: &str| match group.len() {
            1 => verb.to_owned(),
            k => format!("{verb} {k}"),
        };
        ui.horizontal(|ui| {
            if ui.button(n("Move")).clicked() {
                self.pending = Some(move_by(&self.model, group, self.offset));
            }
            if ui.button(n("Copy")).clicked() {
                let (c, ids) = copy_by(&self.model, group, self.offset);
                self.pending = Some(c);
                self.select_after = ids.first().copied();
            }
            if ui.button(n("Delete")).on_hover_text("Delete key").clicked() {
                self.pending = Some(delete(group));
            }
        });
    }

    /// Placing new elements: kind, storey, sizes.
    pub fn place_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            for k in Kind::ALL {
                let on = self.placing.as_ref().is_some_and(|p| p.kind == k);
                if ui.selectable_label(on, k.label()).clicked() {
                    self.kind = k;
                    self.placing = if on {
                        None
                    } else {
                        Some(Placing {
                            kind: k,
                            points: Vec::new(),
                        })
                    };
                }
            }
        });
        let mut storeys: Vec<_> = self.model.storeys.values().collect();
        storeys.sort_by(|a, b| b.elevation.total_cmp(&a.elevation));
        let current = self
            .storey
            .and_then(|s| self.model.storeys.get(&s))
            .map(|s| s.name.clone())
            .unwrap_or_default();
        egui::ComboBox::from_label("Storey")
            .selected_text(current)
            .show_ui(ui, |ui| {
                for s in storeys {
                    ui.selectable_value(
                        &mut self.storey,
                        Some(s.id),
                        format!("{}  ({:+.3} m)", s.name, s.elevation / 1000.0),
                    );
                }
            });
        fn mm(v: &mut f64) -> egui::DragValue<'_> {
            egui::DragValue::new(v).speed(5.0).range(10.0..=100_000.0)
        }
        egui::Grid::new("place-sizes")
            .num_columns(2)
            .show(ui, |ui| {
                let s = &mut self.sizes;
                match self.kind {
                    Kind::Column => {
                        ui.label("Section");
                        ui.add(mm(&mut s.column));
                    }
                    Kind::Beam => {
                        ui.label("Width");
                        ui.add(mm(&mut s.beam_width));
                        ui.end_row();
                        ui.label("Height");
                        ui.add(mm(&mut s.beam_height));
                    }
                    Kind::Wall => {
                        ui.label("Thickness");
                        ui.add(mm(&mut s.wall));
                    }
                    Kind::Slab => {
                        ui.label("Thickness");
                        ui.add(mm(&mut s.slab));
                    }
                }
                ui.end_row();
            });
        if let Some(p) = &self.placing {
            ui.weak(match (p.kind.clicks(), p.points.len()) {
                (1, _) => "Click where the column goes (on the storey level). Esc stops.",
                (_, 0) if p.kind == Kind::Slab => "Click a corner of the slab.",
                (_, 0) => "Click the start point.",
                _ if p.kind == Kind::Slab => "Click the opposite corner.",
                _ => "Click the end point.",
            });
            ui.weak(
                "Snaps to axis crossings, column centres, corners, axes and edges. Shift: free.",
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::IfcClass;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn demo() -> (Model, GlobalId) {
        let m = concrete_frame(&FrameSpec::default());
        let s = m.storeys.values().find(|s| s.elevation == 0.0).unwrap().id;
        (m, s)
    }

    /// Several selected (#54): one Delete removes them all, and one undo brings them back.
    #[test]
    fn several_elements_are_deleted_and_restored_together() {
        let m = concrete_frame(&FrameSpec::default());
        let ids: Vec<GlobalId> = m.elements.keys().copied().take(3).collect();
        let mut ed = Editor::new(m.clone());
        ed.apply(delete(&ids)).unwrap();
        assert!(ids.iter().all(|id| !ed.model.elements.contains_key(id)));
        assert_eq!(ed.model.elements.len(), m.elements.len() - 3);
        assert!(ed.undo());
        assert_eq!(ed.model.elements, m.elements);
    }

    /// Working together (ADR 0038, #51): the first change borrows the element on the shared
    /// folder; someone else's change to it is refused, saying whose it is; others go on.
    #[test]
    fn a_first_change_borrows_and_others_are_refused() {
        let dir = std::env::temp_dir().join(format!("bimble-borrow-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let m = concrete_frame(&FrameSpec::default());
        // A shared folder that isn't there (not mounted): the change goes on, nothing borrowed,
        // and no history is created in its place.
        let mut away = Editor::new(m.clone());
        away.borrowing = Some(Borrowing {
            remote: dir.clone(),
            who: "Ana".into(),
            mine: Default::default(),
        });
        let first = *m.elements.keys().next().unwrap();
        away.apply(move_by(&away.model, &[first], [0.0, 0.0, 10.0]))
            .unwrap();
        assert!(
            away.message
                .as_deref()
                .is_some_and(|t| t.starts_with("Not borrowed"))
        );
        assert!(
            !dir.exists(),
            "no history created in a missing shared folder"
        );
        // Shared folder… creates it.
        bimble_core::remote::open_or_init(&dir).unwrap();
        let beams: Vec<GlobalId> = m
            .elements
            .values()
            .filter(|e| e.class == IfcClass::IfcBeam)
            .map(|e| e.id)
            .take(2)
            .collect();
        let with = |who: &str| {
            let mut ed = Editor::new(m.clone());
            ed.borrowing = Some(Borrowing {
                remote: dir.clone(),
                who: who.into(),
                mine: Default::default(),
            });
            ed
        };
        let (mut ana, mut ben) = (with("Ana"), with("Ben"));
        ana.apply(move_by(&ana.model, &beams[..1], [0.0, 0.0, 100.0]))
            .unwrap();
        let refused = ben.apply(move_by(&ben.model, &beams[..1], [0.0, 0.0, 50.0]));
        assert!(refused.unwrap_err().ends_with("belongs to Ana"));
        assert_eq!(ben.belongs.as_ref().map(Vec::len), Some(1));
        assert!(!ben.dirty, "nothing changed");
        // Another beam is free.
        ben.apply(move_by(&ben.model, &beams[1..2], [0.0, 0.0, 50.0]))
            .unwrap();
        assert!(ben.borrowing.as_ref().unwrap().mine.contains(&beams[1]));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// An imported IFC exists only in memory until saved: Save is enabled and closing asks.
    #[test]
    fn imported_model_starts_unsaved() {
        let (m, _) = demo();
        assert!(!Editor::new(m.clone()).dirty);
        assert!(Editor::imported(m).dirty);
    }

    #[test]
    fn move_copy_delete_and_undo() {
        let (mut m, _) = demo();
        let id = *m.elements.keys().next().unwrap();
        let before = m.clone();
        let mut ed = Editor::new(m.clone());
        ed.apply(move_by(&ed.model, &[id], [100.0, 0.0, -50.0]))
            .unwrap();
        assert_eq!(
            ed.model.elements[&id].placement.origin[0],
            before.elements[&id].placement.origin[0] + 100.0
        );
        let (c, ids) = copy_by(&ed.model, &[id], [0.0, 1000.0, 0.0]);
        ed.apply(c).unwrap();
        let copy = &ed.model.elements[&ids[0]];
        assert_ne!(copy.id, id);
        assert!(copy.name.ends_with("(2)"), "{}", copy.name);
        ed.apply(delete(&[id])).unwrap();
        assert!(!ed.model.elements.contains_key(&id));
        assert!(ed.undo() && ed.undo() && ed.undo());
        assert_eq!(ed.model, before);
        assert!(ed.redo());
        m = ed.model.clone();
        assert_ne!(m, before);
    }

    #[test]
    fn the_form_turns_edits_into_commands() {
        let (m, _) = demo();
        let beam = m
            .elements
            .values()
            .find(|e| e.class == IfcClass::IfcBeam)
            .unwrap();
        let mut f = Form::of(beam);
        assert!(f.commands(&m).unwrap().is_empty());
        f.name = "B-edited".into();
        f.params.iter_mut().find(|(k, _)| k == "height").unwrap().1 = 650.0;
        f.origin[2] += 10.0;
        let cmds = f.commands(&m).unwrap();
        assert_eq!(cmds.len(), 3, "{cmds:?}");
        // Invalid sizes are refused before anything is applied.
        f.params.iter_mut().find(|(k, _)| k == "height").unwrap().1 = -1.0;
        assert!(f.commands(&m).is_err());
    }
}
