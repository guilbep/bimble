//! Layouts in the main view (#34): a sheet drawn from the same lines as its PDF, at any zoom;
//! views dragged onto it, moved, resized by scale, removed; texts and dimensions added on it.
//! Texts inside a placed view and all dimensions belong to the view (ADR 0024); other texts to
//! the sheet. Changes go through Commands (.bimble) or the side file (IFC).

use super::*;
use bimble_core::GlobalId;
use bimble_core::sheet::{Placed, Sheet as SheetDef, SheetText, next_number};
use bimble_core::view::{Anchor as VAnchor, Annotation, View};
use bimble_drawing::sheetdraw::{PlacedInfo, SheetInfo, SheetOptions};

/// What a layout tool does with a click.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub(crate) enum Tool {
    #[default]
    Select,
    Text,
    Dimension,
}

/// What is selected on a sheet.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Picked {
    Placed(GlobalId),
    SheetText(GlobalId),
    /// An annotation of a view: the view, the annotation.
    Annotation(GlobalId, GlobalId),
}

/// A change to a view, applied after the frame's drawing.
type ViewEdit = Box<dyn FnOnce(&mut View)>;

/// A view dragged from the navigator onto a sheet.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ViewDrag(pub GlobalId);

/// The open sheet: its drawing, the camera on it, the tool and what is selected.
impl Layout {
    /// Fit the sheet in the view at the next frame.
    pub fn fit_next(&mut self) {
        self.fit = true;
    }
}

pub(crate) struct Layout {
    pub sheet: GlobalId,
    key: u64,
    prepared: Option<bimble_drawing::sheet::Prepared>,
    info: SheetInfo,
    error: Option<String>,
    /// Paper mm at the centre of the view, and screen points per paper mm.
    center: [f64; 2],
    zoom: f64,
    fit: bool,
    texture: Option<(egui::TextureHandle, u64)>,
    pub tool: Tool,
    pub picked: Option<Picked>,
    /// A placed view being dragged: its id and the drag so far, paper mm.
    moving: Option<(GlobalId, [f64; 2])>,
    /// Dimension points so far: the placed view and the points, paper mm.
    dim: Option<(GlobalId, Vec<[f64; 2]>)>,
    hover: Option<[f64; 2]>,
    /// The text being edited in the properties, and whether it changed.
    text_form: Option<(Picked, String, f64)>,
    pub last_render_ms: f64,
    /// The text type and dimension type new texts and dimensions get (#43).
    pub text_kind: Option<String>,
    pub dim_kind: Option<String>,
}

fn hash_of(parts: &[&[u8]]) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    for p in parts {
        p.hash(&mut h);
    }
    h.finish()
}

impl Viewer {
    /// Open a sheet in the main view.
    pub(crate) fn open_sheet(&mut self, id: GlobalId) {
        if self.layout.as_ref().is_some_and(|l| l.sheet == id) {
            return;
        }
        self.layout = Some(Layout {
            sheet: id,
            key: 0,
            prepared: None,
            info: SheetInfo::default(),
            error: None,
            center: [0.0, 0.0],
            zoom: 1.0,
            fit: true,
            texture: None,
            tool: Tool::Select,
            picked: None,
            moving: None,
            dim: None,
            hover: None,
            text_form: None,
            last_render_ms: 0.0,
            text_kind: None,
            dim_kind: None,
        });
        self.selected = None;
        self.needs_upload = true;
    }

    pub(crate) fn close_sheet(&mut self) {
        self.layout = None;
    }

    fn sheet_def(&self, id: GlobalId) -> Option<SheetDef> {
        self.sheets().into_iter().find(|s| s.id == id)
    }

    /// Store a sheet: a Command in a .bimble model (undoable), the side file for IFC.
    pub(crate) fn store_sheet(&mut self, s: SheetDef) -> Result<(), String> {
        let ids: BTreeSet<GlobalId> = self.views.iter().map(|v| v.id).collect();
        s.validate(|v| ids.contains(&v))?;
        let Some(l) = self.loaded.as_mut() else {
            return Ok(());
        };
        match (&mut l.edit, &mut self.side_views) {
            (Some(ed), _) => {
                let cmd = if ed.model.sheets.contains_key(&s.id) {
                    bimble_core::Command::SetSheet(s)
                } else {
                    bimble_core::Command::AddSheet(s)
                };
                ed.apply(vec![cmd])?;
            }
            (None, Some(sv)) => {
                if sv
                    .sheets
                    .values()
                    .any(|o| o.id != s.id && o.number == s.number)
                {
                    return Err(format!("sheet number {} is already used", s.number));
                }
                sv.sheets.insert(s.id, bimble_core::version::canonical(s));
                sv.dirty = true;
            }
            (None, None) => return Err("no model".into()),
        }
        Ok(())
    }

    pub(crate) fn delete_sheet(&mut self, id: GlobalId) {
        let Some(l) = self.loaded.as_mut() else {
            return;
        };
        let r = match (&mut l.edit, &mut self.side_views) {
            (Some(ed), _) => ed.apply(vec![bimble_core::Command::RemoveSheet { id }]),
            (None, Some(sv)) => {
                sv.sheets.remove(&id);
                sv.dirty = true;
                Ok(())
            }
            (None, None) => Ok(()),
        };
        if let Err(e) = r {
            self.view_message = Some(e);
        }
        if self.layout.as_ref().is_some_and(|l| l.sheet == id) {
            self.layout = None;
        }
    }

    pub(crate) fn new_sheet(&mut self) {
        let sheets = self.sheets();
        let number = next_number(sheets.iter().map(|s| s.number.as_str()));
        let s = SheetDef::new(number, "New sheet");
        let id = s.id;
        match self.store_sheet(s) {
            Ok(()) => self.open_sheet(id),
            Err(e) => self.view_message = Some(e),
        }
    }

