//! Views in the viewer (ADR 0021): the model navigator, the view's properties (view range,
//! visibility, reference plans), switching between 3D and 2D views, and reference plans drawn,
//! moved and snapped to in 2D views.

use super::*;
use bimble_core::GlobalId;
use bimble_core::view::{
    Facing, Level, LevelRef, Look, Reference, View, ViewKind, ViewsFile, Visibility, default_views,
    effective, side_file,
};
use glam::Vec3;

/// The views of a model that isn't edited (IFC): kept in a side file next to it.
pub(crate) struct SideViews {
    pub stored: BTreeMap<GlobalId, View>,
    /// Sheets of the side file (#33), kept and saved back.
    pub sheets: BTreeMap<GlobalId, bimble_core::sheet::Sheet>,
    pub dirty: bool,
}

/// Which views a navigator group lists.
type ViewFilter = fn(&View) -> bool;

/// How a reference's placement changed in the form this frame.
#[derive(Default)]
struct RefEdits {
    commit: bool,
    remove: Option<usize>,
}

impl Viewer {
    pub(crate) fn levels(&self) -> Vec<Level> {
        self.loaded
            .as_ref()
            .map(|l| l.scene.levels())
            .unwrap_or_default()
    }

    /// Views stored in the model (.bimble) or the side file (IFC).
    pub(crate) fn stored_views(&self) -> BTreeMap<GlobalId, View> {
        let Some(l) = &self.loaded else {
            return BTreeMap::new();
        };
        match (&l.edit, &self.side_views) {
            (Some(ed), _) => ed.model.views.clone(),
            (None, Some(s)) => s.stored.clone(),
            (None, None) => BTreeMap::new(),
        }
    }

    pub(crate) fn refresh_views(&mut self) {
        self.views = effective(&self.stored_views(), &self.levels());
        if self
            .active_view
            .is_none_or(|id| !self.views.iter().any(|v| v.id == id))
        {
            self.active_view = self.views.first().map(|v| v.id);
        }
        // The form is made again from the stored view (it may have changed: a new reference,
        // undo, another storey).
        self.view_form = None;
        if let Some(vis) = self.active_view().map(|v| v.visibility.clone()) {
            self.apply_visibility(&vis);
        }
    }

    pub(crate) fn active_view(&self) -> Option<&View> {
        let id = self.active_view?;
        self.views.iter().find(|v| v.id == id)
    }

    pub(crate) fn in_2d(&self) -> bool {
        self.active_view().is_some_and(View::is_2d)
    }

    /// What the active 2D view shows (scene coordinates); None in 3D.
    pub(crate) fn view_clip(&self) -> Option<ClipBox> {
        let v = self.active_view()?;
        let l = self.loaded.as_ref()?;
        views::view_clip(v, &l.scene, &l.scene.levels())
            .ok()
            .flatten()
    }

    /// After a model is loaded: its views (stored ones, the side file's for IFC), 3D first.
    pub(crate) fn setup_views_after_load(&mut self) {
        self.side_views = None;
        self.view_cameras.clear();
        self.view_form = None;
        self.moving_ref = None;
        self.view_message = None;
        self.view_note = None;
        let Some(l) = &self.loaded else { return };
        if l.edit.is_none() {
            // In a browser there is no file next to the model to read.
            #[cfg(target_arch = "wasm32")]
            let (stored, sheets) = (BTreeMap::new(), BTreeMap::new());
            #[cfg(not(target_arch = "wasm32"))]
            let (stored, sheets) = {
                let p = side_file(&l.path);
                match p.exists().then(|| {
                    std::fs::read(&p)
                        .map_err(|e| e.to_string())
                        .and_then(|b| ViewsFile::from_bytes(&b))
                }) {
                    Some(Ok(f)) => (f.views, f.sheets),
                    Some(Err(e)) => {
                        self.view_message = Some(format!("{}: {e}", p.display()));
                        (BTreeMap::new(), BTreeMap::new())
                    }
                    None => (BTreeMap::new(), BTreeMap::new()),
                }
            };
            self.side_views = Some(SideViews {
                stored,
                sheets,
                dirty: false,
            });
        }
        self.active_view = None;
        self.refresh_views();
    }

