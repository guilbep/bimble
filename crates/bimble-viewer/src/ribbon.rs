//! The main window's frame (ADR 0027, ADR 0037): one top row (the File menu, the ribbon's tabs,
//! the file's name, quick access as icons, the command palette), the ribbon (tools in named
//! groups), tabs of the open views and sheets, and a command palette (Ctrl+K) that finds every
//! action by typing.
//!
//! Every button of the ribbon and every entry of the palette is an `Action`, run by one function,
//! so the two never disagree.

use super::*;
use crate::ui_theme as th;
use bimble_core::GlobalId;

/// The ribbon's tabs.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub(crate) enum Tab {
    /// What the project is, its settings and its links (#51).
    Project,
    #[default]
    Model,
    Annotate,
    View,
    Sheets,
    Manage,
}

impl Tab {
    pub const ALL: [Tab; 6] = [
        Tab::Project,
        Tab::Model,
        Tab::Annotate,
        Tab::View,
        Tab::Sheets,
        Tab::Manage,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Tab::Project => "Project",
            Tab::Model => "Model",
            Tab::Annotate => "Annotate",
            Tab::View => "View",
            Tab::Sheets => "Sheets",
            Tab::Manage => "Manage",
        }
    }
}

/// Something the user can do from the ribbon or the palette.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    Open,
    /// Import an IFC file as an editable .bimble copy (ADR 0032).
    ImportIfc,
    Save,
    Undo,
    Redo,
    Place(edit::Kind),
    Measure,
    Edges,
    Hatching,
    Orthographic,
    SectionBox,
    BoxToSelection,
    Fit,
    Top,
    OnPaper,
    SectionLabels,
    Openings,
    SectionMarks,
    SlabLevels,
    NewSheet,
    CloseSheet,
    SheetTool(layout::Tool),
    LinkModel,
    ReferencePlan,
    Standards,
    Project,
    Materials,
    History,
    Settings,
    Palette,
}

/// Every action, in palette order.
pub(crate) const ALL_ACTIONS: [Action; 33] = [
    Action::Open,
    Action::ImportIfc,
    Action::Save,
    Action::Undo,
    Action::Redo,
    Action::Place(edit::Kind::Column),
    Action::Place(edit::Kind::Beam),
    Action::Place(edit::Kind::Wall),
    Action::Place(edit::Kind::Slab),
    Action::Measure,
    Action::SheetTool(layout::Tool::Select),
    Action::SheetTool(layout::Tool::Text),
    Action::SheetTool(layout::Tool::Dimension),
    Action::SectionLabels,
    Action::Openings,
    Action::SectionMarks,
    Action::SlabLevels,
    Action::Edges,
    Action::Hatching,
    Action::Orthographic,
    Action::SectionBox,
    Action::BoxToSelection,
    Action::Fit,
    Action::Top,
    Action::OnPaper,
    Action::NewSheet,
    Action::CloseSheet,
    Action::LinkModel,
    Action::ReferencePlan,
    Action::Standards,
    Action::Project,
    Action::Materials,
    Action::Settings,
];

impl Action {
    pub fn label(self) -> String {
        match self {
            Action::Open => "Open…".into(),
            Action::ImportIfc => "Import IFC…".into(),
            Action::Save => "Save".into(),
            Action::Undo => "Undo".into(),
            Action::Redo => "Redo".into(),
            Action::Place(k) => format!("Place: {}", k.label()),
            Action::Measure => "Measure".into(),
            Action::Edges => "Edges".into(),
            Action::Hatching => "Hatching".into(),
            Action::Orthographic => "Orthographic".into(),
            Action::SectionBox => "Section box".into(),
            Action::BoxToSelection => "Section box to selection".into(),
            Action::Fit => "Fit all".into(),
            Action::Top => "Top view".into(),
            Action::OnPaper => "On paper… (export the view)".into(),
            Action::SectionLabels => "Section labels on plans".into(),
            Action::Openings => "Opening sizes on plans".into(),
            Action::SectionMarks => "Section marks on plans".into(),
            Action::SlabLevels => "Slab levels on plans".into(),
            Action::NewSheet => "New sheet".into(),
            Action::CloseSheet => "Close the sheet".into(),
            Action::SheetTool(t) => match t {
                layout::Tool::Select => "Sheet: select".into(),
                layout::Tool::Text => "Sheet: text".into(),
                layout::Tool::Dimension => "Sheet: dimension".into(),
            },
            Action::LinkModel => "Link a model…".into(),
            Action::ReferencePlan => "Reference plan (DXF, PDF)…".into(),
            Action::Standards => "Graphic standards".into(),
            Action::Project => "Project information".into(),
            Action::Materials => "Materials".into(),
            Action::History => "History…".into(),
            Action::Settings => "Settings…".into(),
            Action::Palette => "Command palette".into(),
        }
    }

