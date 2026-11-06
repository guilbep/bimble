//! Editing a .bimble model in the viewer: properties, move, copy, delete, placing new columns,
//! beams, walls and slabs, undo/redo, save and history (commit, branches).
//!
//! Every change is a group of `Command`s applied through `History`, so it can be undone and is
//! validated like any other mutation. The functions that build commands are plain functions of
//! the model (tested below); the panels only collect input.

use bimble_core::{
    Command, Element, Generator, GlobalId, History, IfcClass, Material, MaterialCategory, Model,
    Placement,
};
use eframe::egui;

/// What to place with clicks in the view.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// One click: its centre.
    Column,
    /// Two clicks: start and end of its axis.
    Beam,
    Wall,
    /// Two clicks: opposite corners of a rectangle.
    Slab,
}

impl Kind {
    pub const ALL: [Kind; 4] = [Kind::Column, Kind::Beam, Kind::Wall, Kind::Slab];

    pub fn label(self) -> &'static str {
        match self {
            Kind::Column => "Column",
            Kind::Beam => "Beam",
            Kind::Wall => "Wall",
            Kind::Slab => "Slab",
        }
    }

    pub fn clicks(self) -> usize {
        if self == Kind::Column { 1 } else { 2 }
    }

    fn class(self) -> IfcClass {
        match self {
            Kind::Column => IfcClass::IfcColumn,
            Kind::Beam => IfcClass::IfcBeam,
            Kind::Wall => IfcClass::IfcWall,
            Kind::Slab => IfcClass::IfcSlab,
        }
    }

    /// Name prefix of new elements (as in the demo frame: P columns, B beams, M walls, D slabs).
    fn prefix(self) -> &'static str {
        match self {
            Kind::Column => "P",
            Kind::Beam => "B",
            Kind::Wall => "M",
            Kind::Slab => "D",
        }
    }
}

/// Sizes of new elements, mm.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sizes {
    pub column: f64,
    pub beam_width: f64,
    pub beam_height: f64,
    pub wall: f64,
    pub slab: f64,
}

impl Default for Sizes {
    fn default() -> Self {
        Sizes {
            column: 300.0,
            beam_width: 250.0,
            beam_height: 500.0,
            wall: 200.0,
            slab: 250.0,
        }
    }
}

/// Height of a storey: up to the next one above it, else 3 m.
pub fn storey_height(m: &Model, storey: GlobalId) -> f64 {
    let Some(z) = m.storeys.get(&storey).map(|s| s.elevation) else {
        return 3000.0;
    };
    m.storeys
        .values()
        .map(|s| s.elevation - z)
        .filter(|&d| d > 1.0)
        .fold(None, |a: Option<f64>, d| Some(a.map_or(d, |a| a.min(d))))
        .unwrap_or(3000.0)
}

/// A concrete material of the model, or a command adding one.
fn concrete(m: &Model) -> (GlobalId, Option<Command>) {
    match m
        .materials
        .values()
        .find(|x| x.category == MaterialCategory::Concrete)
    {
        Some(x) => (x.id, None),
        None => {
            let id = GlobalId::new();
            (
                id,
                Some(Command::AddMaterial(Material {
                    id,
                    name: "C30/37".into(),
                    category: MaterialCategory::Concrete,
                    props: bimble_core::library::lookup("C30/37")
                        .map(|m| m.props)
                        .unwrap_or_default(),
                })),
            )
        }
    }
}

/// The next free name with a prefix: P1, P2, …
fn next_name(m: &Model, prefix: &str) -> String {
    let used: std::collections::BTreeSet<&str> =
        m.elements.values().map(|e| e.name.as_str()).collect();
    (1..)
        .map(|n| format!("{prefix}{n}"))
        .find(|n| !used.contains(n.as_str()))
        .expect("an unused name")
}