    /// Store a view after changing its annotations.
    fn update_view(&mut self, id: GlobalId, f: impl FnOnce(&mut View)) {
        let Some(mut v) = self.views.iter().find(|v| v.id == id).cloned() else {
            return;
        };
        f(&mut v);
        if let Err(e) = self.store_view(v) {
            self.view_message = Some(e);
        }
    }

    /// Redraw the sheet when it, its views, the model or the style changed.
    fn refresh_layout(&mut self) {
        let Some(lay) = &self.layout else { return };
        let Some(def) = self.sheet_def(lay.sheet) else {
            self.layout = None;
            return;
        };
        let Some(l) = &self.loaded else { return };
        let used: Vec<&View> = self
            .views
            .iter()
            .filter(|v| def.placed.iter().any(|p| p.view == v.id))
            .collect();
        let key = hash_of(&[
            serde_json::to_string(&def).unwrap_or_default().as_bytes(),
            serde_json::to_string(&used).unwrap_or_default().as_bytes(),
            serde_json::to_string(&self.opts.style)
                .unwrap_or_default()
                .as_bytes(),
            &self.scene_rev.to_le_bytes(),
            &self.plan.template_rev.to_le_bytes(),
        ]);
        if key == lay.key {
            return;
        }
        let o = SheetOptions {
            style: self.opts.style.clone(),
            template: self.plan.template.clone(),
            project: l
                .path
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default(),
            date: bimble_drawing::today(),
        };
        let drawn = bimble_drawing::sheetdraw::draw(&l.scene, &def, &self.views, &o);
        let lay = self.layout.as_mut().expect("checked above");
        lay.key = key;
        lay.texture = None;
        match drawn.and_then(|(sheet, info)| Ok((sheet.prepare()?, info))) {
            Ok((p, info)) => {
                lay.prepared = Some(p);
                lay.info = info;
                lay.error = None;
            }
            Err(e) => {
                lay.prepared = None;
                lay.error = Some(e);
            }
        }
    }