    /// Its keyboard shortcut, as shown.
    pub fn keys(self) -> &'static str {
        match self {
            Action::Save => "Ctrl S",
            Action::Undo => "Ctrl Z",
            Action::Redo => "Ctrl Y",
            Action::Measure => "M",
            Action::Edges => "E",
            Action::Hatching => "H",
            Action::Orthographic => "O",
            Action::SectionBox => "B",
            Action::Fit => "F",
            Action::Top => "T",
            Action::Palette => "Ctrl K",
            _ => "",
        }
    }

    /// Where it lives, as the palette shows it: its ribbon tab, the File menu or quick access.
    pub fn home(self) -> &'static str {
        let tab = match self {
            Action::Open | Action::ImportIfc | Action::Save => return "File",
            Action::Undo | Action::Redo | Action::Palette => return "Quick access",
            Action::Place(_) | Action::Measure => Tab::Model,
            Action::SheetTool(_)
            | Action::SectionLabels
            | Action::Openings
            | Action::SectionMarks
            | Action::SlabLevels => Tab::Annotate,
            Action::Edges
            | Action::Hatching
            | Action::Orthographic
            | Action::SectionBox
            | Action::BoxToSelection
            | Action::Fit
            | Action::Top => Tab::View,
            Action::OnPaper | Action::NewSheet | Action::CloseSheet => Tab::Sheets,
            Action::Project | Action::Settings | Action::LinkModel | Action::ReferencePlan => {
                Tab::Project
            }
            Action::Standards | Action::Materials | Action::History => Tab::Manage,
        };
        tab.label()
    }
}

/// A tab over the main view: a view, a sheet or a schedule.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum TabRef {
    View(GlobalId),
    Sheet(GlobalId),
    Schedule(String),
    Project,
    Materials,
}

/// What a palette entry does: run an action, or show a view, sheet or schedule.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum PaletteGo {
    Action(Action),
    Tab(TabRef),
}

/// The command palette's state: what is typed and which result is highlighted.
#[derive(Default)]
pub(crate) struct Palette {
    pub query: String,
    pub index: usize,
    /// Frames drawn: the click or key that opened it must not close it.
    pub frames: u32,
}

/// Whether every word of `query` starts a word of `label` (case-insensitive), or the
/// query is part of it: "sec box" finds "Section box", "dim" finds "Sheet: dimension".
pub(crate) fn matches(label: &str, query: &str) -> bool {
    let l = label.to_lowercase();
    let q = query.trim().to_lowercase();
    if q.is_empty() || l.contains(&q) {
        return true;
    }
    let words: Vec<&str> = l
        .split(|c: char| !c.is_alphanumeric())
        .filter(|w| !w.is_empty())
        .collect();
    q.split_whitespace()
        .all(|qw| words.iter().any(|w| w.starts_with(qw)))
}

impl Viewer {
    /// What Undo (or Redo) would do, in words (ADR 0028).
    pub(crate) fn undo_text(&self, undo: bool) -> Option<String> {
        let h = &self.loaded.as_ref()?.edit.as_ref()?.history;
        let l = if undo { h.undo_label() } else { h.redo_label() }?;
        Some(l.to_string())
    }

    fn editable(&self) -> bool {
        self.loaded.as_ref().is_some_and(|l| l.edit.is_some())
    }

    /// Whether an action can run now.
    pub(crate) fn action_enabled(&self, a: Action) -> bool {
        let loaded = self.loaded.is_some();
        let flat = self.in_2d();
        match a {
            Action::Open | Action::ImportIfc | Action::Palette | Action::Standards => true,
            Action::Save => {
                self.side_views_dirty()
                    || self
                        .loaded
                        .as_ref()
                        .and_then(|l| l.edit.as_ref())
                        .is_some_and(|e| e.dirty)
            }
            Action::Undo => self.undo_text(true).is_some(),
            Action::Redo => self.undo_text(false).is_some(),
            Action::Place(_) | Action::History => self.editable(),
            Action::Settings => true,
            Action::Orthographic | Action::SectionBox | Action::Top => loaded && !flat,
            Action::BoxToSelection => loaded && !flat && self.section_on,
            Action::SheetTool(_) | Action::CloseSheet => self.layout.is_some(),
            Action::ReferencePlan => loaded && flat,
            _ => loaded,
        }
    }