/// Commands placing a new element on `storey` from clicked points (storey-relative mm: x, y;
/// z is ignored). Columns and walls stand on the storey level and are as high as the storey;
/// beams and slabs hang from the level above (top flush with it), like the demo frame.
pub fn new_element(
    m: &Model,
    kind: Kind,
    storey: GlobalId,
    a: [f64; 2],
    b: [f64; 2],
    sizes: &Sizes,
) -> Result<Vec<Command>, String> {
    if !m.storeys.contains_key(&storey) {
        return Err("choose a storey".into());
    }
    let h = storey_height(m, storey);
    let (mat, add_mat) = concrete(m);
    let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
    let len = dx.hypot(dy);
    if kind.clicks() == 2 && len < 10.0 {
        return Err("the two points are too close".into());
    }
    let dir = [dx / len, dy / len, 0.0];
    let (placement, generator) = match kind {
        Kind::Column => (
            Placement::at(a[0], a[1], 0.0),
            Generator::RectColumn {
                width: sizes.column,
                depth: sizes.column,
                height: h,
            },
        ),
        Kind::Beam => (
            Placement {
                origin: [a[0], a[1], h - sizes.beam_height / 2.0],
                z_axis: dir,
                x_axis: [-dir[1], dir[0], 0.0],
            },
            Generator::RectBeam {
                width: sizes.beam_width,
                height: sizes.beam_height,
                length: len,
            },
        ),
        Kind::Wall => (
            Placement {
                origin: [a[0], a[1], 0.0],
                z_axis: [0.0, 0.0, 1.0],
                x_axis: dir,
            },
            Generator::Wall {
                length: len,
                thickness: sizes.wall,
                height: h,
            },
        ),
        Kind::Slab => {
            let (x0, x1) = (a[0].min(b[0]), a[0].max(b[0]));
            let (y0, y1) = (a[1].min(b[1]), a[1].max(b[1]));
            if x1 - x0 < 10.0 || y1 - y0 < 10.0 {
                return Err("the slab rectangle is too thin".into());
            }
            (
                Placement::at(x0, y0, h - sizes.slab),
                Generator::Slab {
                    outline: vec![
                        [0.0, 0.0],
                        [x1 - x0, 0.0],
                        [x1 - x0, y1 - y0],
                        [0.0, y1 - y0],
                    ],
                    thickness: sizes.slab,
                },
            )
        }
    };
    let e = Element {
        id: GlobalId::new(),
        class: kind.class(),
        name: next_name(m, kind.prefix()),
        storey,
        material: mat,
        placement,
        generator,
    };
    Ok(add_mat
        .into_iter()
        .chain([Command::AddElement(e)])
        .collect())
}

/// Move elements by `d` (mm).
pub fn move_by(m: &Model, ids: &[GlobalId], d: [f64; 3]) -> Vec<Command> {
    ids.iter()
        .filter_map(|id| m.elements.get(id))
        .map(|e| {
            let mut p = e.placement.clone();
            for (o, d) in p.origin.iter_mut().zip(d) {
                *o += d;
            }
            Command::SetPlacement {
                id: e.id,
                placement: p,
            }
        })
        .collect()
}

/// Copies of elements moved by `d`, with new GlobalIds and names; returns the new ids too.
pub fn copy_by(m: &Model, ids: &[GlobalId], d: [f64; 3]) -> (Vec<Command>, Vec<GlobalId>) {
    let mut cmds = Vec::new();
    let mut new_ids = Vec::new();
    let mut names: std::collections::BTreeSet<String> =
        m.elements.values().map(|e| e.name.clone()).collect();
    for e in ids.iter().filter_map(|id| m.elements.get(id)) {
        let mut c = e.clone();
        c.id = GlobalId::new();
        // "B1" → "B1 (2)", "B1 (3)", …
        c.name = (2..)
            .map(|n| format!("{} ({n})", e.name))
            .find(|n| !names.contains(n))
            .expect("an unused name");
        names.insert(c.name.clone());
        for (o, d) in c.placement.origin.iter_mut().zip(d) {
            *o += d;
        }
        new_ids.push(c.id);
        cmds.push(Command::AddElement(c));
    }
    (cmds, new_ids)
}