    /// The sheet in the main view, and what is done on it.
    pub(crate) fn layout_ui(&mut self, ui: &mut egui::Ui, rect: egui::Rect) {
        self.refresh_layout();
        let Some(def) = self.layout.as_ref().and_then(|l| self.sheet_def(l.sheet)) else {
            return;
        };
        let resp = ui.allocate_rect(rect, Sense::click_and_drag());
        let ppp = ui.ctx().pixels_per_point() as f64;
        let lay = self.layout.as_mut().expect("refreshed");
        let (w, h) = def.size_mm().unwrap_or((841.0, 594.0));
        if lay.fit {
            lay.center = [w / 2.0, h / 2.0];
            lay.zoom = (rect.width() as f64 / (w * 1.05)).min(rect.height() as f64 / (h * 1.05));
            lay.fit = false;
        }
        // Screen points ↔ paper mm.
        let (c, z) = (lay.center, lay.zoom);
        let to_paper = move |p: egui::Pos2| {
            [
                c[0] + (p.x - rect.center().x) as f64 / z,
                c[1] + (p.y - rect.center().y) as f64 / z,
            ]
        };
        let to_screen = move |p: [f64; 2]| {
            egui::pos2(
                rect.center().x + ((p[0] - c[0]) * z) as f32,
                rect.center().y + ((p[1] - c[1]) * z) as f32,
            )
        };

        // The picture of the part of the sheet in view.
        let (wpx, hpx) = (
            (rect.width() as f64 * ppp).round() as u32,
            (rect.height() as f64 * ppp).round() as u32,
        );
        let tkey = hash_of(&[
            &lay.key.to_le_bytes(),
            &c[0].to_bits().to_le_bytes(),
            &c[1].to_bits().to_le_bytes(),
            &z.to_bits().to_le_bytes(),
            &wpx.to_le_bytes(),
            &hpx.to_le_bytes(),
        ]);
        if lay.texture.as_ref().is_none_or(|t| t.1 != tkey)
            && let Some(p) = &lay.prepared
        {
            let t0 = Instant::now();
            let origin = to_paper(rect.min);
            match p.render(z * ppp, origin, wpx, hpx) {
                Ok(r) => {
                    let img = egui::ColorImage::from_rgba_unmultiplied(
                        [r.width as usize, r.height as usize],
                        &r.rgba,
                    );
                    let tex = ui
                        .ctx()
                        .load_texture("layout", img, egui::TextureOptions::LINEAR);
                    lay.texture = Some((tex, tkey));
                }
                Err(e) => lay.error = Some(e),
            }
            lay.last_render_ms = t0.elapsed().as_secs_f64() * 1000.0;
        }
        let painter = ui.painter_at(rect);
        painter.rect_filled(rect, 0.0, Color32::from_gray(200));
        if let Some((t, _)) = &lay.texture {
            painter.image(
                t.id(),
                rect,
                egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
                Color32::WHITE,
            );
        }
        if let Some(e) = &lay.error {
            painter.text(
                rect.left_top() + egui::vec2(10.0, 10.0),
                egui::Align2::LEFT_TOP,
                e,
                egui::FontId::proportional(14.0),
                Color32::RED,
            );
        }

        // ---- navigation: scroll zooms at the pointer, middle/right drag pans, primary drag pans
        // unless it moves a view.
        if resp.hovered() {
            let (scroll, pinch) = ui.input(|i| (i.smooth_scroll_delta().y, i.zoom_delta()));
            if (scroll != 0.0 || pinch != 1.0)
                && let Some(p) = resp.hover_pos()
            {
                let before = to_paper(p);
                let f = (scroll as f64 * 0.0015).exp() * pinch as f64;
                lay.zoom = (lay.zoom * f).clamp(0.05, 200.0);
                // Keep the point under the pointer.
                lay.center = [
                    before[0] - (p.x - rect.center().x) as f64 / lay.zoom,
                    before[1] - (p.y - rect.center().y) as f64 / lay.zoom,
                ];
            }
        }
        let pan = |lay: &mut Layout, d: egui::Vec2| {
            lay.center[0] -= d.x as f64 / lay.zoom;
            lay.center[1] -= d.y as f64 / lay.zoom;
        };
        if resp.dragged_by(PointerButton::Middle) || resp.dragged_by(PointerButton::Secondary) {
            pan(lay, resp.drag_delta());
        }

        let accent = {
            let a = self.opts.style.selection.0;
            Color32::from_rgb(a[0], a[1], a[2])
        };
        let pointer = resp.hover_pos().or(resp.interact_pointer_pos());
        lay.hover = pointer.map(to_paper);
        let placed_at = |info: &SheetInfo, p: [f64; 2]| -> Option<PlacedInfo> {
            info.placed
                .iter()
                .rev()
                .find(|pi| {
                    p[0] >= pi.rect[0]
                        && p[0] <= pi.rect[2]
                        && p[1] >= pi.rect[1]
                        && p[1] <= pi.rect[3]
                })
                .cloned()
        };

        // ---- a view dropped from the navigator: placed at 1:100 under the pointer.
        let mut store: Option<SheetDef> = None;
        let mut view_change: Option<(GlobalId, ViewEdit)> = None;
        let mut open_view: Option<GlobalId> = None;
        if let Some(d) = resp.dnd_release_payload::<ViewDrag>()
            && let Some(p) = pointer
        {
            let at = to_paper(p);
            let mut s = def.clone();
            let id = GlobalId::new();
            s.placed.push(Placed {
                id,
                view: d.0,
                scale: 100,
                at_mm: [at[0].clamp(0.0, w), at[1].clamp(0.0, h)],
            });
            lay.picked = Some(Picked::Placed(id));
            store = Some(s);
        }

        // ---- tools
        match lay.tool {
            Tool::Select => {
                // Drag a selected view; else drag pans.
                if resp.drag_started_by(PointerButton::Primary)
                    && let Some(p) = pointer
                    && let Some(pi) = placed_at(&lay.info, to_paper(p))
                    && lay.picked == Some(Picked::Placed(pi.id))
                {
                    lay.moving = Some((pi.id, [0.0, 0.0]));
                }
                if resp.dragged_by(PointerButton::Primary) {
                    let d = resp.drag_delta();
                    match &mut lay.moving {
                        Some((_, m)) => {
                            m[0] += d.x as f64 / lay.zoom;
                            m[1] += d.y as f64 / lay.zoom;
                        }
                        None => pan(lay, d),
                    }
                }
                if resp.drag_stopped()
                    && let Some((id, m)) = lay.moving.take()
                {
                    let mut s = def.clone();
                    if let Some(p) = s.placed.iter_mut().find(|p| p.id == id) {
                        p.at_mm = [
                            (p.at_mm[0] + m[0]).clamp(0.0, w),
                            (p.at_mm[1] + m[1]).clamp(0.0, h),
                        ];
                    }
                    store = Some(s);
                }
                if resp.double_clicked()
                    && let Some(p) = pointer
                    && let Some(pi) = placed_at(&lay.info, to_paper(p))
                {
                    open_view = Some(pi.view);
                } else if resp.clicked()
                    && let Some(p) = pointer
                {
                    let at = to_paper(p);
                    let tol = 3.0 / lay.zoom;
                    lay.picked = pick_text(&def, at, tol)
                        .or_else(|| {
                            placed_at(&lay.info, at).and_then(|pi| {
                                let v = self.views.iter().find(|v| v.id == pi.view)?;
                                pick_annotation(v, &pi, at, tol)
                                    .map(|a| Picked::Annotation(pi.view, a))
                            })
                        })
                        .or_else(|| placed_at(&lay.info, at).map(|pi| Picked::Placed(pi.id)));
                    lay.text_form = None;
                }
            }
            Tool::Text => {
                if resp.clicked()
                    && let Some(p) = pointer
                {
                    let at = to_paper(p);
                    let id = GlobalId::new();
                    match placed_at(&lay.info, at) {
                        // In a view: the view's.
                        Some(pi) => {
                            let world = pi.frame.to_world(at);
                            let kind = lay.text_kind.clone();
                            let size = kind
                                .as_ref()
                                .and_then(|k| self.opts.style.paper.text_type(k))
                                .map_or(3.5, |t| t.size_mm as f64);
                            view_change = Some((
                                pi.view,
                                Box::new(move |v: &mut View| {
                                    v.annotations.push(Annotation::Text {
                                        id,
                                        text: "Text".into(),
                                        at: world,
                                        size_mm: size,
                                        angle_deg: 0.0,
                                        kind,
                                    })
                                }),
                            ));
                            lay.picked = Some(Picked::Annotation(pi.view, id));
                        }
                        None => {
                            let mut s = def.clone();
                            let kind = lay.text_kind.clone();
                            let size = kind
                                .as_ref()
                                .and_then(|k| self.opts.style.paper.text_type(k))
                                .map_or(5.0, |t| t.size_mm as f64);
                            s.texts.push(SheetText {
                                id,
                                text: "Text".into(),
                                at_mm: at,
                                size_mm: size,
                                angle_deg: 0.0,
                                kind,
                            });
                            store = Some(s);
                            lay.picked = Some(Picked::SheetText(id));
                        }
                    }
                    lay.tool = Tool::Select;
                    lay.text_form = None;
                }
                if resp.dragged_by(PointerButton::Primary) {
                    pan(lay, resp.drag_delta());
                }
            }
            Tool::Dimension => {
                if resp.dragged_by(PointerButton::Primary) {
                    pan(lay, resp.drag_delta());
                }
                let snap_px = 10.0 / lay.zoom;
                if resp.clicked()
                    && let Some(p) = pointer
                {
                    let raw = to_paper(p);
                    match &mut lay.dim {
                        None => {
                            if let Some(pi) = placed_at(&lay.info, raw) {
                                let s = snap(&pi.segments, raw, snap_px);
                                lay.dim = Some((pi.id, vec![s]));
                            }
                        }
                        Some((pid, pts)) => {
                            let pi = lay.info.placed.iter().find(|x| x.id == *pid).cloned();
                            if let Some(pi) = pi {
                                if pts.len() == 1 {
                                    let s = snap(&pi.segments, raw, snap_px);
                                    if (s[0] - pts[0][0]).hypot(s[1] - pts[0][1]) > 1e-6 {
                                        pts.push(s);
                                    }
                                } else {
                                    // Third click: where the dimension line goes, and so
                                    // whether it measures across, up, or along a→b.
                                    let (a, b) = (pts[0], pts[1]);
                                    let (axis, off_paper) = dim_axis_and_offset(a, b, raw);
                                    let (wa, wb) = (pi.frame.to_world(a), pi.frame.to_world(b));
                                    let offset_mm = (off_paper / pi.frame.k).round();
                                    let id = GlobalId::new();
                                    let kind = lay.dim_kind.clone();
                                    view_change = Some((
                                        pi.view,
                                        Box::new(move |v: &mut View| {
                                            v.annotations.push(Annotation::Dimension {
                                                id,
                                                a: VAnchor {
                                                    at: wa,
                                                    element: None,
                                                },
                                                b: VAnchor {
                                                    at: wb,
                                                    element: None,
                                                },
                                                offset_mm,
                                                axis,
                                                kind,
                                            })
                                        }),
                                    ));
                                    lay.picked = Some(Picked::Annotation(pi.view, id));
                                    lay.dim = None;
                                }
                            } else {
                                lay.dim = None;
                            }
                        }
                    }
                }
            }
        }

        // ---- keys: Delete removes what is selected; Esc drops a half-made dimension, then
        // the selection; F fits the sheet.
        let typing = ui.ctx().egui_wants_keyboard_input();
        let (del, esc, f) = ui.input(|i| {
            (
                i.key_pressed(egui::Key::Delete),
                i.key_pressed(egui::Key::Escape),
                i.key_pressed(egui::Key::F),
            )
        });
        if !typing {
            if f {
                lay.fit = true;
            }
            if esc {
                if lay.dim.take().is_none() && lay.tool == Tool::Select {
                    lay.picked = None;
                }
                lay.tool = Tool::Select;
            }
            if del && let Some(pk) = lay.picked.take() {
                match pk {
                    Picked::Placed(id) => {
                        let mut s = def.clone();
                        s.placed.retain(|p| p.id != id);
                        store = Some(s);
                    }
                    Picked::SheetText(id) => {
                        let mut s = def.clone();
                        s.texts.retain(|t| t.id != id);
                        store = Some(s);
                    }
                    Picked::Annotation(v, id) => {
                        view_change = Some((
                            v,
                            Box::new(move |v: &mut View| v.annotations.retain(|a| a.id() != id)),
                        ));
                    }
                }
            }
        }

        // ---- overlays: the selection, a view being moved, snap and dimension in progress.
        let stroke = egui::Stroke::new(1.5, accent);
        if let Some(Picked::Placed(id)) = lay.picked
            && let Some(pi) = lay.info.placed.iter().find(|p| p.id == id)
        {
            let m = lay.moving.map(|(_, m)| m).unwrap_or([0.0, 0.0]);
            let r = egui::Rect::from_min_max(
                to_screen([pi.rect[0] + m[0], pi.rect[1] + m[1]]),
                to_screen([pi.rect[2] + m[0], pi.rect[3] + m[1]]),
            );
            painter.rect_stroke(r, 0.0, stroke, egui::StrokeKind::Outside);
        }
        if let Some(Picked::SheetText(id)) = lay.picked
            && let Some(t) = def.texts.iter().find(|t| t.id == id)
        {
            let r = text_rect(&t.text, t.at_mm, t.size_mm);
            painter.rect_stroke(
                egui::Rect::from_min_max(to_screen(r[0]), to_screen(r[1])),
                0.0,
                stroke,
                egui::StrokeKind::Outside,
            );
        }
        if let Some(Picked::Annotation(vid, aid)) = lay.picked
            && let Some(v) = self.views.iter().find(|v| v.id == vid)
            && let Some(a) = v.annotations.iter().find(|a| a.id() == aid)
        {
            for pi in lay.info.placed.iter().filter(|p| p.view == vid) {
                match a {
                    Annotation::Text {
                        text, at, size_mm, ..
                    } => {
                        let r = text_rect(text, pi.frame.to_paper(*at), *size_mm);
                        painter.rect_stroke(
                            egui::Rect::from_min_max(to_screen(r[0]), to_screen(r[1])),
                            0.0,
                            stroke,
                            egui::StrokeKind::Outside,
                        );
                    }
                    Annotation::Dimension { a, b, .. } => {
                        for q in [a, b] {
                            painter.circle_stroke(to_screen(pi.frame.to_paper(q.at)), 4.0, stroke);
                        }
                    }
                }
            }
        }
        if lay.tool == Tool::Dimension
            && let Some(hp) = lay.hover
        {
            let target = match &lay.dim {
                Some((pid, _)) => lay.info.placed.iter().find(|x| x.id == *pid).cloned(),
                None => placed_at(&lay.info, hp),
            };
            if let Some(pi) = target {
                let s = snap(&pi.segments, hp, 10.0 / lay.zoom);
                painter.circle_stroke(to_screen(s), 4.0, stroke);
                if let Some((_, pts)) = &lay.dim {
                    let thin = egui::Stroke::new(1.0, accent);
                    if pts.len() == 1 {
                        painter.line_segment([to_screen(pts[0]), to_screen(s)], thin);
                    } else {
                        // The dimension it would make.
                        let (axis, off) = dim_axis_and_offset(pts[0], pts[1], hp);
                        if let Some((_, qa, qb, _)) =
                            bimble_drawing::viewdraw::dimension_on_paper(pts[0], pts[1], off, axis)
                        {
                            painter.line_segment([to_screen(pts[0]), to_screen(qa)], thin);
                            painter.line_segment([to_screen(pts[1]), to_screen(qb)], thin);
                            painter.line_segment([to_screen(qa), to_screen(qb)], thin);
                        }
                    }
                }
            }
        }
        // While a view is dragged from the navigator: where it will go.
        if let Some(p) = pointer
            && resp.dnd_hover_payload::<ViewDrag>().is_some()
        {
            painter.circle_filled(p, 5.0, accent);
        }
        if lay.moving.is_some() || resp.dragged() {
            ui.ctx().request_repaint();
        }

        // ---- apply
        if let Some(s) = store
            && let Err(e) = self.store_sheet(s)
        {
            self.view_message = Some(e);
        }
        if let Some((v, f)) = view_change {
            self.update_view(v, f);
        }
        if let Some(v) = open_view {
            self.close_sheet();
            self.activate_view(v);
        }
    }

