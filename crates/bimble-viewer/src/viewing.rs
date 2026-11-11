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
    /// Project information (ADR 0030), kept and saved back.
    pub project: BTreeMap<String, String>,
    /// Schedules changed or added (ADR 0029), kept and saved back.
    pub schedules: Vec<bimble_core::schedule::ScheduleDef>,
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
    /// A views side file opened or dropped (#36): its views and sheets replace those of the
    /// open IFC, once its plans are checked against the model's storeys. In a browser this is
    /// how views saved earlier (downloaded) come back.
    pub(crate) fn open_side_file(&mut self, name: &str, bytes: &[u8]) -> Result<String, String> {
        let Some(l) = &self.loaded else {
            return Err(format!("{name}: open its model first, then the views file"));
        };
        if l.edit.is_some() {
            return Err(format!(
                "{name}: a .bimble model keeps its views inside; views files are for IFC"
            ));
        }
        let f = ViewsFile::from_bytes(bytes).map_err(|e| format!("{name}: {e}"))?;
        let warnings = check_side_file(&f, &self.levels()).map_err(|e| format!("{name}: {e}"))?;
        let (n, s) = (f.views.len(), f.sheets.len());
        self.side_views = Some(SideViews {
            stored: f.views,
            sheets: f.sheets,
            project: f.project,
            schedules: f.schedules,
            dirty: false,
        });
        self.active_view = None;
        self.layout = None;
        self.refresh_views();
        let mut msg = format!("{name}: {n} views, {s} sheets");
        if !warnings.is_empty() {
            msg.push_str(&format!(" ({})", warnings.join("; ")));
        }
        Ok(msg)
    }

    /// The project information (ADR 0030).
    pub(crate) fn project_info(&self) -> BTreeMap<String, String> {
        project_of(&self.loaded, &self.side_views)
    }

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

    /// A view's scale (ADR 0046), and its placements on sheets with it, so a file read by an
    /// older version prints the same.
    pub(crate) fn set_view_scale(&mut self, id: GlobalId, scale: u32) {
        let Some(mut v) = self.views.iter().find(|v| v.id == id).cloned() else {
            return;
        };
        v.scale = scale;
        if let Err(e) = self.store_view(v) {
            self.view_message = Some(e);
            return;
        }
        for mut s in self.sheets() {
            let mut changed = false;
            for p in s
                .placed
                .iter_mut()
                .filter(|p| p.view == id && p.scale != scale)
            {
                p.scale = scale;
                changed = true;
            }
            if changed && let Err(e) = self.store_sheet(s) {
                self.view_message = Some(e);
            }
        }
        self.view_form = None;
    }

    /// A plan (or ceiling plan) is shown: its cut is a horizontal plane.
    pub(crate) fn in_plan(&self) -> bool {
        self.active_view()
            .is_some_and(|v| matches!(v.kind, ViewKind::Plan { .. }))
            && self.in_2d()
    }

    /// The building's turn (radians): from above, project north up (ADR 0045).
    pub(crate) fn project_north(&self) -> f32 {
        self.loaded
            .as_ref()
            .map_or(0.0, |l| l.scene.georef.plan_turn(false) as f32)
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
            let (stored, sheets, project, schedules) = (
                BTreeMap::new(),
                BTreeMap::new(),
                BTreeMap::new(),
                Vec::new(),
            );
            #[cfg(not(target_arch = "wasm32"))]
            let (stored, sheets, project, schedules) = {
                let p = side_file(&l.path);
                match p.exists().then(|| {
                    std::fs::read(&p)
                        .map_err(|e| e.to_string())
                        .and_then(|b| ViewsFile::from_bytes(&b))
                }) {
                    Some(Ok(f)) => (f.views, f.sheets, f.project, f.schedules),
                    Some(Err(e)) => {
                        self.view_message = Some(format!("{}: {e}", p.display()));
                        (
                            BTreeMap::new(),
                            BTreeMap::new(),
                            BTreeMap::new(),
                            Vec::new(),
                        )
                    }
                    None => (
                        BTreeMap::new(),
                        BTreeMap::new(),
                        BTreeMap::new(),
                        Vec::new(),
                    ),
                }
            };
            self.side_views = Some(SideViews {
                stored,
                sheets,
                project,
                schedules,
                dirty: false,
            });
        }
        self.active_view = None;
        self.refresh_views();
        self.adopt_view_scales();
    }

    /// Views placed on sheets at another scale than theirs, in files from before 0.15: they take
    /// it (ADR 0046), as an edit to save, so the sheets print as before. Said, never silent.
    fn adopt_view_scales(&mut self) {
        let adopted = bimble_core::sheet::adopted_scales(&self.views, &self.sheets());
        if adopted.is_empty() {
            return;
        }
        let mut names = Vec::new();
        for (id, scale) in adopted {
            let Some(mut v) = self.views.iter().find(|v| v.id == id).cloned() else {
                continue;
            };
            v.scale = scale;
            names.push(format!("{} 1:{scale}", v.name));
            if let Err(e) = self.store_view(v) {
                self.view_message = Some(e);
                return;
            }
        }
        self.view_note = Some(format!(
            "Scales now belong to views: {} (from their sheets; save to keep)",
            names.join(", ")
        ));
    }

    /// Switch to a view: its camera (remembered per view), clip and visibility.
    pub(crate) fn activate_view(&mut self, id: GlobalId) {
        // Choosing a view leaves a schedule shown in its place.
        self.close_pages();
        if self.active_view == Some(id) {
            return;
        }
        if let Some(cur) = self.active_view {
            self.view_cameras.insert(cur, self.camera);
        }
        // Hidden for now belongs to the view it was done in (Revit's temporary hide).
        self.temporary = None;
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
                let georef = self
                    .loaded
                    .as_ref()
                    .map(|l| l.scene.georef.clone())
                    .unwrap_or_default();
                if let Some((yaw, pitch, mirror)) = views::camera_angles(&v, &georef) {
                    self.camera.yaw = yaw;
                    self.camera.pitch = pitch;
                    self.camera.mirror_x = mirror;
                    self.camera.orthographic = true;
                } else {
                    // The usual 3D angle, seen from the building's south-west (project north).
                    self.camera = Camera::default();
                    self.camera.yaw += georef.plan_turn(false) as f32;
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

    /// A material drawn as another family (the graphic standards, for every model): back to
    /// the family it came with, the move is forgotten.
    fn move_material(&mut self, m: String, to: MaterialCategory) {
        let Some(l) = &self.loaded else { return };
        let came_with = l
            .scene
            .elements
            .iter()
            .zip(&l.imported)
            .find(|(e, _)| e.material == m)
            .map(|(_, &c)| c);
        let map = &mut self.opts.style.material_families;
        if came_with == Some(to) {
            map.remove(&m);
        } else {
            map.insert(m, to);
        }
        // Applied at the end of the frame (Loaded::apply_families).
    }

    fn apply_visibility(&mut self, v: &Visibility) {
        self.hidden_categories = MaterialCategory::ALL
            .into_iter()
            .filter(|c| v.hidden_families.iter().any(|f| f == c.label()))
            .collect();
        self.hidden_materials = v.hidden_materials.iter().cloned().collect();
        self.hidden_classes = v.hidden_classes.iter().cloned().collect();
        self.hidden_storeys = v.hidden_storeys.iter().cloned().collect();
        self.hidden_links = v.hidden_links.iter().cloned().collect();
        self.hidden_ids = v.hidden_elements.iter().cloned().collect();
        self.isolated_ids = v.isolated_elements.iter().cloned().collect();
        if self.selected.is_some_and(|i| !self.visible(i)) {
            self.selected = None;
        }
        let kept: BTreeSet<usize> = self
            .also_selected
            .iter()
            .copied()
            .filter(|&i| self.visible(i))
            .collect();
        self.also_selected = kept;
    }

    /// Save what is hidden by element in the active view (#54), like the other visibility.
    pub(crate) fn save_visibility(&mut self) {
        let editable =
            self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) || self.side_views.is_some();
        if !editable {
            self.view_message =
                Some("This view can't be saved here: hidden until the file is opened again".into());
            return;
        }
        let vis = self.current_visibility();
        if let Some(mut v) = self.active_view().cloned()
            && v.visibility != vis
        {
            v.visibility = vis;
            if let Err(e) = self.store_view(v) {
                self.view_message = Some(e);
            }
        }
        self.view_form = None;
    }

    /// Show everything in the active view again, and save that with the view (ADR 0028).
    pub(crate) fn show_all_in_view(&mut self) {
        self.hidden_categories.clear();
        self.hidden_materials.clear();
        self.hidden_classes.clear();
        self.hidden_storeys.clear();
        self.hidden_links.clear();
        self.hidden_ids.clear();
        self.isolated_ids.clear();
        let editable =
            self.loaded.as_ref().is_some_and(|l| l.edit.is_some()) || self.side_views.is_some();
        if editable
            && let Some(mut v) = self.active_view().cloned()
            && !v.visibility.is_empty()
        {
            v.visibility = Visibility::default();
            if let Err(e) = self.store_view(v) {
                self.view_message = Some(e);
            }
        }
        self.view_form = None;
        self.needs_upload = true;
    }

    fn current_visibility(&self) -> Visibility {
        Visibility {
            hidden_families: self
                .hidden_categories
                .iter()
                .map(|c| c.label().to_owned())
                .collect(),
            hidden_materials: self.hidden_materials.iter().cloned().collect(),
            hidden_classes: self.hidden_classes.iter().cloned().collect(),
            hidden_storeys: self.hidden_storeys.iter().cloned().collect(),
            hidden_links: self.hidden_links.iter().cloned().collect(),
            hidden_elements: self.hidden_ids.iter().cloned().collect(),
            isolated_elements: self.isolated_ids.iter().cloned().collect(),
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
            style: self.opts.style.clone(),
            template: self.plan.template.with_project(&self.project_info()),
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
        let text = ViewsFile::new(s.stored.clone(), s.sheets.clone())
            .with_project(s.project.clone())
            .with_schedules(s.schedules.clone())
            .to_json();
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

    /// The model navigator: views, schedules, layouts, objects, links, measurements, warnings.
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
            self.close_sheet();
            self.activate_view(id);
        }
    }

    fn navigator_tree(&mut self, ui: &mut egui::Ui) {
        let schedule_counts = self.schedule_counts();
        let open_schedule_name = self.schedule.as_ref().map(|s| s.name.clone());
        let mut open_schedule: Option<String> = None;
        let mut new_schedule = false;
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
        let mut open_sheet = None;
        let mut delete_sheet = None;
        let mut new_sheet = false;
        let open_sheet_id = self.layout.as_ref().map(|l| l.sheet);
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
                                        let on = on && self.layout.is_none();
                                        // 2D views can be dragged onto a sheet.
                                        let sense = if v.is_2d() {
                                            Sense::click_and_drag()
                                        } else {
                                            Sense::click()
                                        };
                                        let r =
                                            ui.add(egui::Button::selectable(on, name).sense(sense));
                                        if v.is_2d() {
                                            r.dnd_set_drag_payload(crate::layout::ViewDrag(v.id));
                                        }
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
                                    true_north: false,
                                    scale: 100,
                                });
                                ui.close();
                            }
                        });
                    });
                egui::CollapsingHeader::new(egui::RichText::new("Schedules").strong()).show(
                    ui,
                    |ui| {
                        if ui.button("New schedule").clicked() {
                            new_schedule = true;
                        }
                        for (name, n) in &schedule_counts {
                            let on = open_schedule_name.as_deref() == Some(name.as_str());
                            let r = ui.add_enabled(
                                *n > 0,
                                egui::Button::selectable(on, format!("{name}  ({n})")),
                            );
                            if r.clicked() {
                                open_schedule = Some(name.clone());
                            }
                        }
                    },
                );
                egui::CollapsingHeader::new(egui::RichText::new("Layouts").strong()).show(
                    ui,
                    |ui| {
                        for sh in &sheets {
                            let on = open_sheet_id == Some(sh.id);
                            let r = ui.selectable_label(on, format!("{}  {}", sh.number, sh.name));
                            if r.clicked() {
                                open_sheet = Some(sh.id);
                            }
                            r.context_menu(|ui| {
                                if ui.button("Export PDF…").clicked() {
                                    export_sheet = Some(sh.clone());
                                    ui.close();
                                }
                                if ui.button("Delete sheet").clicked() {
                                    delete_sheet = Some(sh.id);
                                    ui.close();
                                }
                            });
                        }
                        if ui.button("New sheet").clicked() {
                            new_sheet = true;
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
        if let Some(id) = open_sheet {
            self.open_sheet(id);
        }
        if let Some(n) = open_schedule {
            self.open_schedule(&n);
        }
        if new_schedule {
            self.new_schedule();
        }
        if let Some(id) = delete_sheet {
            self.delete_sheet(id);
        }
        if new_sheet {
            self.new_sheet();
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
        let mut open_standards: Option<String> = None;
        let mut families_moved: Option<(String, MaterialCategory)> = None;
        let mut turned = false;
        let mut new_scale: Option<u32> = None;
        let rotation = self
            .loaded
            .as_ref()
            .map_or(0.0, |l| l.scene.georef.rotation_deg);
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
                // Revit's view Orientation (ADR 0045): along the building, or true north.
                if form.kind != ViewKind::ThreeD {
                    ui.label("Orientation");
                    let before = form.true_north;
                    ui.add_enabled_ui(editable, |ui| {
                        egui::ComboBox::from_id_salt("orientation")
                            .selected_text(if form.true_north {
                                "True north"
                            } else {
                                "Project north"
                            })
                            .show_ui(ui, |ui| {
                                ui.selectable_value(&mut form.true_north, false, "Project north");
                                ui.selectable_value(&mut form.true_north, true, "True north");
                            })
                            .response
                            .on_hover_text(format!(
                                "Project north: along the building (turned {rotation:.2}° from \
                                 true north in this model). True north: north up."
                            ));
                    });
                    if form.true_north != before {
                        turned = true;
                        commit = true;
                    }
                    ui.end_row();
                    // The view's scale (ADR 0046): its pens and texts on paper and on screen, its
                    // size on every sheet.
                    ui.label("Scale 1:");
                    ui.add_enabled_ui(editable, |ui| {
                        ui.horizontal(|ui| {
                            let mut sc = form.scale;
                            let r = ui.add(egui::DragValue::new(&mut sc).range(1..=5000));
                            if (r.drag_stopped() || r.lost_focus()) && sc != active.scale {
                                new_scale = Some(sc);
                            }
                            form.scale = sc;
                            for k in [20, 50, 100, 200] {
                                if ui.small_button(k.to_string()).clicked() && k != active.scale {
                                    form.scale = k;
                                    new_scale = Some(k);
                                }
                            }
                        });
                    });
                    ui.end_row();
                }
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
                // Each family: its colour (a click opens it in the standards), and its
                // materials, each hidden on its own or moved to another family (saved in the
                // standards, for every model).
                let mut move_to: Option<(String, MaterialCategory)> = None;
                for (cat, n) in &l.categories {
                    let mut on = !self.hidden_categories.contains(cat);
                    let id = ui.make_persistent_id(("vis-family", *cat));
                    egui::collapsing_header::CollapsingState::load_with_default_open(
                        ui.ctx(),
                        id,
                        false,
                    )
                    .show_header(ui, |ui| {
                        let c = self.opts.style.families.get(*cat).color.0;
                        let (r, swatch) = ui.allocate_exact_size(Vec2::splat(12.0), Sense::click());
                        ui.painter()
                            .rect_filled(r, 2.0, Color32::from_rgb(c[0], c[1], c[2]));
                        if swatch
                            .on_hover_text("Colour and hatches: open in the graphic standards")
                            .on_hover_cursor(egui::CursorIcon::PointingHand)
                            .clicked()
                        {
                            open_standards = Some(format!("{} on screen", cat.label()));
                        }
                        if ui
                            .checkbox(&mut on, format!("{} ({n})", cat.label()))
                            .changed()
                        {
                            toggle(&mut self.hidden_categories, *cat, on);
                            visibility_changed = true;
                        }
                    })
                    .body(|ui| {
                        for ((_, m), k) in l
                            .materials
                            .range((*cat, String::new())..)
                            .take_while(|((c, _), _)| c == cat)
                        {
                            ui.horizontal(|ui| {
                                let mut on = !self.hidden_materials.contains(m);
                                let label = if m.is_empty() { "(no material)" } else { m };
                                if ui.checkbox(&mut on, format!("{label} ({k})")).changed() {
                                    toggle(&mut self.hidden_materials, m.clone(), on);
                                    visibility_changed = true;
                                }
                                if m.is_empty() {
                                    return;
                                }
                                let mut to = *cat;
                                egui::ComboBox::from_id_salt(("vis-move", m))
                                    .selected_text(to.label())
                                    .width(90.0)
                                    .show_ui(ui, |ui| {
                                        for c in MaterialCategory::ALL {
                                            ui.selectable_value(&mut to, c, c.label());
                                        }
                                    })
                                    .response
                                    .on_hover_text(
                                        "The family this material is drawn as (saved in the \
                                         graphic standards)",
                                    );
                                if to != *cat {
                                    move_to = Some((m.clone(), to));
                                }
                            });
                        }
                    });
                }
                if let Some((m, to)) = move_to {
                    families_moved = Some((m, to));
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
                if !self.links.is_empty() {
                    ui.separator();
                    for name in self.links.iter().map(Linked::name).collect::<Vec<_>>() {
                        let mut on = !self.hidden_links.contains(&name);
                        if ui.checkbox(&mut on, format!("Link: {name}")).changed() {
                            toggle(&mut self.hidden_links, name, on);
                            visibility_changed = true;
                        }
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
        if let Some(q) = open_standards {
            self.standards_open = true;
            self.standards_query = q;
        }
        if let Some((m, to)) = families_moved {
            self.move_material(m, to);
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
        if let Some(sc) = new_scale {
            self.set_view_scale(active.id, sc);
        }
        // Turned, or a section looking elsewhere: the camera and the box follow at once.
        let facing = |v: &View| match v.kind {
            ViewKind::Section { facing, .. } => Some(facing),
            _ => None,
        };
        let turned = turned
            || self
                .active_view()
                .is_some_and(|v| v.id == active.id && facing(v) != facing(&active));
        if turned
            && let Some(v) = self.active_view().cloned()
            && let Some(g) = self.loaded.as_ref().map(|l| l.scene.georef.clone())
        {
            self.view_cameras.remove(&v.id);
            if let Some((yaw, pitch, mirror)) = views::camera_angles(&v, &g) {
                self.camera.yaw = yaw;
                self.camera.pitch = pitch;
                self.camera.mirror_x = mirror;
            }
            self.fit_pending = true;
            self.needs_upload = true;
            self.needs_render = true;
        }
        if pick_reference {
            self.reference_view = Some(active.id);
            self.reference_replace = None;
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
            source: p.path.as_ref().map(|p| p.display().to_string()),
            offset_mm: [0.0, 0.0],
            rotation_deg: 0.0,
            visible: true,
            polylines: read.polylines,
        };
        let notes = read.notes.join("; ");
        // Reloaded (Manage links): the new lines in the old one's place, kept where it was put.
        let replacing = self
            .reference_replace
            .take()
            .and_then(|old| v.references.iter_mut().find(|x| x.id == old));
        let placed = match replacing {
            Some(old) => {
                crate::views::reload_reference(old, r);
                Placed::AsIs
            }
            None => {
                let placed = self.place_reference(&v, &mut r);
                v.references.push(r);
                placed
            }
        };
        let id = v.id;
        match self.store_view(v) {
            Ok(()) => {
                self.view_note = Some(format!(
                    "Added {}: {notes}{}",
                    p.name,
                    match placed {
                        Placed::AsIs => "",
                        Placed::Survey => "; placed by its survey coordinates (LV95)",
                        Placed::Centred =>
                            "; its coordinates are elsewhere: centred on the model, upright",
                    }
                ));
                self.activate_view(id);
            }
            Err(e) => self.view_message = Some(e),
        }
    }

    /// Keep a reference where its file puts it when that overlaps the model (the same
    /// coordinates: an LV95 DXF on a model drawn in LV95); else, in a plan of a model in its own
    /// coordinates, where its survey coordinates say (IFC4 map conversion); else centre it on the
    /// model, upright in the view (project north: turned with the building). ADR 0045.
    fn place_reference(&self, v: &View, r: &mut Reference) -> Placed {
        let placed = self.place_reference_by_coordinates(v, r);
        if placed != Placed::Centred {
            return placed;
        }
        // Upright in a turned plan: the file's up is the view's up.
        if matches!(v.kind, ViewKind::Plan { .. })
            && let Some(l) = &self.loaded
        {
            r.rotation_deg = (v.plan_turn(&l.scene.georef).to_degrees() * 1e6).round() / 1e6 + 0.0;
        }
        // Centred again, now turned.
        if self.place_reference_by_coordinates(v, r) == Placed::AsIs {
            return Placed::AsIs;
        }
        Placed::Centred
    }

    fn place_reference_by_coordinates(&self, v: &View, r: &mut Reference) -> Placed {
        // A plan of a model in its own coordinates: try the reference's survey coordinates.
        if matches!(v.kind, ViewKind::Plan { .. })
            && let Some(l) = &self.loaded
            && l.scene.georef.has_map()
            && r.rotation_deg == 0.0
            && r.offset_mm == [0.0, 0.0]
        {
            let (rot, off) = l.scene.georef.survey_plan_to_world();
            let mut by_survey = r.clone();
            by_survey.rotation_deg = rot;
            by_survey.offset_mm = off;
            if self.reference_overlaps(v, &by_survey) == Some(true) {
                *r = by_survey;
                return Placed::Survey;
            }
        }
        match self.reference_overlaps(v, r) {
            Some(true) | None => Placed::AsIs,
            Some(false) => {
                self.centre_reference(v, r);
                Placed::Centred
            }
        }
    }

    /// Whether a reference, placed as it is, overlaps the model in the view's plane (None: no
    /// model or no lines).
    fn reference_overlaps(&self, v: &View, r: &Reference) -> Option<bool> {
        let (lo, hi) = self.model_in_view_plane(v)?;
        let pts: Vec<[f64; 2]> = r.polylines.iter().flatten().map(|p| r.place(*p)).collect();
        if pts.is_empty() {
            return None;
        }
        let (mut rl, mut rh) = ([f64::MAX; 2], [f64::MIN; 2]);
        for p in &pts {
            for k in 0..2 {
                rl[k] = rl[k].min(p[k]);
                rh[k] = rh[k].max(p[k]);
            }
        }
        let grow = |a: f64, b: f64| (b - a) * 0.5;
        Some(
            (0..2).all(|k| {
                rl[k] <= hi[k] + grow(lo[k], hi[k]) && rh[k] >= lo[k] - grow(lo[k], hi[k])
            }),
        )
    }

    /// Move a reference so its middle is the model's (in the view's plane), its turn kept.
    fn centre_reference(&self, v: &View, r: &mut Reference) {
        let Some((lo, hi)) = self.model_in_view_plane(v) else {
            return;
        };
        let pts: Vec<[f64; 2]> = r.polylines.iter().flatten().map(|p| r.place(*p)).collect();
        let (mut rl, mut rh) = ([f64::MAX; 2], [f64::MIN; 2]);
        for p in &pts {
            for k in 0..2 {
                rl[k] = rl[k].min(p[k]);
                rh[k] = rh[k].max(p[k]);
            }
        }
        for k in 0..2 {
            r.offset_mm[k] =
                (r.offset_mm[k] + (lo[k] + hi[k]) / 2.0 - (rl[k] + rh[k]) / 2.0).round();
        }
    }

    /// The model's box in the view's plane (world mm): x, y in a plan; along, up in a section.
    fn model_in_view_plane(&self, v: &View) -> Option<([f64; 2], [f64; 2])> {
        let l = self.loaded.as_ref()?;
        let b = ClipBox::around(&l.scene, |_| true)?;
        let o = l.scene.origin_mm;
        // The model's box in the view's plane (world mm).
        let (lo, hi) = match &v.kind {
            ViewKind::Section { facing, .. } => {
                let rv = views::section_right(v.facing_dir(*facing, &l.scene.georef));
                let along = |x: f32, y: f32| (x as f64 + o[0]) * rv[0] + (y as f64 + o[1]) * rv[1];
                // Every corner: the section may be turned (project north).
                let c = [
                    along(b.min.x, b.min.y),
                    along(b.max.x, b.max.y),
                    along(b.min.x, b.max.y),
                    along(b.max.x, b.min.y),
                ];
                let (a, c) = (
                    c.iter().copied().fold(f64::INFINITY, f64::min),
                    c.iter().copied().fold(f64::NEG_INFINITY, f64::max),
                );
                ([a, b.min.z as f64 + o[2]], [c, b.max.z as f64 + o[2]])
            }
            _ => (
                [b.min.x as f64 + o[0], b.min.y as f64 + o[1]],
                [b.max.x as f64 + o[0], b.max.y as f64 + o[1]],
            ),
        };
        Some((lo, hi))
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
    /// The active 2D view's dimensions and texts over it (they belong to the view, ADR 0024).
    pub(crate) fn draw_annotations(&self, ui: &egui::Ui, rect: egui::Rect, aspect: f32) {
        use bimble_core::view::Annotation;
        use bimble_drawing::viewdraw::{Turn, ViewFrame, dimension_on_paper};
        let (Some(l), Some(v)) = (&self.loaded, self.active_view()) else {
            return;
        };
        if v.annotations.is_empty() {
            return;
        }
        let vp = self.camera.view_proj(aspect, &l.bounds);
        let o = l.scene.origin_mm;
        // The view's plane in model mm ("paper" at 1:1), and from there to the screen.
        let frame = ViewFrame {
            turn: Turn::of(v, &l.scene.georef),
            origin: o,
            x0: 0.0,
            y1: 0.0,
            k: 1.0,
            ox: 0.0,
            oy: 0.0,
            cut: 0.0,
        };
        let screen = |p: [f64; 2]| {
            let w = frame.to_world(p);
            section::project(
                &vp,
                rect,
                Vec3::new(
                    (w[0] - o[0]) as f32,
                    (w[1] - o[1]) as f32,
                    (w[2] - o[2]) as f32,
                ),
            )
        };
        let px_per_mm = match (screen([0.0, 0.0]), screen([1000.0, 0.0])) {
            (Some(a), Some(b)) => (a.distance(b) / 1000.0) as f64,
            _ => return,
        };
        let a = self.opts.style.dimensions.0;
        let col = Color32::from_rgb(a[0], a[1], a[2]);
        let painter = ui.painter_at(rect);
        // Text sizes as on paper at the view's scale (ADR 0046), within readable bounds.
        let k = f64::from(v.scale);
        let font = |size_mm: f64| {
            egui::FontId::proportional((size_mm * k * px_per_mm).clamp(8.0, 40.0) as f32)
        };
        for an in &v.annotations {
            match an {
                Annotation::Dimension {
                    a,
                    b,
                    offset_mm,
                    axis,
                    kind,
                    ..
                } => {
                    let dt = kind
                        .as_ref()
                        .and_then(|k| self.opts.style.paper.dim_type(k));
                    let col = dt.map_or(col, |d| {
                        Color32::from_rgb(d.color.0[0], d.color.0[1], d.color.0[2])
                    });
                    let stroke = egui::Stroke::new(1.0, col);
                    let (pa, pb) = (frame.to_paper(a.at), frame.to_paper(b.at));
                    let Some((_, qa, qb, len)) = dimension_on_paper(pa, pb, *offset_mm, *axis)
                    else {
                        continue;
                    };
                    let pts = [pa, qa, pb, qb].map(screen);
                    let [Some(sa), Some(sqa), Some(sb), Some(sqb)] = pts else {
                        continue;
                    };
                    painter.line_segment([sa, sqa], stroke);
                    painter.line_segment([sb, sqb], stroke);
                    painter.line_segment([sqa, sqb], stroke);
                    let mid = sqa + (sqb - sqa) / 2.0;
                    let angle = (sqb - sqa).angle();
                    let angle = if angle.abs() > std::f32::consts::FRAC_PI_2 {
                        angle + std::f32::consts::PI
                    } else {
                        angle
                    };
                    let (text, size) = match dt {
                        Some(d) => (d.format(len), d.text_mm as f64),
                        None => (self.plan.template.dimension_unit.format(len), 2.25),
                    };
                    let galley = painter.layout_no_wrap(text, font(size), col);
                    let size = galley.size();
                    let (s, c) = angle.sin_cos();
                    // Centred over the line, upright.
                    let up = egui::vec2(s, -c) * (size.y + 2.0);
                    let pos = mid + up - egui::vec2(c, s) * (size.x / 2.0);
                    painter.add(egui::epaint::TextShape::new(pos, galley, col).with_angle(angle));
                }
                Annotation::Text {
                    text,
                    at,
                    size_mm,
                    kind,
                    ..
                } => {
                    let tt = kind
                        .as_ref()
                        .and_then(|k| self.opts.style.paper.text_type(k));
                    let col = tt.map_or(col, |t| {
                        Color32::from_rgb(t.color.0[0], t.color.0[1], t.color.0[2])
                    });
                    let size = tt.map_or(*size_mm, |t| t.size_mm as f64);
                    if let Some(p) = screen(frame.to_paper(*at)) {
                        painter.text(p, egui::Align2::LEFT_BOTTOM, text, font(size), col);
                    }
                }
            }
        }
    }

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
        let georef = self
            .loaded
            .as_ref()
            .map(|l| l.scene.georef.clone())
            .unwrap_or_default();
        let mv = views::reference_move(&active, &georef, b - a);
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

/// Whether a views file fits a model: its plans must be on the model's storeys. None fitting
/// (and some plans): another model's file, refused. Some not fitting: listed as warnings.
pub(crate) fn check_side_file(f: &ViewsFile, levels: &[Level]) -> Result<Vec<String>, String> {
    let plans: Vec<(&str, &str)> = f
        .views
        .values()
        .filter_map(|v| match &v.kind {
            bimble_core::view::ViewKind::Plan { storey, .. } => {
                Some((v.name.as_str(), storey.as_str()))
            }
            _ => None,
        })
        .collect();
    let missing: Vec<&str> = plans
        .iter()
        .filter(|(_, s)| !levels.iter().any(|l| l.key == *s))
        .map(|(n, _)| *n)
        .collect();
    if !plans.is_empty() && missing.len() == plans.len() {
        return Err(
            "none of its plans is on a storey of this model: views of another model?".into(),
        );
    }
    Ok(missing
        .into_iter()
        .map(|n| format!("plan {n}: its storey is not in this model"))
        .collect())
}

#[cfg(test)]
mod side_file_tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    /// A views file of this model is taken; one whose plans are on no storey of it is refused;
    /// one with a plan on a missing storey is taken with a warning.
    #[test]
    fn a_views_file_is_checked_against_the_storeys() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let levels = scene.levels();
        let views: BTreeMap<GlobalId, View> = default_views(&levels)
            .into_iter()
            .map(|v| (v.id, v))
            .collect();
        let f = ViewsFile::new(views.clone(), BTreeMap::new());
        let f = ViewsFile::from_bytes(&serde_json::to_vec(&f).unwrap()).unwrap();
        assert_eq!(check_side_file(&f, &levels).unwrap(), Vec::<String>::new());
        let other: Vec<Level> = levels
            .iter()
            .map(|l| Level {
                key: format!("other-{}", l.key),
                ..l.clone()
            })
            .collect();
        assert!(check_side_file(&f, &other).is_err());
        let fewer = &levels[1..];
        let w = check_side_file(&f, fewer).unwrap();
        assert!(
            !w.is_empty() && w.iter().all(|x| x.contains("storey")),
            "{w:?}"
        );
    }
}

/// How a new reference plan was placed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Placed {
    /// Where its file says: the model's own coordinates.
    AsIs,
    /// By its survey coordinates, through the model's map conversion.
    Survey,
    /// Its coordinates are elsewhere: centred on the model, upright in the view.
    Centred,
}