    /// Whether a toggle action is on (drawn pressed in the ribbon).
    pub(crate) fn action_on(&self, a: Action) -> bool {
        let t = &self.plan.template;
        match a {
            Action::Place(k) => self
                .loaded
                .as_ref()
                .and_then(|l| l.edit.as_ref())
                .and_then(|e| e.placing.as_ref())
                .is_some_and(|p| p.kind == k),
            Action::Measure => self.measure_on,
            Action::Edges => self.opts.edges,
            Action::Hatching => self.opts.hatching,
            Action::Orthographic => self.camera.orthographic,
            Action::SectionBox => self.section_on,
            Action::OnPaper => self.plan_window,
            Action::SectionLabels => t.element_labels,
            Action::Openings => t.opening_dimensions,
            Action::SectionMarks => t.section_markers,
            Action::SlabLevels => t.slab_levels,
            Action::SheetTool(tool) => self.layout.as_ref().is_some_and(|l| l.tool == tool),
            Action::Standards => self.standards_open,
            Action::Project => self.project_page.is_some(),
            Action::Materials => self.materials_page.is_some(),
            Action::History => self.history_window,
            Action::Settings => self.settings_window,
            _ => false,
        }
    }

    /// Run an action (from the ribbon, the palette or a key).
    pub(crate) fn run_action(&mut self, a: Action, ctx: &egui::Context) {
        if !self.action_enabled(a) {
            return;
        }
        match a {
            Action::Open => self.files.pick(files::Purpose::Open, ctx),
            Action::ImportIfc => self.files.pick(files::Purpose::ImportIfc, ctx),
            Action::Save => {
                if self.side_views.is_some() {
                    self.save_side_views();
                } else {
                    self.save();
                }
            }
            Action::Undo => self.undo_redo(true),
            Action::Redo => self.undo_redo(false),
            Action::Place(k) => {
                let on = self.action_on(a);
                if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
                    ed.placing = (!on).then_some(edit::Placing {
                        kind: k,
                        points: Vec::new(),
                    });
                    if !on {
                        ed.kind = k;
                    }
                }
                if !on {
                    self.measure_on = false;
                }
            }
            Action::Measure => {
                self.measure_on = !self.measure_on;
                self.measure_first = None;
                self.hover_snap = None;
            }
            Action::Edges => {
                self.opts.edges = !self.opts.edges;
                self.needs_render = true;
            }
            Action::Hatching => {
                self.opts.hatching = !self.opts.hatching;
                self.needs_render = true;
            }
            Action::Orthographic => {
                self.camera.orthographic = !self.camera.orthographic;
                self.needs_render = true;
            }
            Action::SectionBox => self.toggle_section_box(),
            Action::BoxToSelection => self.fit_section_box(),
            Action::Fit => {
                if let Some(l) = &mut self.layout {
                    l.fit_next();
                } else {
                    self.fit(self.last_aspect);
                }
            }
            Action::Top => {
                self.camera.pitch = 1.5;
                self.camera.yaw = -std::f32::consts::FRAC_PI_2;
                self.needs_render = true;
            }
            Action::OnPaper => self.plan_window = !self.plan_window,
            Action::SectionLabels => {
                self.plan.template.element_labels ^= true;
                self.plan.template_rev += 1;
            }
            Action::Openings => {
                self.plan.template.opening_dimensions ^= true;
                self.plan.template_rev += 1;
            }
            Action::SectionMarks => {
                self.plan.template.section_markers ^= true;
                self.plan.template_rev += 1;
            }
            Action::SlabLevels => {
                self.plan.template.slab_levels ^= true;
                self.plan.template_rev += 1;
            }
            Action::NewSheet => self.new_sheet(),
            Action::CloseSheet => self.close_sheet(),
            Action::SheetTool(t) => {
                if let Some(l) = &mut self.layout {
                    l.tool = t;
                }
            }
            Action::LinkModel => self.files.pick(files::Purpose::Link, ctx),
            Action::ReferencePlan => {
                self.reference_view = self.active_view;
                self.files.pick(files::Purpose::Reference, ctx);
            }
            Action::Standards => self.standards_open = !self.standards_open,
            Action::Project => {
                if self.project_page.is_some() {
                    self.close_pages();
                } else {
                    self.close_pages();
                    self.close_sheet();
                    self.project_page = Some(Default::default());
                }
            }
            Action::Materials => {
                if self.materials_page.is_some() {
                    self.close_pages();
                } else {
                    self.close_pages();
                    self.close_sheet();
                    self.materials_page = Some(Default::default());
                }
            }
            Action::Settings => self.settings_window = !self.settings_window,
            Action::History => {
                self.history_window = !self.history_window;
                if self.history_window {
                    self.refresh_history();
                }
            }
            Action::Palette => {
                self.palette = match self.palette {
                    Some(_) => None,
                    None => Some(Palette::default()),
                }
            }
        }
    }

    /// The top row (#51): the File menu and the ribbon's tabs on the left; the file's name,
    /// quick access as icons and the palette on the right.
    pub(crate) fn title_row(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        let mut run = None;
        ui.horizontal(|ui| {
            ui.label(egui::RichText::new("Bimble").strong());
            ui.add_space(th::GAP_M);
            self.file_menu(ui, &mut run);
            ui.add_space(th::GAP_M);
            self.ribbon_tabs(ui);
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let p = ui
                    .add(egui::Button::new(
                        egui::RichText::new("Command…  Ctrl K").color(th::TEXT_MUTED),
                    ))
                    .on_hover_text("Find and run any command");
                if p.clicked() {
                    run = Some(Action::Palette);
                }
                ui.add_space(th::GAP_M);
                for a in [Action::Redo, Action::Undo, Action::Save, Action::Open] {
                    let tip = self.quick_tip(a);
                    if self.icon_button(ui, a, &tip) {
                        run = Some(a);
                    }
                }
                ui.add_space(th::GAP_M);
                self.file_name(ui);
            });
        });
        if let Some(a) = run {
            self.run_action(a, &ctx);
        }
    }

    /// The open file's name and what isn't saved (ADR 0028), cut short when the row is full.
    fn file_name(&self, ui: &mut egui::Ui) {
        let Some(l) = &self.loaded else { return };
        let name = l
            .path
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let unsaved = crate::state::unsaved(&self.snapshot());
        if !unsaved.is_empty() {
            ui.add(
                egui::Label::new(
                    egui::RichText::new(format!("Not saved: {}", unsaved.join(", ")))
                        .color(ui.visuals().warn_fg_color),
                )
                .truncate(),
            )
            .on_hover_text(
                "Save (Ctrl S) saves the model or the views; the standards are saved \
                 from their panel",
            );
        }
        ui.add(egui::Label::new(egui::RichText::new(&name).color(th::TEXT_MUTED)).truncate())
            .on_hover_text(l.path.display().to_string());
    }

    /// What a quick-access button would do: Undo and Redo say what, Save says what is unsaved.
    fn quick_tip(&self, a: Action) -> String {
        match a {
            Action::Undo | Action::Redo => {
                let undo = a == Action::Undo;
                let verb = if undo { "Undo" } else { "Redo" };
                match self.undo_text(undo) {
                    Some(t) => format!("{verb}: {t} ({})", a.keys()),
                    None => format!("Nothing to {}", verb.to_lowercase()),
                }
            }
            Action::Save => {
                let st = self.snapshot();
                let mut t = if st.model_dirty {
                    "Save the model (Ctrl S)".to_string()
                } else if st.views_dirty {
                    "Save the views next to the IFC (Ctrl S)".to_string()
                } else {
                    "Save: the model and its views are saved".to_string()
                };
                if st.standards_changed {
                    t.push_str("\nThe standards are changed: Save… in their panel");
                }
                t
            }
            _ => a.label(),
        }
    }

    /// A small button showing an action's icon; the tip says what it does (and why not, greyed).
    fn icon_button(&self, ui: &mut egui::Ui, a: Action, tip: &str) -> bool {
        let enabled = self.action_enabled(a);
        let sense = if enabled {
            egui::Sense::click()
        } else {
            egui::Sense::hover()
        };
        let (rect, r) = ui.allocate_exact_size(egui::vec2(26.0, 26.0), sense);
        if enabled && r.hovered() {
            ui.painter().rect_filled(
                rect,
                th::RADIUS as f32,
                egui::Color32::from_rgb(0x35, 0x35, 0x35),
            );
        }
        let color = if enabled {
            th::TEXT
        } else {
            egui::Color32::from_gray(0x66)
        };
        crate::icons::paint(ui.painter(), rect.shrink(3.0), a, color);
        let r = r.on_hover_text(tip);
        enabled && r.clicked()
    }

    /// The File menu (#51): what is done to files, then the project's settings; Exit on the
    /// desktop. Every entry is an action, run like the ribbon's.
    fn file_menu(&self, ui: &mut egui::Ui, run: &mut Option<Action>) {
        ui.menu_button("File", |ui| {
            let groups: &[&[Action]] = &[
                &[Action::Open, Action::ImportIfc, Action::Save],
                &[Action::LinkModel, Action::ReferencePlan],
                &[Action::OnPaper],
                &[Action::Project, Action::Settings],
            ];
            for (i, g) in groups.iter().enumerate() {
                if i > 0 {
                    ui.separator();
                }
                for a in *g {
                    let b = egui::Button::new(a.label()).shortcut_text(a.keys());
                    if ui.add_enabled(self.action_enabled(*a), b).clicked() {
                        *run = Some(*a);
                    }
                }
            }
            if cfg!(not(target_arch = "wasm32")) {
                if ui.button(Action::History.label()).clicked() {
                    *run = Some(Action::History);
                }
                ui.separator();
                if ui.button("Exit").clicked() {
                    ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
                }
            }
        });
    }

    /// The ribbon's tab strip.
    pub(crate) fn ribbon_tabs(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;
            for t in Tab::ALL {
                let on = self.ribbon_tab == t;
                let text = egui::RichText::new(t.label()).color(if on {
                    th::TEXT
                } else {
                    th::TEXT_MUTED
                });
                let r = ui.add(
                    egui::Button::new(text)
                        .fill(if on { th::RIBBON_BG } else { th::TABS_BG })
                        .stroke(egui::Stroke::NONE)
                        .corner_radius(egui::CornerRadius {
                            nw: th::RADIUS,
                            ne: th::RADIUS,
                            sw: 0,
                            se: 0,
                        })
                        .min_size(egui::vec2(72.0, 26.0)),
                );
                if on {
                    let rr = r.rect;
                    ui.painter().line_segment(
                        [rr.left_top(), rr.right_top()],
                        egui::Stroke::new(2.0, th::ACCENT),
                    );
                }
                if r.clicked() {
                    self.ribbon_tab = t;
                }
            }
        });
    }

    /// One ribbon button, its icon above its name: pressed when its action is on, greyed when it
    /// can't run.
    fn ribbon_button(&self, ui: &mut egui::Ui, a: Action, text: &str) -> bool {
        let on = self.action_on(a);
        let enabled = self.action_enabled(a);
        let galley = ui.painter().layout_no_wrap(
            text.to_string(),
            egui::FontId::proportional(th::TEXT_SMALL + 0.5),
            th::TEXT,
        );
        let w = (galley.size().x + 12.0).max(56.0);
        let sense = if enabled {
            egui::Sense::click()
        } else {
            egui::Sense::hover()
        };
        let (rect, mut r) = ui.allocate_exact_size(egui::vec2(w, 50.0), sense);
        let fill = if on {
            th::SELECTED_BG
        } else if enabled && r.hovered() {
            egui::Color32::from_rgb(0x35, 0x35, 0x35)
        } else {
            egui::Color32::TRANSPARENT
        };
        let p = ui.painter();
        p.rect_filled(rect, th::RADIUS as f32, fill);
        if on {
            p.rect_stroke(
                rect,
                th::RADIUS as f32,
                egui::Stroke::new(1.0, th::ACCENT),
                egui::StrokeKind::Inside,
            );
        }
        let color = if enabled {
            th::TEXT
        } else {
            egui::Color32::from_gray(0x66)
        };
        let icon = egui::Rect::from_center_size(
            egui::pos2(rect.center().x, rect.top() + 16.0),
            egui::vec2(22.0, 22.0),
        );
        crate::icons::paint(p, icon, a, color);
        p.galley(
            egui::pos2(
                rect.center().x - galley.size().x / 2.0,
                rect.bottom() - 17.0,
            ),
            galley,
            color,
        );
        let tip = match a.keys() {
            "" => a.label(),
            k => format!("{} ({k})", a.label()),
        };
        r = r.on_hover_text(tip);
        enabled && r.clicked()
    }

    /// A named group of the ribbon.
    fn group(
        &self,
        ui: &mut egui::Ui,
        name: &str,
        items: &[(Action, &str)],
        run: &mut Option<Action>,
    ) {
        ui.vertical(|ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                for (a, t) in items {
                    if self.ribbon_button(ui, *a, t) {
                        *run = Some(*a);
                    }
                }
            });
            ui.label(
                egui::RichText::new(name)
                    .size(th::TEXT_SMALL)
                    .color(th::TEXT_MUTED),
            );
        });
        ui.separator();
    }

    /// The tools of the open ribbon tab.
    pub(crate) fn ribbon_panel(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        let mut run = None;
        ui.horizontal(|ui| match self.ribbon_tab {
            Tab::Project => {
                self.group(
                    ui,
                    "Project",
                    &[(Action::Project, "Project information")],
                    &mut run,
                );
                self.group(ui, "Settings", &[(Action::Settings, "Settings")], &mut run);
                self.group(
                    ui,
                    "Links",
                    &[
                        (Action::LinkModel, "Model…"),
                        (Action::ReferencePlan, "Reference plan…"),
                    ],
                    &mut run,
                );
                ui.weak(format!(
                    "{} linked model(s): display and differences under Links, left",
                    self.links.len()
                ));
            }
            Tab::Model => {
                let k = |k: edit::Kind| (Action::Place(k), k.label());
                self.group(
                    ui,
                    "Place",
                    &[
                        k(edit::Kind::Column),
                        k(edit::Kind::Beam),
                        k(edit::Kind::Wall),
                        k(edit::Kind::Slab),
                    ],
                    &mut run,
                );
                self.group(ui, "Inspect", &[(Action::Measure, "Measure")], &mut run);
                if !self.editable() && self.loaded.is_some() {
                    ui.weak("An IFC is read-only: open a .bimble file to place elements.");
                }
            }
            Tab::Annotate => {
                self.group(
                    ui,
                    "On the sheet",
                    &[
                        (Action::SheetTool(layout::Tool::Select), "Select"),
                        (Action::SheetTool(layout::Tool::Text), "Text"),
                        (Action::SheetTool(layout::Tool::Dimension), "Dimension"),
                    ],
                    &mut run,
                );
                self.annotation_types(ui);
                ui.separator();
                self.group(
                    ui,
                    "On plans",
                    &[
                        (Action::SectionLabels, "Section labels"),
                        (Action::Openings, "Openings"),
                        (Action::SectionMarks, "Section marks"),
                        (Action::SlabLevels, "Levels"),
                    ],
                    &mut run,
                );
            }
            Tab::View => {
                self.group(
                    ui,
                    "Show",
                    &[
                        (Action::Edges, "Edges"),
                        (Action::Hatching, "Hatching"),
                        (Action::Orthographic, "Orthographic"),
                    ],
                    &mut run,
                );
                self.group(
                    ui,
                    "Cut",
                    &[
                        (Action::SectionBox, "Section box"),
                        (Action::BoxToSelection, "Box to selection"),
                    ],
                    &mut run,
                );
                self.group(
                    ui,
                    "Camera",
                    &[(Action::Fit, "Fit all"), (Action::Top, "Top")],
                    &mut run,
                );
            }
            Tab::Sheets => {
                self.group(
                    ui,
                    "Sheets",
                    &[
                        (Action::NewSheet, "New sheet"),
                        (Action::CloseSheet, "Close sheet"),
                    ],
                    &mut run,
                );
                self.group(ui, "Export", &[(Action::OnPaper, "On paper…")], &mut run);
            }
            Tab::Manage => {
                self.group(
                    ui,
                    "Standards",
                    &[(Action::Standards, "Graphic standards")],
                    &mut run,
                );
                self.group(ui, "Library", &[(Action::Materials, "Materials")], &mut run);
                if cfg!(not(target_arch = "wasm32")) {
                    self.group(ui, "Versions", &[(Action::History, "History")], &mut run);
                }
            }
        });
        if let Some(a) = run {
            self.run_action(a, &ctx);
        }
    }

    /// The text and dimension types new annotations get (#43), in the Annotate tab.
    fn annotation_types(&mut self, ui: &mut egui::Ui) {
        let paper = &self.opts.style.paper;
        let texts: Vec<String> = paper.text_types.iter().map(|t| t.name.clone()).collect();
        let dims: Vec<String> = paper
            .dimension_types
            .iter()
            .map(|t| t.name.clone())
            .collect();
        let Some(l) = &mut self.layout else {
            ui.weak("Open a sheet to add texts and dimensions.");
            return;
        };
        ui.vertical(|ui| {
            for (label, list, cur) in [
                ("Dimension type", &dims, &mut l.dim_kind),
                ("Text type", &texts, &mut l.text_kind),
            ] {
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new(label).color(th::TEXT_MUTED));
                    egui::ComboBox::from_id_salt(("ribbon-kind", label))
                        .selected_text(cur.clone().unwrap_or_else(|| "Default".into()))
                        .height(300.0)
                        .show_ui(ui, |ui| {
                            ui.selectable_value(cur, None, "Default");
                            for n in list {
                                ui.selectable_value(cur, Some(n.clone()), n);
                            }
                        });
                });
            }
        });
    }

    /// Tabs of the open views, sheets and schedules, over the main view.
    pub(crate) fn view_tabs(&mut self, ui: &mut egui::Ui) {
        // What the main view shows always has its tab.
        let current = if self.materials_page.is_some() {
            Some(TabRef::Materials)
        } else if self.project_page.is_some() {
            Some(TabRef::Project)
        } else if let Some(s) = &self.schedule {
            Some(TabRef::Schedule(s.name.clone()))
        } else if let Some(l) = &self.layout {
            Some(TabRef::Sheet(l.sheet))
        } else {
            self.active_view.map(TabRef::View)
        };
        if let Some(c) = &current
            && !self.open_tabs.contains(c)
        {
            self.open_tabs.push(c.clone());
        }
        let sheets = self.sheets();
        let schedules = self.schedule_defs();
        self.open_tabs.retain(|t| match t {
            TabRef::View(id) => self.views.iter().any(|v| v.id == *id),
            TabRef::Sheet(id) => sheets.iter().any(|s| s.id == *id),
            TabRef::Schedule(n) => schedules.iter().any(|d| d.name == *n),
            TabRef::Project | TabRef::Materials => true,
        });
        let mut go: Option<TabRef> = None;
        let mut close: Option<TabRef> = None;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;
            for (k, t) in self.open_tabs.clone().into_iter().enumerate() {
                let name = match &t {
                    TabRef::Sheet(id) => sheets
                        .iter()
                        .find(|s| s.id == *id)
                        .map(|s| format!("{} · {}", s.number, s.name))
                        .unwrap_or_default(),
                    TabRef::View(id) => self
                        .views
                        .iter()
                        .find(|v| v.id == *id)
                        .map(|v| v.name.clone())
                        .unwrap_or_default(),
                    TabRef::Schedule(n) => format!("Schedule · {n}"),
                    TabRef::Project => "Project information".to_string(),
                    TabRef::Materials => "Materials".to_string(),
                };
                let on = current.as_ref() == Some(&t);
                let fill = if on { th::CANVAS } else { th::RIBBON_BG };
                let color = if on {
                    egui::Color32::from_rgb(0x1a, 0x1a, 0x1a)
                } else {
                    th::TEXT
                };
                ui.push_id(k, |ui| {
                    let r = ui.add(
                        egui::Button::new(egui::RichText::new(&name).color(color))
                            .fill(fill)
                            .stroke(egui::Stroke::NONE)
                            .corner_radius(egui::CornerRadius {
                                nw: th::RADIUS,
                                ne: th::RADIUS,
                                sw: 0,
                                se: 0,
                            }),
                    );
                    if r.clicked() {
                        go = Some(t.clone());
                    }
                    if r.middle_clicked() {
                        close = Some(t.clone());
                    }
                    if self.open_tabs.len() > 1
                        && ui
                            .small_button("×")
                            .on_hover_text("Close the tab")
                            .clicked()
                    {
                        close = Some(t.clone());
                    }
                });
            }
        });
        if let Some(t) = close {
            self.open_tabs.retain(|x| *x != t);
            if current.as_ref() == Some(&t) {
                go = self.open_tabs.last().cloned();
                if go.is_none() {
                    self.close_pages();
                    self.close_sheet();
                }
            }
        }
        if let Some(t) = go {
            self.go_to(t);
        }
    }

    /// Show what a tab refers to in the main view: a view, a sheet, a schedule, a page.
    pub(crate) fn go_to(&mut self, t: TabRef) {
        match t {
            TabRef::Sheet(id) => self.open_sheet(id),
            TabRef::View(id) => {
                self.close_pages();
                self.close_sheet();
                self.activate_view(id);
            }
            TabRef::Schedule(n) => self.open_schedule(&n),
            TabRef::Project => {
                if self.project_page.is_none() {
                    self.close_pages();
                    self.close_sheet();
                    self.project_page = Some(Default::default());
                }
            }
            TabRef::Materials => {
                if self.materials_page.is_none() {
                    self.close_pages();
                    self.close_sheet();
                    self.materials_page = Some(Default::default());
                }
            }
        }
    }

    /// What the status bar says the pointer will do.
    pub(crate) fn status_hint(&self) -> String {
        if let Some(p) = self
            .loaded
            .as_ref()
            .and_then(|l| l.edit.as_ref())
            .and_then(|e| e.placing.as_ref())
        {
            return format!(
                "Place {}: click {}; snaps to axes, centres, corners (Shift: free) · Esc stops",
                p.kind.label().to_lowercase(),
                if p.kind.clicks() == 1 {
                    "its centre"
                } else if p.points.is_empty() {
                    "the start"
                } else {
                    "the end"
                }
            );
        }
        if self.materials_page.is_some() {
            return "Materials: choose one on the left; Add takes a grade from the library".into();
        }
        if self.project_page.is_some() {
            return "Project information: a value is saved when you leave its field".into();
        }
        if self.schedule.is_some() {
            return "Schedule: click a row to select its element · a column's title to sort by it"
                .into();
        }
        if let Some(l) = &self.layout {
            return match l.tool {
                layout::Tool::Select => {
                    "Click: select · drag a selected view to move it · double-click: open the view"
                        .into()
                }
                layout::Tool::Text => "Click where the text goes".into(),
                layout::Tool::Dimension => {
                    "Click two points, then where the dimension line goes".into()
                }
            };
        }
        if self.measure_on {
            return "Measure: click two points (corners, edges, faces) · Esc drops the first"
                .into();
        }
        if self.in_2d() {
            "Drag: pan · Scroll: zoom · Click: select · F: fit · Ctrl K: commands".into()
        } else {
            "Drag: orbit · Right-drag: pan · Scroll: zoom · Click: select · F: fit · T: top · Ctrl K: commands"
                .into()
        }
    }

    /// The command palette, over everything.
    /// Everything the palette can find: the actions, then the views, sheets and schedules to go
    /// to. Each with its label, where it is (tab and key, or what it is), and whether it can run.
    fn palette_entries(&mut self) -> Vec<(String, String, bool, PaletteGo)> {
        let mut v: Vec<(String, String, bool, PaletteGo)> = ALL_ACTIONS
            .iter()
            .chain([Action::History].iter())
            .map(|a| {
                let where_ = match a.keys() {
                    "" => a.home().to_string(),
                    k => format!("{} · {k}", a.home()),
                };
                (
                    a.label(),
                    where_,
                    self.action_enabled(*a),
                    PaletteGo::Action(*a),
                )
            })
            .collect();
        for view in &self.views {
            v.push((
                format!("Go to {}", view.name),
                "View".into(),
                true,
                PaletteGo::Tab(TabRef::View(view.id)),
            ));
        }
        for sh in self.sheets() {
            v.push((
                format!("Go to sheet {} {}", sh.number, sh.name),
                "Sheet".into(),
                true,
                PaletteGo::Tab(TabRef::Sheet(sh.id)),
            ));
        }
        for (name, n) in self.schedule_counts() {
            v.push((
                format!("Schedule: {name}"),
                format!("{n} elements"),
                n > 0,
                PaletteGo::Tab(TabRef::Schedule(name)),
            ));
        }
        v
    }

    /// The command palette, over everything.
    pub(crate) fn palette_ui(&mut self, ctx: &egui::Context) {
        if self.palette.is_none() {
            return;
        }
        let entries = self.palette_entries();
        let Some(p) = &mut self.palette else { return };
        let found: Vec<(String, String, bool, PaletteGo)> = entries
            .into_iter()
            .filter(|e| matches(&e.0, &p.query))
            .collect();
        let (up, down, enter, esc) = ctx.input(|i| {
            (
                i.key_pressed(egui::Key::ArrowUp),
                i.key_pressed(egui::Key::ArrowDown),
                i.key_pressed(egui::Key::Enter),
                i.key_pressed(egui::Key::Escape),
            )
        });
        if down && p.index + 1 < found.len() {
            p.index += 1;
        }
        if up {
            p.index = p.index.saturating_sub(1);
        }
        p.index = p.index.min(found.len().saturating_sub(1));
        let mut run: Option<PaletteGo> = None;
        let mut close = esc;
        let p = self.palette.as_mut().expect("checked above");
        let mut lost = false;
        let area = egui::Area::new(egui::Id::unique("command-palette"))
            .order(egui::Order::Foreground)
            .anchor(egui::Align2::CENTER_TOP, egui::vec2(0.0, 90.0))
            .show(ctx, |ui| {
                egui::Frame::popup(ui.style())
                    .inner_margin(egui::Margin::same(th::GAP_M as i8))
                    .show(ui, |ui| {
                        ui.set_width(480.0);
                        let r = ui.add(
                            egui::TextEdit::singleline(&mut p.query)
                                .hint_text("A command, a view, a sheet or a schedule")
                                .desired_width(f32::INFINITY),
                        );
                        // Esc in the field gives its focus up: that closes the palette.
                        lost = r.lost_focus();
                        r.request_focus();
                        if r.changed() {
                            p.index = 0;
                        }
                        ui.add_space(th::GAP_S);
                        egui::ScrollArea::vertical()
                            .max_height(320.0)
                            .show(ui, |ui| {
                                for (k, (label, where_, enabled, go)) in found.iter().enumerate() {
                                    let on = k == p.index;
                                    let text = egui::RichText::new(label).color(if *enabled {
                                        th::TEXT
                                    } else {
                                        th::TEXT_MUTED
                                    });
                                    let row = ui.add(
                                        egui::Button::selectable(on, text)
                                            .shortcut_text(where_.as_str())
                                            .min_size(egui::vec2(ui.available_width(), 24.0)),
                                    );
                                    if on {
                                        row.scroll_to_me(None);
                                    }
                                    // A greyed command can't run now: the palette stays open.
                                    if row.clicked() && *enabled {
                                        run = Some(go.clone());
                                    }
                                }
                                if found.is_empty() {
                                    ui.weak("No command matches.");
                                }
                            });
                    });
            });
        // A click outside closes it too.
        let outside = ctx.input(|i| {
            i.pointer.any_pressed()
                && i.pointer
                    .interact_pos()
                    .is_some_and(|q| !area.response.rect.contains(q))
        });
        let settled = p.frames >= 2;
        close |= settled && (outside || (lost && !enter));
        if enter
            && let Some(e) = found.get(p.index)
            && e.2
        {
            run = Some(e.3.clone());
        }
        if let Some(p) = &mut self.palette {
            p.frames += 1;
        }
        if let Some(go) = run {
            close = true;
            self.palette = None;
            match go {
                PaletteGo::Action(a) => self.run_action(a, ctx),
                PaletteGo::Tab(t) => self.go_to(t),
            }
        }
        if close {
            self.palette = None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn palette_matches_word_starts_and_parts() {
        assert!(matches("Section box", "sec box"));
        assert!(matches("Sheet: dimension", "dim"));
        assert!(matches("Place: Column", "col"));
        assert!(matches("Anything", ""));
        assert!(!matches("Section box", "beam"));
        assert!(!matches("Place: Column", "col wall"));
    }

    /// Every action has a label, and the palette lists each one once.
    #[test]
    fn actions_are_listed_once() {
        let mut labels: Vec<String> = ALL_ACTIONS.iter().map(|a| a.label()).collect();
        labels.push(Action::History.label());
        let n = labels.len();
        labels.sort();
        labels.dedup();
        assert_eq!(labels.len(), n);
        assert!(labels.iter().all(|l| !l.is_empty()));
    }
}