    /// Properties of the open sheet, or of what is selected on it.
    pub(crate) fn layout_properties(&mut self, ui: &mut egui::Ui) {
        let Some(lay) = &self.layout else { return };
        let Some(def) = self.sheet_def(lay.sheet) else {
            return;
        };
        let picked = lay.picked;
        ui.horizontal(|ui| {
            let lay = self.layout.as_mut().expect("checked above");
            for (t, label, tip) in [
                (
                    Tool::Select,
                    "Select",
                    "Select, move (drag), open (double-click); Delete removes",
                ),
                (
                    Tool::Text,
                    "Text",
                    "Click to place a text: in a view it belongs to the view",
                ),
                (
                    Tool::Dimension,
                    "Dimension",
                    "Two points on the lines of a view, then where the dimension line goes",
                ),
            ] {
                if ui
                    .selectable_label(lay.tool == t, label)
                    .on_hover_text(tip)
                    .clicked()
                {
                    lay.tool = t;
                    lay.dim = None;
                }
            }
            if ui.small_button("Fit").clicked() {
                lay.fit = true;
            }
        });
        // The type new texts or dimensions get.
        let paper = &self.opts.style.paper;
        let text_names: Vec<String> = paper.text_types.iter().map(|t| t.name.clone()).collect();
        let dim_names: Vec<String> = paper
            .dimension_types
            .iter()
            .map(|t| t.name.clone())
            .collect();
        let lay = self.layout.as_mut().expect("checked above");
        match lay.tool {
            Tool::Text => {
                kind_combo(
                    ui,
                    "New texts",
                    "text-kind",
                    &text_names,
                    &mut lay.text_kind,
                );
            }
            Tool::Dimension => {
                kind_combo(
                    ui,
                    "New dimensions",
                    "dim-kind",
                    &dim_names,
                    &mut lay.dim_kind,
                );
            }
            Tool::Select => {}
        }
        ui.separator();
        if let Some(m) = &self.view_message {
            ui.colored_label(ui.visuals().warn_fg_color, m);
        }
        match picked {
            Some(Picked::Placed(id)) => {
                let Some(p) = def.placed.iter().find(|p| p.id == id).cloned() else {
                    return;
                };
                let name = self
                    .views
                    .iter()
                    .find(|v| v.id == p.view)
                    .map_or("(missing view)".to_string(), |v| v.name.clone());
                ui.heading("Placed view");
                ui.label(&name);
                let mut scale = p.scale;
                let mut commit = false;
                ui.horizontal(|ui| {
                    ui.label("Scale 1:");
                    let r = ui.add(egui::DragValue::new(&mut scale).range(1..=5000));
                    commit |= r.drag_stopped() || r.lost_focus();
                    for s in [20, 50, 100, 200] {
                        if ui.small_button(s.to_string()).clicked() {
                            scale = s;
                            commit = true;
                        }
                    }
                });
                let mut remove = false;
                let mut open = false;
                ui.horizontal(|ui| {
                    open = ui.button("Open view").clicked();
                    remove = ui.button("Remove from sheet").clicked();
                });
                if commit && scale != p.scale {
                    let mut s = def.clone();
                    if let Some(q) = s.placed.iter_mut().find(|q| q.id == id) {
                        q.scale = scale;
                    }
                    if let Err(e) = self.store_sheet(s) {
                        self.view_message = Some(e);
                    }
                }
                if remove {
                    let mut s = def.clone();
                    s.placed.retain(|q| q.id != id);
                    if let Some(l) = self.layout.as_mut() {
                        l.picked = None;
                    }
                    if let Err(e) = self.store_sheet(s) {
                        self.view_message = Some(e);
                    }
                }
                if open {
                    self.close_sheet();
                    self.activate_view(p.view);
                }
            }
            Some(pk @ (Picked::SheetText(_) | Picked::Annotation(..))) => {
                self.text_properties(ui, &def, pk);
            }
            None => self.sheet_properties(ui, &def),
        }
    }