pub fn delete(ids: &[GlobalId]) -> Vec<Command> {
    ids.iter()
        .map(|&id| Command::RemoveElement { id })
        .collect()
}

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
        }
    }

    /// Apply a group of commands as one undoable step.
    pub fn apply(&mut self, cmds: Vec<Command>) -> Result<(), String> {
        if cmds.is_empty() {
            return Ok(());
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
        ))
    }

    /// The properties form of the selected element; queues commands when a field is left.
    pub fn element_panel(&mut self, ui: &mut egui::Ui, id: GlobalId) {
        let Some(e) = self.model.elements.get(&id) else {
            return;
        };
        if self.form.as_ref().is_none_or(|f| f.id != id) {
            self.form = Some(Form::of(e));
        }
        let class = e.class.name();
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
            for k in 0..3 {
                ui.add(
                    egui::DragValue::new(&mut self.offset[k])
                        .speed(10.0)
                        .range(-1e6..=1e6),
                );
            }
            ui.weak("mm");
        });
        ui.horizontal(|ui| {
            if ui.button("Move").clicked() {
                self.pending = Some(move_by(&self.model, &[id], self.offset));
            }
            if ui.button("Copy").clicked() {
                let (c, ids) = copy_by(&self.model, &[id], self.offset);
                self.pending = Some(c);
                self.select_after = ids.first().copied();
            }
            if ui.button("Delete").on_hover_text("Delete key").clicked() {
                self.pending = Some(delete(&[id]));
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
    use bimble_core::demo::{FrameSpec, concrete_frame};

    fn demo() -> (Model, GlobalId) {
        let m = concrete_frame(&FrameSpec::default());
        let s = m.storeys.values().find(|s| s.elevation == 0.0).unwrap().id;
        (m, s)
    }

    #[test]
    fn placed_elements_are_valid_and_sized_from_the_storey() {
        let (mut m, s) = demo();
        let sizes = Sizes::default();
        let h = storey_height(&m, s);
        assert_eq!(h, 3000.0);
        for kind in Kind::ALL {
            let cmds = new_element(&m, kind, s, [500.0, 500.0], [4500.0, 3500.0], &sizes).unwrap();
            let mut hist = History::default();
            hist.apply(&mut m, cmds).unwrap();
        }
        let added: Vec<&Element> = m
            .elements
            .values()
            .filter(|e| {
                ["P", "B", "M", "D"]
                    .iter()
                    .any(|p| e.name == format!("{p}1"))
            })
            .collect();
        assert_eq!(
            added.len(),
            4,
            "{:?}",
            added.iter().map(|e| &e.name).collect::<Vec<_>>()
        );
        // The beam is 5 m long (3-4-5), its top flush with the next level.
        let beam = added.iter().find(|e| e.class == IfcClass::IfcBeam).unwrap();
        let Generator::RectBeam { length, height, .. } = beam.generator else {
            panic!()
        };
        assert!((length - 5000.0).abs() < 1e-9);
        assert_eq!(beam.placement.origin[2] + height / 2.0, h);
        // The meshes are where the clicks were: the column's centre at (500, 500).
        let scene = bimble_geom::Scene::from_model(&m);
        let col = scene.elements.iter().find(|e| e.name == "P1").unwrap();
        let (mut lo, mut hi) = ([f64::MAX; 3], [f64::MIN; 3]);
        for p in &col.mesh.positions {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k] as f64 + scene.origin_mm[k]);
                hi[k] = hi[k].max(p[k] as f64 + scene.origin_mm[k]);
            }
        }
        assert!(((lo[0] + hi[0]) / 2.0 - 500.0).abs() < 0.01);
        assert!((hi[2] - lo[2] - 3000.0).abs() < 0.01);
    }

    #[test]
    fn points_too_close_are_refused() {
        let (m, s) = demo();
        assert!(new_element(&m, Kind::Beam, s, [0.0, 0.0], [1.0, 0.0], &Sizes::default()).is_err());
        assert!(
            new_element(
                &m,
                Kind::Slab,
                s,
                [0.0, 0.0],
                [5000.0, 2.0],
                &Sizes::default()
            )
            .is_err()
        );
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