    /// Switch to a view: its camera (remembered per view), clip and visibility.
    pub(crate) fn activate_view(&mut self, id: GlobalId) {
        if self.active_view == Some(id) {
            return;
        }
        if let Some(cur) = self.active_view {
            self.view_cameras.insert(cur, self.camera);
        }
        let Some(v) = self.views.iter().find(|v| v.id == id).cloned() else {
            return;
        };
        self.active_view = Some(id);
        self.view_form = None;
        self.view_message = None;
        self.view_note = None;
        self.moving_ref = None;
        self.turn = None;
        self.measure_first = None;
        self.hover_snap = None;
        self.apply_visibility(&v.visibility);
        match self.view_cameras.get(&id) {
            Some(c) => self.camera = *c,
            None => {
                if let Some((yaw, pitch, mirror)) = views::camera_angles(&v) {
                    self.camera.yaw = yaw;
                    self.camera.pitch = pitch;
                    self.camera.mirror_x = mirror;
                    self.camera.orthographic = true;
                } else {
                    self.camera = Camera::default();
                }
                self.fit_pending = true;
            }
        }
        // Placing elements on a plan goes on its storey.
        if let ViewKind::Plan { storey, .. } = &v.kind
            && let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut())
            && let Ok(sid) = storey.parse::<GlobalId>()
            && ed.model.storeys.contains_key(&sid)
        {
            ed.storey = Some(sid);
        }
        // The plan window draws the view shown, if it is 2D.
        if v.is_2d() {
            self.plan.view_id = Some(v.id);
        }
        self.needs_upload = true;
        self.needs_render = true;
    }

    fn apply_visibility(&mut self, v: &Visibility) {
        self.hidden_categories = MaterialCategory::ALL
            .into_iter()
            .filter(|c| v.hidden_families.iter().any(|f| f == c.label()))
            .collect();
        self.hidden_classes = v.hidden_classes.iter().cloned().collect();
        self.hidden_storeys = v.hidden_storeys.iter().cloned().collect();
        if self.selected.is_some_and(|i| !self.visible(i)) {
            self.selected = None;
        }
    }

    fn current_visibility(&self) -> Visibility {
        Visibility {
            hidden_families: self
                .hidden_categories
                .iter()
                .map(|c| c.label().to_owned())
                .collect(),
            hidden_classes: self.hidden_classes.iter().cloned().collect(),
            hidden_storeys: self.hidden_storeys.iter().cloned().collect(),
        }
    }

    /// Store a view: a Command in a .bimble model (undoable), the side file for IFC.
    pub(crate) fn store_view(&mut self, v: View) -> Result<(), String> {
        let levels = self.levels();
        v.validate(&levels)?;
        let Some(l) = self.loaded.as_mut() else {
            return Ok(());
        };
        match (&mut l.edit, &mut self.side_views) {
            (Some(ed), _) => {
                let cmd = if ed.model.views.contains_key(&v.id) {
                    bimble_core::Command::SetView(v)
                } else {
                    bimble_core::Command::AddView(v)
                };
                ed.apply(vec![cmd])?;
            }
            (None, Some(s)) => {
                s.stored.insert(v.id, bimble_core::version::canonical(v));
                s.dirty = true;
            }
            (None, None) => return Err("no model".into()),
        }
        self.refresh_views();
        self.needs_upload = true;
        Ok(())
    }

    /// Remove a stored view; a changed default view goes back to its default.
    fn remove_view(&mut self, id: GlobalId) {
        let Some(l) = self.loaded.as_mut() else {
            return;
        };
        let r = match (&mut l.edit, &mut self.side_views) {
            (Some(ed), _) => ed.apply(vec![bimble_core::Command::RemoveView { id }]),
            (None, Some(s)) => {
                s.stored.remove(&id);
                s.dirty = true;
                Ok(())
            }
            (None, None) => Ok(()),
        };
        if let Err(e) = r {
            self.view_message = Some(e);
        }
        if self.active_view == Some(id) {
            self.view_cameras.remove(&id);
        }
        self.refresh_views();
        if let Some(v) = self.active_view().map(|v| v.visibility.clone()) {
            self.apply_visibility(&v);
        }
        self.needs_upload = true;
    }

    /// The model's sheets (#33), by number.
    pub(crate) fn sheets(&self) -> Vec<bimble_core::sheet::Sheet> {
        let mut v: Vec<_> = match (&self.loaded, &self.side_views) {
            (Some(l), _) if l.edit.is_some() => l
                .edit
                .as_ref()
                .map(|ed| ed.model.sheets.values().cloned().collect())
                .unwrap_or_default(),
            (_, Some(s)) => s.sheets.values().cloned().collect(),
            _ => Vec::new(),
        };
        v.sort_by(|a, b| a.number.cmp(&b.number));
        v
    }

    /// A sheet as PDF (saved, or downloaded in a browser).
    fn export_sheet(&mut self, sh: &bimble_core::sheet::Sheet) {
        let Some(l) = &self.loaded else { return };
        let o = bimble_drawing::sheetdraw::SheetOptions {
            style: self.opts.style,
            template: self.plan.template.clone(),
            project: l
                .path
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default(),
            date: bimble_drawing::today(),
        };
        self.view_note = None;
        self.view_message = None;
        match bimble_drawing::sheetdraw::draw(&l.scene, sh, &self.views, &o) {
            Err(e) => self.view_message = Some(e),
            Ok((sheet, info)) => {
                let name = format!("{} {}.pdf", sh.number, sh.name);
                match files::save(&name, &[("PDF", &["pdf"])], &sheet.to_pdf()) {
                    Ok(Some(to)) => {
                        self.view_note = Some(if info.problems.is_empty() {
                            format!("Saved {to}")
                        } else {
                            format!("Saved {to}; {}", info.problems.join("; "))
                        })
                    }
                    Ok(None) => {}
                    Err(e) => self.view_message = Some(e),
                }
            }
        }
    }

    /// Save the side file of an IFC model (a download in a browser).
    pub(crate) fn save_side_views(&mut self) {
        let Some(l) = &self.loaded else { return };
        let Some(s) = &mut self.side_views else {
            return;
        };
        let text = ViewsFile::new(s.stored.clone(), s.sheets.clone()).to_json();
        #[cfg(target_arch = "wasm32")]
        let r = {
            let name = side_file(Path::new(
                &l.path
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default(),
            ));
            files::download(&name.to_string_lossy(), text.as_bytes())
                .map(|()| format!("Downloaded {}", name.display()))
        };
        #[cfg(not(target_arch = "wasm32"))]
        let r = {
            let p = side_file(&l.path);
            bimble_core::file::write_atomic(&p, text.as_bytes())
                .map(|()| format!("Saved views to {}", p.display()))
                .map_err(|e| format!("{}: {e}", p.display()))
        };
        match r {
            Ok(m) => {
                s.dirty = false;
                self.view_note = Some(m);
            }
            Err(e) => self.view_message = Some(e),
        }
    }

    pub(crate) fn side_views_dirty(&self) -> bool {
        self.side_views.as_ref().is_some_and(|s| s.dirty)
    }

    // ---------------------------------------------------------------- navigator

    /// The model navigator: views, quantities, layouts, objects, links, measurements, warnings.
    pub(crate) fn navigator(&mut self, ui: &mut egui::Ui) {
        egui::ScrollArea::vertical()
            .id_salt("navigator")
            .auto_shrink([false, false])
            .show(ui, |ui| {
                self.navigator_tree(ui);
                self.side_extras(ui);
            });
    }

    /// Reference plans of all views, listed under Links (click: open the view).
    pub(crate) fn reference_links(&mut self, ui: &mut egui::Ui) {
        let mut activate = None;
        for v in &self.views {
            for r in &v.references {
                if ui
                    .selectable_label(false, format!("{} — in {}", r.name, v.name))
                    .on_hover_text("Reference plan: click to open its view")
                    .clicked()
                {
                    activate = Some(v.id);
                }
            }
        }
        if let Some(id) = activate {
            self.activate_view(id);
        }
    }

    fn navigator_tree(&mut self, ui: &mut egui::Ui) {
        let Some(l) = &self.loaded else {
            ui.weak("No model loaded.");
            return;
        };
        let levels = l.scene.levels();
        let defaults: Vec<GlobalId> = default_views(&levels).iter().map(|v| v.id).collect();
        let stored = self.stored_views();
        let mut activate = None;
        let mut new_view: Option<View> = None;
        let mut remove = None;
        let mut select_element = None;
        let mut open_plan = false;
        let mut export_sheet: Option<bimble_core::sheet::Sheet> = None;
        let sheets = self.sheets();
        {
            {
                egui::CollapsingHeader::new(egui::RichText::new("Views").strong())
                    .default_open(true)
                    .show(ui, |ui| {
                        let groups: [(&str, ViewFilter); 4] = [
                            ("3D views", |v| matches!(v.kind, ViewKind::ThreeD)),
                            ("Plans", |v| {
                                matches!(
                                    v.kind,
                                    ViewKind::Plan {
                                        look: Look::Down,
                                        ..
                                    }
                                )
                            }),
                            ("Ceiling plans (looking up)", |v| {
                                matches!(v.kind, ViewKind::Plan { look: Look::Up, .. })
                            }),
                            ("Sections", |v| matches!(v.kind, ViewKind::Section { .. })),
                        ];
                        for (title, keep) in groups {
                            let list: Vec<&View> = self.views.iter().filter(|v| keep(v)).collect();
                            egui::CollapsingHeader::new(format!("{title} ({})", list.len()))
                                .id_salt(title)
                                .default_open(title != "Ceiling plans (looking up)")
                                .show(ui, |ui| {
                                    for v in list {
                                        let on = self.active_view == Some(v.id);
                                        let mut name = v.name.clone();
                                        if !v.references.is_empty() {
                                            name.push_str(&format!(
                                                "  [{} ref.]",
                                                v.references.len()
                                            ));
                                        }
                                        let r = ui.selectable_label(on, name);
                                        if r.clicked() {
                                            activate = Some(v.id);
                                        }
                                        r.context_menu(|ui| {
                                            if ui.button("Duplicate").clicked() {
                                                let mut d = v.clone();
                                                d.id = GlobalId::new();
                                                d.name = format!("{} (copy)", v.name);
                                                new_view = Some(d);
                                                ui.close();
                                            }
                                            let is_default = defaults.contains(&v.id);
                                            if stored.contains_key(&v.id)
                                                && ui
                                                    .button(if is_default {
                                                        "Reset to default"
                                                    } else {
                                                        "Delete"
                                                    })
                                                    .clicked()
                                            {
                                                remove = Some(v.id);
                                                ui.close();
                                            }
                                        });
                                    }
                                });
                        }
                        ui.menu_button("New view…", |ui| {
                            for lv in &levels {
                                if ui.button(format!("Plan of {}", lv.name)).clicked() {
                                    new_view = Some(views::new_plan(lv, Look::Down));
                                    ui.close();
                                }
                            }
                            for lv in &levels {
                                if ui.button(format!("Ceiling plan of {}", lv.name)).clicked() {
                                    new_view = Some(views::new_plan(lv, Look::Up));
                                    ui.close();
                                }
                            }
                            ui.separator();
                            let n = self
                                .views
                                .iter()
                                .filter(|v| matches!(v.kind, ViewKind::Section { .. }))
                                .count()
                                + 1;
                            for f in Facing::ALL {
                                if ui
                                    .button(format!("Section looking {}", f.label()))
                                    .clicked()
                                {
                                    new_view = Some(views::new_section(&l.scene, f, n));
                                    ui.close();
                                }
                            }
                            ui.separator();
                            if ui.button("3D view").clicked() {
                                new_view = Some(View {
                                    id: GlobalId::new(),
                                    name: format!("3D {}", self.views.len() + 1),
                                    kind: ViewKind::ThreeD,
                                    visibility: Visibility::default(),
                                    references: Vec::new(),
                                    annotations: Vec::new(),
                                });
                                ui.close();
                            }
                        });
                    });
                egui::CollapsingHeader::new(egui::RichText::new("Quantities").strong()).show(
                    ui,
                    |ui| {
                        egui::Grid::new("quantities").striped(true).show(ui, |ui| {
                            ui.strong("IFC class");
                            ui.strong("Count");
                            ui.strong("Volume");
                            ui.end_row();
                            for (class, n, vol) in views::quantities(&l.scene) {
                                ui.label(class);
                                ui.label(n.to_string());
                                ui.label(format!("{vol:.3} m³"));
                                ui.end_row();
                            }
                        });
                    },
                );
                egui::CollapsingHeader::new(egui::RichText::new("Layouts").strong()).show(
                    ui,
                    |ui| {
                        for sh in &sheets {
                            ui.horizontal(|ui| {
                                ui.label(format!("{}  {}", sh.number, sh.name));
                                if ui
                                    .small_button("PDF…")
                                    .on_hover_text("The sheet, its views at their scales")
                                    .clicked()
                                {
                                    export_sheet = Some(sh.clone());
                                }
                            });
                        }
                        if sheets.is_empty() {
                            ui.weak(
                                "No sheets yet (bimble sheet-new; placing views here comes next).",
                            );
                        }
                        if ui.button("Drawing on paper…").clicked() {
                            open_plan = true;
                        }
                    },
                );
                egui::CollapsingHeader::new(egui::RichText::new("Objects").strong()).show(
                    ui,
                    |ui| {
                        for (class, n) in &l.classes {
                            egui::CollapsingHeader::new(format!("{class} ({n})"))
                                .id_salt(("objects", class))
                                .show(ui, |ui| {
                                    for (i, e) in l
                                        .scene
                                        .elements
                                        .iter()
                                        .enumerate()
                                        .filter(|(_, e)| &e.class == class)
                                        .take(500)
                                    {
                                        let name = if e.name.is_empty() {
                                            "(no name)"
                                        } else {
                                            e.name.as_str()
                                        };
                                        if ui
                                            .selectable_label(self.selected == Some(i), name)
                                            .clicked()
                                        {
                                            select_element = Some(i);
                                        }
                                    }
                                    if *n > 500 {
                                        ui.weak(format!("… and {} more", n - 500));
                                    }
                                });
                        }
                    },
                );
            }
        }
        if let Some(v) = new_view {
            let id = v.id;
            match self.store_view(v) {
                Ok(()) => activate = Some(id),
                Err(e) => self.view_message = Some(e),
            }
        }
        if let Some(id) = remove {
            self.remove_view(id);
        }
        if let Some(id) = activate {
            self.activate_view(id);
        }
        if let Some(i) = select_element {
            self.selected = Some(i);
            self.needs_upload = true;
            self.fit_selection(self.last_aspect);
        }
        if open_plan {
            self.plan_window = true;
        }
        if let Some(sh) = export_sheet {
            self.export_sheet(&sh);
        }
    }

    // ---------------------------------------------------------------- view properties

    pub(crate) fn view_properties(&mut self, ui: &mut egui::Ui) {
        let Some(active) = self.active_view().cloned() else {
            ui.weak("No view.");
            return;
        };
        let levels = self.levels();
        if self.view_form.as_ref().is_none_or(|f| f.id != active.id) {
            self.view_form = Some(active.clone());
        }
        let mut commit = false;
        let mut pick_reference = false;
        let mut visibility_changed = false;
        let editable =
            self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) || self.side_views.is_some();
        {
            let form = self.view_form.as_mut().expect("set above");
            ui.heading(match &form.kind {
                ViewKind::ThreeD => "3D view",
                ViewKind::Plan {
                    look: Look::Down, ..
                } => "Plan",
                ViewKind::Plan { look: Look::Up, .. } => "Ceiling plan (looking up)",
                ViewKind::Section { .. } => "Section",
            });
            if let Some(m) = &self.view_message {
                ui.colored_label(ui.visuals().warn_fg_color, m);
            }
            if let Some(m) = &self.view_note {
                ui.weak(m);
            }
            egui::Grid::new("view-props").num_columns(2).show(ui, |ui| {
                ui.label("Name");
                commit |= ui
                    .add_enabled(
                        editable,
                        egui::TextEdit::singleline(&mut form.name).desired_width(180.0),
                    )
                    .lost_focus();
                ui.end_row();
                match &mut form.kind {
                    ViewKind::ThreeD => {}
                    ViewKind::Plan { storey, .. } => {
                        ui.label("Storey");
                        ui.label(
                            levels
                                .iter()
                                .find(|l| &l.key == storey)
                                .map(|l| format!("{} ({:+.3} m)", l.name, l.elevation_mm / 1000.0))
                                .unwrap_or_else(|| storey.clone()),
                        );
                        ui.end_row();
                    }
                    ViewKind::Section {
                        origin,
                        facing,
                        depth_mm,
                    } => {
                        ui.label("Through X / Y");
                        ui.horizontal(|ui| {
                            for v in origin.iter_mut() {
                                let r = ui.add(egui::DragValue::new(v).speed(10.0).suffix(" mm"));
                                commit |= r.drag_stopped() || r.lost_focus();
                            }
                        });
                        ui.end_row();
                        ui.label("Looking");
                        egui::ComboBox::from_id_salt("facing")
                            .selected_text(facing.label())
                            .show_ui(ui, |ui| {
                                for f in Facing::ALL {
                                    commit |= ui.selectable_value(facing, f, f.label()).changed();
                                }
                            });
                        ui.end_row();
                        ui.label("Depth (0: all)");
                        let r = ui.add(
                            egui::DragValue::new(depth_mm)
                                .speed(10.0)
                                .range(0.0..=1.0e7)
                                .suffix(" mm"),
                        );
                        commit |= r.drag_stopped() || r.lost_focus();
                        ui.end_row();
                    }
                }
            });
            if let ViewKind::Plan { range, look, .. } = &mut form.kind {
                ui.add_space(4.0);
                ui.strong("View range");
                egui::Grid::new("view-range").num_columns(3).show(ui, |ui| {
                    let rows: [(&str, &mut bimble_core::view::RangePlane); 4] = [
                        ("Top", &mut range.top),
                        ("Cut plane", &mut range.cut),
                        ("Bottom", &mut range.bottom),
                        (
                            match look {
                                Look::Down => "View depth (below)",
                                Look::Up => "View depth (above)",
                            },
                            &mut range.depth,
                        ),
                    ];
                    for (label, plane) in rows {
                        ui.label(label);
                        egui::ComboBox::from_id_salt(("level", label))
                            .selected_text(plane.level.label())
                            .show_ui(ui, |ui| {
                                for lr in LevelRef::ALL {
                                    commit |= ui
                                        .selectable_value(&mut plane.level, lr, lr.label())
                                        .changed();
                                }
                            });
                        let r = ui.add_enabled(
                            plane.level != LevelRef::Unlimited,
                            egui::DragValue::new(&mut plane.offset_mm)
                                .speed(10.0)
                                .range(-1.0e6..=1.0e6)
                                .suffix(" mm"),
                        );
                        commit |= r.drag_stopped() || r.lost_focus();
                        ui.end_row();
                    }
                });
                ui.weak(match look {
                    Look::Down => "Shows from the view depth up to the cut plane.",
                    Look::Up => "Shows from the cut plane up to the view depth.",
                });
            }
        }

        // Visibility: what this view hides.
        ui.add_space(6.0);
        egui::CollapsingHeader::new("Visibility")
            .default_open(true)
            .show(ui, |ui| {
                let Some(l) = &self.loaded else { return };
                for (cat, n) in &l.categories {
                    let mut on = !self.hidden_categories.contains(cat);
                    ui.horizontal(|ui| {
                        let c = self.opts.style.families.get(*cat).color.0;
                        let (r, _) = ui.allocate_exact_size(Vec2::splat(12.0), Sense::hover());
                        ui.painter()
                            .rect_filled(r, 2.0, Color32::from_rgb(c[0], c[1], c[2]));
                        if ui
                            .checkbox(&mut on, format!("{} ({n})", cat.label()))
                            .changed()
                        {
                            toggle(&mut self.hidden_categories, *cat, on);
                            visibility_changed = true;
                        }
                    });
                }
                ui.separator();
                for (class, n) in &l.classes {
                    let mut on = !self.hidden_classes.contains(class);
                    if ui.checkbox(&mut on, format!("{class} ({n})")).changed() {
                        toggle(&mut self.hidden_classes, class.clone(), on);
                        visibility_changed = true;
                    }
                }
                ui.separator();
                for (storey, n) in &l.storeys {
                    let mut on = !self.hidden_storeys.contains(storey);
                    let label = if storey.is_empty() {
                        "(none)"
                    } else {
                        storey.as_str()
                    };
                    if ui.checkbox(&mut on, format!("{label} ({n})")).changed() {
                        toggle(&mut self.hidden_storeys, storey.clone(), on);
                        visibility_changed = true;
                    }
                }
            });

        // Reference plans (2D views).
        let mut edits = RefEdits::default();
        if active.is_2d() {
            ui.add_space(6.0);
            egui::CollapsingHeader::new("Reference plans")
                .default_open(true)
                .show(ui, |ui| {
                    let form = self.view_form.as_mut().expect("set above");
                    for (k, r) in form.references.iter_mut().enumerate() {
                        ui.push_id(r.id.as_str(), |ui| {
                            ui.horizontal(|ui| {
                                edits.commit |= ui.checkbox(&mut r.visible, "").changed();
                                ui.label(&r.name);
                                if ui.small_button("Remove").clicked() {
                                    edits.remove = Some(k);
                                }
                            });
                            ui.horizontal(|ui| {
                                ui.weak("Move");
                                for v in r.offset_mm.iter_mut() {
                                    let d =
                                        ui.add(egui::DragValue::new(v).speed(10.0).suffix(" mm"));
                                    edits.commit |= d.drag_stopped() || d.lost_focus();
                                }
                                let d = ui.add(
                                    egui::DragValue::new(&mut r.rotation_deg)
                                        .speed(0.1)
                                        .range(-360.0..=360.0)
                                        .suffix("°"),
                                );
                                edits.commit |= d.drag_stopped() || d.lost_focus();
                            });
                            let moving = self.moving_ref == Some(r.id);
                            if ui
                                .selectable_label(moving, "Drag it in the view")
                                .on_hover_text(
                                    "While on, dragging in the view moves this reference",
                                )
                                .clicked()
                            {
                                self.moving_ref = if moving { None } else { Some(r.id) };
                            }
                        });
                        ui.separator();
                    }
                    ui.horizontal(|ui| {
                        if ui
                            .add_enabled(editable, egui::Button::new("Add reference plan…"))
                            .on_hover_text("DXF or vector PDF (a DWG: convert it to DXF first)")
                            .clicked()
                        {
                            pick_reference = true;
                        }
                        ui.label("PDF at 1:");
                        ui.add(egui::DragValue::new(&mut self.pdf_scale).range(1.0..=100_000.0));
                    });
                });
        }
        if let Some(k) = edits.remove {
            let form = self.view_form.as_mut().expect("set above");
            let r = form.references.remove(k);
            if self.moving_ref == Some(r.id) {
                self.moving_ref = None;
            }
            edits.commit = true;
        }
        if visibility_changed {
            let vis = self.current_visibility();
            if let Some(f) = self.view_form.as_mut() {
                f.visibility = vis;
            }
            if self.selected.is_some_and(|i| !self.visible(i)) {
                self.selected = None;
            }
            self.needs_upload = true;
            commit = true;
        }
        if (commit || edits.commit) && editable {
            let v = self.view_form.clone().expect("set above");
            if v != active {
                match self.store_view(v) {
                    Ok(()) => self.view_message = None,
                    Err(e) => {
                        self.view_message = Some(e);
                        self.view_form = None;
                        // The visibility shown is the view's again.
                        self.apply_visibility(&active.visibility);
                    }
                }
            }
        }
        if pick_reference {
            self.reference_view = Some(active.id);
            self.files.pick(files::Purpose::Reference, ui.ctx());
        }
    }

    /// A picked reference plan: read it, place it, add it to the view it was asked for.
    pub(crate) fn add_reference(&mut self, p: files::Picked) {
        let Some(id) = self.reference_view.take() else {
            return;
        };
        let Some(mut v) = self.views.iter().find(|v| v.id == id).cloned() else {
            return;
        };
        let read = p
            .content()
            .and_then(|b| bimble_drawing::reference::read(&p.name, &b, self.pdf_scale));
        let read = match read {
            Ok(r) => r,
            Err(e) => {
                self.view_message = Some(e);
                return;
            }
        };
        let mut r = Reference {
            id: GlobalId::new(),
            name: p.name.clone(),
            format: p
                .name
                .rsplit('.')
                .next()
                .unwrap_or_default()
                .to_ascii_lowercase(),
            offset_mm: [0.0, 0.0],
            rotation_deg: 0.0,
            visible: true,
            polylines: read.polylines,
        };
        let placed = self.place_reference(&v, &mut r);
        let notes = read.notes.join("; ");
        v.references.push(r);
        let id = v.id;
        match self.store_view(v) {
            Ok(()) => {
                self.view_note = Some(format!(
                    "Added {}: {notes}{}",
                    p.name,
                    if placed {
                        "; moved onto the model (its coordinates were elsewhere)"
                    } else {
                        ""
                    }
                ));
                self.activate_view(id);
            }
            Err(e) => self.view_message = Some(e),
        }
    }

    /// Keep a reference where its file puts it when that overlaps the model (shared project
    /// coordinates); else centre it on the model. Returns whether it was moved.
    fn place_reference(&self, v: &View, r: &mut Reference) -> bool {
        let Some(l) = &self.loaded else { return false };
        let Some(b) = ClipBox::around(&l.scene, |_| true) else {
            return false;
        };
        let o = l.scene.origin_mm;
        // The model's box in the view's plane (world mm).
        let (lo, hi) = match &v.kind {
            ViewKind::Section { facing, .. } => {
                let rv = views::section_right(*facing);
                let along = |p: Vec3| (p.x as f64 + o[0]) * rv[0] + (p.y as f64 + o[1]) * rv[1];
                let (a, c) = (along(b.min), along(b.max));
                (
                    [a.min(c), b.min.z as f64 + o[2]],
                    [a.max(c), b.max.z as f64 + o[2]],
                )
            }
            _ => (
                [b.min.x as f64 + o[0], b.min.y as f64 + o[1]],
                [b.max.x as f64 + o[0], b.max.y as f64 + o[1]],
            ),
        };
        let pts: Vec<[f64; 2]> = r.polylines.iter().flatten().copied().collect();
        if pts.is_empty() {
            return false;
        }
        let (mut rl, mut rh) = ([f64::MAX; 2], [f64::MIN; 2]);
        for p in &pts {
            for k in 0..2 {
                rl[k] = rl[k].min(p[k]);
                rh[k] = rh[k].max(p[k]);
            }
        }
        let grow = |a: f64, b: f64| (b - a) * 0.5;
        let overlaps = (0..2)
            .all(|k| rl[k] <= hi[k] + grow(lo[k], hi[k]) && rh[k] >= lo[k] - grow(lo[k], hi[k]));
        if overlaps {
            return false;
        }
        for k in 0..2 {
            r.offset_mm[k] = ((lo[k] + hi[k]) / 2.0 - (rl[k] + rh[k]) / 2.0).round();
        }
        true
    }

    // ---------------------------------------------------------------- references in the view

    /// The active view's references as placed now (the form's while it is being edited).
    fn shown_references(&self) -> Vec<(View, Reference)> {
        let Some(v) = self.active_view() else {
            return Vec::new();
        };
        let src = match &self.view_form {
            Some(f) if f.id == v.id => f,
            _ => v,
        };
        src.references
            .iter()
            .filter(|r| r.visible)
            .map(|r| (src.clone(), r.clone()))
            .collect()
    }

    /// Reference plans over a 2D view, in the reference colour.
    pub(crate) fn draw_references(&self, ui: &egui::Ui, rect: egui::Rect, aspect: f32) {
        let Some(l) = &self.loaded else { return };
        let refs = self.shown_references();
        if refs.is_empty() {
            return;
        }
        let levels = l.scene.levels();
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let c = self.opts.style.reference_edges.0;
        let painter = ui.painter_at(rect);
        for (v, r) in refs {
            let moving = self.moving_ref == Some(r.id);
            let col = if moving {
                let a = self.opts.style.selection.0;
                Color32::from_rgb(a[0], a[1], a[2])
            } else {
                Color32::from_rgb(c[0], c[1], c[2])
            };
            let stroke = egui::Stroke::new(1.0, col);
            for s in views::reference_segments(&v, &r, &l.scene, &levels) {
                if let (Some(a), Some(b)) = (
                    section::project(&vp, rect, s[0]),
                    section::project(&vp, rect, s[1]),
                ) && rect.intersects(egui::Rect::from_two_pos(a, b))
                {
                    painter.line_segment([a, b], stroke);
                }
            }
        }
    }

    /// The nearest corner (within 10 px) or else edge (within 8 px) of a reference plan under
    /// the cursor, with its screen distance.
    pub(crate) fn reference_snap(
        &self,
        pos: egui::Pos2,
        rect: egui::Rect,
        aspect: f32,
    ) -> Option<(measure::Snap, f32)> {
        let l = self.loaded.as_ref()?;
        let refs = self.shown_references();
        if refs.is_empty() {
            return None;
        }
        let levels = l.scene.levels();
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let mut best_v: Option<(f32, Vec3)> = None;
        let mut best_e: Option<(f32, Vec3)> = None;
        for (v, r) in refs {
            for s in views::reference_segments(&v, &r, &l.scene, &levels) {
                let (Some(a), Some(b)) = (
                    section::project(&vp, rect, s[0]),
                    section::project(&vp, rect, s[1]),
                ) else {
                    continue;
                };
                for (p, w) in [(a, s[0]), (b, s[1])] {
                    let d = p.distance(pos);
                    if d <= measure::VERTEX_SNAP_PX && best_v.is_none_or(|(bd, _)| d < bd) {
                        best_v = Some((d, w));
                    }
                }
                let ab = b - a;
                let len2 = ab.length_sq();
                if len2 > 1e-6 {
                    let t = ((pos - a).dot(ab) / len2).clamp(0.0, 1.0);
                    let q = a + ab * t;
                    let d = q.distance(pos);
                    if d <= measure::EDGE_SNAP_PX && best_e.is_none_or(|(bd, _)| d < bd) {
                        best_e = Some((d, s[0] + (s[1] - s[0]) * t));
                    }
                }
            }
        }
        let snap = |(d, p): (f32, Vec3), kind| {
            (
                measure::Snap {
                    point: p,
                    kind,
                    normal: None,
                    element: usize::MAX,
                },
                d,
            )
        };
        best_v
            .map(|b| snap(b, measure::SnapKind::Vertex))
            .or(best_e.map(|b| snap(b, measure::SnapKind::Edge)))
    }

    /// Drag the reference being moved by a screen delta. Returns whether one was moved.
    pub(crate) fn drag_reference(
        &mut self,
        delta: egui::Vec2,
        rect: egui::Rect,
        aspect: f32,
    ) -> bool {
        let (Some(id), Some(l)) = (self.moving_ref, &self.loaded) else {
            return false;
        };
        let Some(active) = self.active_view().cloned() else {
            return false;
        };
        let ndc = |p: egui::Pos2| {
            [
                (p.x - rect.min.x) / rect.width() * 2.0 - 1.0,
                1.0 - (p.y - rect.min.y) / rect.height() * 2.0,
            ]
        };
        let c = rect.center();
        let (a, _) = self.camera.ray(ndc(c), aspect, &l.bounds);
        let (b, _) = self.camera.ray(ndc(c + delta), aspect, &l.bounds);
        let mv = views::reference_move(&active, b - a);
        if self.view_form.as_ref().is_none_or(|f| f.id != active.id) {
            self.view_form = Some(active);
        }
        let Some(r) = self
            .view_form
            .as_mut()
            .and_then(|f| f.references.iter_mut().find(|r| r.id == id))
        else {
            return false;
        };
        r.offset_mm[0] = (r.offset_mm[0] + mv[0]).round();
        r.offset_mm[1] = (r.offset_mm[1] + mv[1]).round();
        true
    }

    /// The dragged reference's new place, stored once the drag ends.
    pub(crate) fn finish_reference_drag(&mut self) {
        let Some(v) = self.view_form.clone() else {
            return;
        };
        if self.active_view().is_some_and(|a| *a != v)
            && let Err(e) = self.store_view(v)
        {
            self.view_message = Some(e);
            self.view_form = None;
        }
    }
}