    /// The type of a selected text or dimension, and a picker to change it.
    fn kind_properties(&mut self, ui: &mut egui::Ui, def: &SheetDef, pk: Picked) {
        let paper = &self.opts.style.paper;
        let current: Option<(Option<String>, bool)> = match pk {
            Picked::SheetText(id) => def
                .texts
                .iter()
                .find(|t| t.id == id)
                .map(|t| (t.kind.clone(), false)),
            Picked::Annotation(v, a) => self
                .views
                .iter()
                .find(|x| x.id == v)
                .and_then(|x| x.annotations.iter().find(|y| y.id() == a))
                .map(|y| match y {
                    Annotation::Text { kind, .. } => (kind.clone(), false),
                    Annotation::Dimension { kind, .. } => (kind.clone(), true),
                }),
            Picked::Placed(_) => None,
        };
        let Some((mut kind, is_dim)) = current else {
            return;
        };
        let names: Vec<String> = if is_dim {
            paper
                .dimension_types
                .iter()
                .map(|t| t.name.clone())
                .collect()
        } else {
            paper.text_types.iter().map(|t| t.name.clone()).collect()
        };
        let size = kind
            .as_ref()
            .and_then(|k| paper.text_type(k))
            .map(|t| t.size_mm as f64);
        if !kind_combo(ui, "Type", "picked-kind", &names, &mut kind) {
            return;
        }
        let size = kind
            .as_ref()
            .and_then(|k| self.opts.style.paper.text_type(k))
            .map(|t| t.size_mm as f64)
            .or(size);
        match pk {
            Picked::SheetText(id) => {
                let mut d = def.clone();
                if let Some(t) = d.texts.iter_mut().find(|t| t.id == id) {
                    t.kind = kind;
                    if let Some(s) = size {
                        t.size_mm = s;
                    }
                }
                if let Err(e) = self.store_sheet(d) {
                    self.view_message = Some(e);
                }
            }
            Picked::Annotation(v, a) => {
                self.update_view(v, move |x| {
                    match x.annotations.iter_mut().find(|y| y.id() == a) {
                        Some(Annotation::Text {
                            kind: k, size_mm, ..
                        }) => {
                            *k = kind;
                            if let Some(s) = size {
                                *size_mm = s;
                            }
                        }
                        Some(Annotation::Dimension { kind: k, .. }) => *k = kind,
                        None => {}
                    }
                });
            }
            Picked::Placed(_) => {}
        }
        if let Some(l) = self.layout.as_mut() {
            l.text_form = None;
        }
    }

    /// A text (the sheet's or a view's) or a dimension.
    fn text_properties(&mut self, ui: &mut egui::Ui, def: &SheetDef, pk: Picked) {
        self.kind_properties(ui, def, pk);
        // The current text and size.
        let current = match pk {
            Picked::SheetText(id) => def
                .texts
                .iter()
                .find(|t| t.id == id)
                .map(|t| (t.text.clone(), t.size_mm)),
            Picked::Annotation(v, a) => self
                .views
                .iter()
                .find(|x| x.id == v)
                .and_then(|x| x.annotations.iter().find(|y| y.id() == a))
                .and_then(|y| match y {
                    Annotation::Text { text, size_mm, .. } => Some((text.clone(), *size_mm)),
                    Annotation::Dimension { .. } => None,
                }),
            Picked::Placed(_) => None,
        };
        let Some((text, size)) = current else {
            if let Picked::Annotation(v, a) = pk {
                ui.heading("Dimension");
                ui.weak("Belongs to the view: it shows wherever the view is placed.");
                if ui.button("Delete").clicked() {
                    self.update_view(v, move |x| x.annotations.retain(|y| y.id() != a));
                    if let Some(l) = self.layout.as_mut() {
                        l.picked = None;
                    }
                }
            }
            return;
        };
        let lay = self.layout.as_mut().expect("open");
        if lay.text_form.as_ref().is_none_or(|f| f.0 != pk) {
            lay.text_form = Some((pk, text.clone(), size));
        }
        let (_, t, s) = lay.text_form.as_mut().expect("set above");
        ui.heading("Text");
        ui.weak(match pk {
            Picked::SheetText(_) => "On the sheet.",
            _ => "Belongs to the view: it shows wherever the view is placed.",
        });
        let r1 = ui.add(egui::TextEdit::multiline(t).desired_rows(2));
        let mut commit = r1.lost_focus();
        ui.horizontal(|ui| {
            ui.label("Size");
            let r = ui.add(
                egui::DragValue::new(s)
                    .range(0.5..=50.0)
                    .speed(0.1)
                    .suffix(" mm"),
            );
            commit |= r.drag_stopped() || r.lost_focus();
        });
        let delete = ui.button("Delete").clicked();
        let (t, s) = (t.clone(), *s);
        if delete {
            lay.picked = None;
            lay.text_form = None;
        }
        match pk {
            Picked::SheetText(id) => {
                let mut d = def.clone();
                if delete {
                    d.texts.retain(|x| x.id != id);
                } else if commit && (t != text || s != size) {
                    if let Some(x) = d.texts.iter_mut().find(|x| x.id == id) {
                        x.text = t;
                        x.size_mm = s;
                    }
                } else {
                    return;
                }
                if let Err(e) = self.store_sheet(d) {
                    self.view_message = Some(e);
                }
            }
            Picked::Annotation(v, a) => {
                if delete {
                    self.update_view(v, move |x| x.annotations.retain(|y| y.id() != a));
                } else if commit && (t != text || s != size) {
                    self.update_view(v, move |x| {
                        if let Some(Annotation::Text { text, size_mm, .. }) =
                            x.annotations.iter_mut().find(|y| y.id() == a)
                        {
                            *text = t;
                            *size_mm = s;
                        }
                    });
                }
            }
            Picked::Placed(_) => {}
        }
    }

    /// Number, name, paper and title block fields of the open sheet.
    fn sheet_properties(&mut self, ui: &mut egui::Ui, def: &SheetDef) {
        let mut d = def.clone();
        let mut commit = false;
        ui.heading("Sheet");
        egui::Grid::new("sheet-props")
            .num_columns(2)
            .show(ui, |ui| {
                ui.label("Number");
                commit |= ui
                    .add(egui::TextEdit::singleline(&mut d.number).desired_width(120.0))
                    .lost_focus();
                ui.end_row();
                ui.label("Name");
                commit |= ui
                    .add(egui::TextEdit::singleline(&mut d.name).desired_width(180.0))
                    .lost_focus();
                ui.end_row();
                ui.label("Paper");
                ui.horizontal(|ui| {
                    egui::ComboBox::from_id_salt("sheet-paper")
                        .selected_text(d.paper.clone())
                        .show_ui(ui, |ui| {
                            for p in bimble_core::sheet::PAPERS {
                                commit |= ui
                                    .selectable_value(&mut d.paper, p.to_string(), p)
                                    .changed();
                            }
                        });
                    commit |= ui.checkbox(&mut d.portrait, "Portrait").changed();
                });
                ui.end_row();
            });
        ui.add_space(4.0);
        ui.strong("Title block");
        ui.weak("Values for {placeholders} of the template, e.g. phase, revision.");
        let mut remove = None;
        let keys: Vec<String> = d.fields.keys().cloned().collect();
        for k in keys {
            ui.horizontal(|ui| {
                ui.label(&k);
                let v = d.fields.get_mut(&k).expect("listed");
                commit |= ui
                    .add(egui::TextEdit::singleline(v).desired_width(140.0))
                    .lost_focus();
                if ui.small_button("x").clicked() {
                    remove = Some(k.clone());
                }
            });
        }
        if let Some(k) = remove {
            d.fields.remove(&k);
            commit = true;
        }
        ui.horizontal(|ui| {
            for k in ["phase", "revision", "drawn", "checked"] {
                if !d.fields.contains_key(k) && ui.small_button(format!("+ {k}")).clicked() {
                    d.fields.insert(k.into(), String::new());
                    commit = true;
                }
            }
        });
        ui.add_space(4.0);
        ui.weak("Drag views from the navigator onto the sheet.");
        let lay = self.layout.as_ref().expect("open");
        ui.weak(format!(
            "{} views · drawn in {:.0} ms",
            lay.info.placed.len(),
            lay.last_render_ms
        ));
        if commit && d != *def {
            match self.store_sheet(d) {
                Ok(()) => self.view_message = None,
                Err(e) => self.view_message = Some(e),
            }
        }
    }
}

/// A type picker: none (plain) or one of `names`. Returns whether it changed.
fn kind_combo(
    ui: &mut egui::Ui,
    label: &str,
    salt: &str,
    names: &[String],
    kind: &mut Option<String>,
) -> bool {
    let mut changed = false;
    ui.horizontal(|ui| {
        ui.label(label);
        egui::ComboBox::from_id_salt(salt)
            .selected_text(kind.clone().unwrap_or_else(|| "Plain".into()))
            .height(300.0)
            .show_ui(ui, |ui| {
                changed |= ui.selectable_value(kind, None, "Plain").changed();
                for n in names {
                    changed |= ui.selectable_value(kind, Some(n.clone()), n).changed();
                }
            });
    });
    changed
}

/// The nearest end of a line within `tol` (paper mm), else the nearest point on a line, else
/// the point itself.
fn snap(segments: &[[[f64; 2]; 2]], p: [f64; 2], tol: f64) -> [f64; 2] {
    let d = |a: [f64; 2], b: [f64; 2]| (a[0] - b[0]).hypot(a[1] - b[1]);
    let mut best: Option<(f64, [f64; 2])> = None;
    for s in segments {
        for q in s {
            let dq = d(*q, p);
            if dq <= tol && best.is_none_or(|(bd, _)| dq < bd) {
                best = Some((dq, *q));
            }
        }
    }
    if let Some((_, q)) = best {
        return q;
    }
    for [a, b] in segments {
        let ab = [b[0] - a[0], b[1] - a[1]];
        let l2 = ab[0] * ab[0] + ab[1] * ab[1];
        if l2 < 1e-12 {
            continue;
        }
        let t = (((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1]) / l2).clamp(0.0, 1.0);
        let q = [a[0] + ab[0] * t, a[1] + ab[1] * t];
        let dq = d(q, p);
        if dq <= tol * 0.8 && best.is_none_or(|(bd, _)| dq < bd) {
            best = Some((dq, q));
        }
    }
    best.map_or(p, |(_, q)| q)
}

/// Where the third click of a dimension puts it: above or below both points → measured across
/// (horizontal); left or right of both → measured up (vertical); between them → along a→b. And
/// its offset (paper mm, to the left of its direction).
fn dim_axis_and_offset(a: [f64; 2], b: [f64; 2], c: [f64; 2]) -> (bimble_core::view::DimAxis, f64) {
    use bimble_core::view::DimAxis;
    let (x0, x1) = (a[0].min(b[0]), a[0].max(b[0]));
    let (y0, y1) = (a[1].min(b[1]), a[1].max(b[1]));
    let axis = if (b[0] - a[0]).abs() < 1e-6 || (b[1] - a[1]).abs() < 1e-6 {
        DimAxis::Aligned
    } else if c[1] < y0 || c[1] > y1 {
        DimAxis::Horizontal
    } else if c[0] < x0 || c[0] > x1 {
        DimAxis::Vertical
    } else {
        DimAxis::Aligned
    };
    let d = match axis {
        DimAxis::Aligned => [b[0] - a[0], b[1] - a[1]],
        DimAxis::Horizontal => [if b[0] < a[0] { -1.0 } else { 1.0 }, 0.0],
        DimAxis::Vertical => [0.0, if b[1] < a[1] { -1.0 } else { 1.0 }],
    };
    let l = d[0].hypot(d[1]).max(1e-12);
    let n = [d[1] / l, -d[0] / l];
    (axis, (c[0] - a[0]) * n[0] + (c[1] - a[1]) * n[1])
}

/// A text's box on paper: its first line from its baseline point, all its lines.
fn text_rect(text: &str, at: [f64; 2], size: f64) -> [[f64; 2]; 2] {
    let lines: Vec<&str> = text.split('\n').collect();
    let wmax = lines
        .iter()
        .map(|l| bimble_drawing::sheet::text_width_mm(l, size, false))
        .fold(0.0, f64::max);
    [
        [at[0], at[1] - size],
        [
            at[0] + wmax.max(size),
            at[1] + (lines.len() as f64 - 1.0) * size * 1.25 + size * 0.3,
        ],
    ]
}

fn inside(r: [[f64; 2]; 2], p: [f64; 2], tol: f64) -> bool {
    p[0] >= r[0][0] - tol && p[0] <= r[1][0] + tol && p[1] >= r[0][1] - tol && p[1] <= r[1][1] + tol
}

/// The sheet text under `p`, if any.
fn pick_text(def: &SheetDef, p: [f64; 2], tol: f64) -> Option<Picked> {
    def.texts
        .iter()
        .rev()
        .find(|t| inside(text_rect(&t.text, t.at_mm, t.size_mm), p, tol))
        .map(|t| Picked::SheetText(t.id))
}

/// A view annotation under `p` on its placed drawing.
fn pick_annotation(v: &View, pi: &PlacedInfo, p: [f64; 2], tol: f64) -> Option<GlobalId> {
    for a in v.annotations.iter().rev() {
        match a {
            Annotation::Text {
                text, at, size_mm, ..
            } => {
                if inside(text_rect(text, pi.frame.to_paper(*at), *size_mm), p, tol) {
                    return Some(a.id());
                }
            }
            Annotation::Dimension {
                a: pa,
                b: pb,
                offset_mm,
                axis,
                ..
            } => {
                let Some((_, la, lb, _)) = bimble_drawing::viewdraw::dimension_on_paper(
                    pi.frame.to_paper(pa.at),
                    pi.frame.to_paper(pb.at),
                    offset_mm * pi.frame.k,
                    *axis,
                ) else {
                    continue;
                };
                let s = snap(&[[la, lb]], p, tol * 2.0);
                if (s[0] - p[0]).hypot(s[1] - p[1]) <= tol * 2.0 {
                    return Some(a.id());
                }
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn snapping_prefers_ends_then_lines() {
        let segs = [[[0.0, 0.0], [10.0, 0.0]]];
        assert_eq!(snap(&segs, [0.5, 0.5], 1.0), [0.0, 0.0]);
        assert_eq!(snap(&segs, [5.0, 0.4], 1.0), [5.0, 0.0]);
        assert_eq!(snap(&segs, [5.0, 3.0], 1.0), [5.0, 3.0]);
    }

    #[test]
    fn texts_are_picked_by_their_box() {
        let mut def = SheetDef::new("S-101", "x");
        let id = GlobalId::new();
        def.texts.push(SheetText {
            id,
            text: "Concrete C30/37".into(),
            at_mm: [100.0, 100.0],
            size_mm: 5.0,
            angle_deg: 0.0,
            kind: None,
        });
        assert_eq!(
            pick_text(&def, [110.0, 98.0], 0.5),
            Some(Picked::SheetText(id))
        );
        assert_eq!(pick_text(&def, [110.0, 120.0], 0.5), None);
    }

    #[test]
    fn the_third_click_chooses_how_a_dimension_measures() {
        use bimble_core::view::DimAxis;
        let (a, b) = ([0.0, 0.0], [10.0, 2.0]);
        // Above both points: across (horizontal), 5 mm up = to the left of +x.
        let (ax, off) = dim_axis_and_offset(a, b, [5.0, -5.0]);
        assert_eq!(ax, DimAxis::Horizontal);
        assert!((off - 5.0).abs() < 1e-9);
        let (q, qa, qb, len) = bimble_drawing::viewdraw::dimension_on_paper(a, b, off, ax).unwrap();
        assert_eq!(q, [1.0, 0.0]);
        assert!(
            (len - 10.0).abs() < 1e-9 && (qa[1] + 5.0).abs() < 1e-9 && (qb[1] + 5.0).abs() < 1e-9
        );
        // Right of both: up (vertical), 2 mm long.
        let (ax, _) = dim_axis_and_offset(a, b, [15.0, 1.0]);
        assert_eq!(ax, DimAxis::Vertical);
        assert!(
            (bimble_drawing::viewdraw::dimension_on_paper(a, b, 3.0, ax)
                .unwrap()
                .3
                - 2.0)
                .abs()
                < 1e-9
        );
        // Between them: along a→b.
        assert_eq!(dim_axis_and_offset(a, b, [5.0, 1.5]).0, DimAxis::Aligned);
    }
}
