//! A view On paper (ADR 0046): the view's own tab shows the exact drawing its PDF and sheets are
//! made of (pens by its scale, hatches, dashed lines above the cut, grids, chains, its
//! annotations), zoomed at the pointer and panned like a sheet. Elements are selected on the
//! model: the clicked paper point becomes a world point, and a ray along the view's look, cut
//! by the view's range, picks the element drawn there. Double-click goes to the model there.

use super::*;
use bimble_core::GlobalId;
use bimble_core::view::{Look, ViewKind};
use bimble_drawing::plan::{PlanInfo, PlanOptions, plan};

/// A drawing ready to show: prepared, its frame (paper ↔ world), its page size (mm).
type Drawn = Result<(bimble_drawing::sheet::Prepared, PlanInfo, (f64, f64)), String>;

/// The view's drawing, made from a shared scene.
fn draw(scene: &bimble_geom::Scene, opts: &PlanOptions) -> Drawn {
    let (sheet, info) = plan(scene, opts)?;
    let size = (sheet.width_mm, sheet.height_mm);
    Ok((sheet.prepare()?, info, size))
}

/// A drawing being made in the background: for which key, where it arrives, since when.
pub(crate) struct Job {
    key: u64,
    rx: std::sync::mpsc::Receiver<(u64, Drawn, f64)>,
    since: web_time::Instant,
}

/// The drawing of the view shown on paper, and the camera on it (paper mm).
pub(crate) struct PaperView {
    pub view: GlobalId,
    key: u64,
    prepared: Option<bimble_drawing::sheet::Prepared>,
    info: Option<PlanInfo>,
    size: (f64, f64),
    error: Option<String>,
    center: [f64; 2],
    zoom: f64,
    fit: bool,
    texture: Option<(egui::TextureHandle, u64)>,
    /// How long the last drawing took (ms): said in the status bar.
    pub drawn_ms: f64,
    /// A world point to bring under the middle once drawn (switching from Model there).
    look_at: Option<[f64; 3]>,
    /// The drawing being made (the picture shown is then the previous one).
    job: Option<Job>,
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
    /// The active view is shown On paper.
    pub(crate) fn on_paper(&self) -> bool {
        self.layout.is_none()
            && self.in_2d()
            && self
                .active_view
                .is_some_and(|id| self.paper_views.contains(&id))
    }

    /// Model or On paper for the active view, keeping the place: the point under the pointer
    /// (else the middle) is where the other picture opens.
    pub(crate) fn set_on_paper(&mut self, on: bool) {
        let Some(id) = self.active_view else { return };
        if !self.in_2d() {
            return;
        }
        if on {
            self.paper_views.insert(id);
            let at = self.pointer_world;
            let p = self.paper.get_or_insert_with(|| PaperView::new(id));
            if p.view != id {
                *p = PaperView::new(id);
            }
            p.look_at = at;
        } else {
            self.paper_views.remove(&id);
            if let (Some(p), Some(l)) = (&self.paper, &self.loaded)
                && p.view == id
                && let Some(info) = &p.info
            {
                // The model at the paper's middle.
                let w = info.frame.to_world(p.center);
                let o = l.scene.origin_mm;
                self.camera.target = glam::Vec3::new(
                    (w[0] - o[0]) as f32,
                    (w[1] - o[1]) as f32,
                    self.camera.target.z,
                );
            }
        }
        self.needs_render = true;
    }

    /// Draw the active view on paper when it changed (its definition, the model, the standards).
    fn refresh_paper(&mut self, ctx: &egui::Context) {
        let Some(v) = self.active_view().cloned() else {
            return;
        };
        let Some(l) = &self.loaded else { return };
        let key = hash_of(&[
            serde_json::to_string(&v).unwrap_or_default().as_bytes(),
            serde_json::to_string(&self.opts.style)
                .unwrap_or_default()
                .as_bytes(),
            &self.scene_rev.to_le_bytes(),
            &self.plan.template_rev.to_le_bytes(),
            serde_json::to_string(&self.project_info())
                .unwrap_or_default()
                .as_bytes(),
        ]);
        let scene = l.scene.clone();
        let opts = PlanOptions {
            scale: v.scale,
            style: self.opts.style.clone(),
            project: l
                .path
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default(),
            date: bimble_drawing::today(),
            template: self.plan.template.with_project(&self.project_info()),
            view: Some(v.clone()),
            bare: true,
            section_marks: bimble_drawing::plan::section_marks(&self.views),
            ..Default::default()
        };
        let p = self.paper.get_or_insert_with(|| PaperView::new(v.id));
        if p.view != v.id {
            *p = PaperView::new(v.id);
        }
        p.receive(key);
        if p.key == key || p.job.as_ref().is_some_and(|j| j.key == key) {
            return;
        }
        // Another drawing is needed: in the background on the desktop (the picture shown stays,
        // marked stale), at once in a browser.
        #[cfg(not(target_arch = "wasm32"))]
        {
            let (tx, rx) = std::sync::mpsc::channel();
            let ctx = ctx.clone();
            std::thread::spawn(move || {
                let t0 = web_time::Instant::now();
                let drawn = draw(&scene, &opts);
                let _ = tx.send((key, drawn, t0.elapsed().as_secs_f64() * 1000.0));
                ctx.request_repaint();
            });
            p.job = Some(Job {
                key,
                rx,
                since: web_time::Instant::now(),
            });
        }
        #[cfg(target_arch = "wasm32")]
        {
            let _ = ctx;
            let t0 = web_time::Instant::now();
            let drawn = draw(&scene, &opts);
            p.install(key, drawn, t0.elapsed().as_secs_f64() * 1000.0);
        }
    }

    /// The element drawn at a paper point: picked on the model along the view's look, within
    /// its range (a cut is hit where it is cut, a seen top where it is seen).
    fn paper_pick(&self, at: [f64; 2]) -> Option<usize> {
        let (l, p, v) = (
            self.loaded.as_ref()?,
            self.paper.as_ref()?,
            self.active_view()?,
        );
        let info = p.info.as_ref()?;
        let w = info.frame.to_world(at);
        let dir = match &v.kind {
            ViewKind::Plan {
                look: Look::Down, ..
            } => glam::Vec3::NEG_Z,
            ViewKind::Plan { look: Look::Up, .. } => glam::Vec3::Z,
            ViewKind::Section { facing, .. } => {
                let d = v.facing_dir(*facing, &l.scene.georef);
                glam::Vec3::new(d[0] as f32, d[1] as f32, 0.0)
            }
            ViewKind::ThreeD => return None,
        };
        let o = l.scene.origin_mm;
        let at = glam::Vec3::new(
            (w[0] - o[0]) as f32,
            (w[1] - o[1]) as f32,
            (w[2] - o[2]) as f32,
        );
        // From well outside the model, through the point, along the look.
        let start = at - dir * (2.0 * l.bounds.radius + 1000.0);
        let clip = crate::views::view_clip(v, &l.scene, &l.scene.levels())
            .ok()
            .flatten();
        bimble_render::pick_hit_bvh(
            &l.scene,
            &l.bvh,
            // What the drawing shows: visible, and not a class the template leaves off paper.
            |i| self.visible(i) && !self.plan.template.hides(&l.scene.elements[i].class),
            start,
            dir,
            clip.as_ref(),
        )
        .map(|h| h.element)
    }

    /// The view On paper in the main view.
    pub(crate) fn paper_ui(&mut self, ui: &mut egui::Ui, rect: egui::Rect) {
        // Placing and measuring are done on the model: there, at this place.
        if self.measure_on
            || self
                .loaded
                .as_ref()
                .and_then(|l| l.edit.as_ref())
                .is_some_and(|e| e.placing.is_some())
        {
            self.set_on_paper(false);
            self.view_note =
                Some("Switched to Model: elements are placed and measured on the model".into());
            return;
        }
        self.refresh_paper(&ui.ctx().clone());
        let resp = ui.allocate_rect(rect, Sense::click_and_drag());
        let ppp = ui.ctx().pixels_per_point() as f64;
        let Some(p) = self.paper.as_mut() else { return };
        let (w, h) = p.size;
        if p.fit && w > 0.0 {
            p.center = [w / 2.0, h / 2.0];
            p.zoom = (rect.width() as f64 / (w * 1.05)).min(rect.height() as f64 / (h * 1.05));
            p.fit = false;
        }
        // Coming from Model: the same world point in the middle.
        if let (Some(at), Some(info)) = (p.look_at, &p.info) {
            p.center = info.frame.to_paper(at);
            p.look_at = None;
        }
        let (c, z) = (p.center, p.zoom);
        let to_paper = move |q: egui::Pos2| {
            [
                c[0] + (q.x - rect.center().x) as f64 / z,
                c[1] + (q.y - rect.center().y) as f64 / z,
            ]
        };
        let to_screen = move |q: [f64; 2]| {
            egui::pos2(
                rect.center().x + ((q[0] - c[0]) * z) as f32,
                rect.center().y + ((q[1] - c[1]) * z) as f32,
            )
        };
        // The picture of the part of the paper in view (as a sheet's).
        let (wpx, hpx) = (
            (rect.width() as f64 * ppp).round() as u32,
            (rect.height() as f64 * ppp).round() as u32,
        );
        let tkey = hash_of(&[
            &p.key.to_le_bytes(),
            &c[0].to_bits().to_le_bytes(),
            &c[1].to_bits().to_le_bytes(),
            &z.to_bits().to_le_bytes(),
            &wpx.to_le_bytes(),
            &hpx.to_le_bytes(),
        ]);
        if p.texture.as_ref().is_none_or(|t| t.1 != tkey)
            && let Some(prep) = &p.prepared
        {
            match prep.render(z * ppp, to_paper(rect.min), wpx, hpx) {
                Ok(r) => {
                    let img = egui::ColorImage::from_rgba_unmultiplied(
                        [r.width as usize, r.height as usize],
                        &r.rgba,
                    );
                    let tex = ui
                        .ctx()
                        .load_texture("on-paper", img, egui::TextureOptions::LINEAR);
                    p.texture = Some((tex, tkey));
                }
                Err(e) => p.error = Some(e),
            }
        }
        let painter = ui.painter_at(rect);
        painter.rect_filled(rect, 0.0, Color32::from_gray(200));
        if let Some((t, _)) = &p.texture {
            painter.image(
                t.id(),
                rect,
                egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
                Color32::WHITE,
            );
        }
        // A drawing under way: the picture shown is the previous one, greyed, and said so.
        if let Some(secs) = p.drawing_for() {
            if p.texture.is_some() {
                painter.rect_filled(rect, 0.0, Color32::from_white_alpha(110));
            }
            painter.text(
                rect.center_top() + egui::vec2(0.0, 16.0),
                egui::Align2::CENTER_TOP,
                format!("Drawing… {secs:.1} s"),
                egui::FontId::proportional(16.0),
                Color32::from_gray(60),
            );
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(250));
        }
        if let Some(e) = &p.error {
            painter.text(
                rect.left_top() + egui::vec2(10.0, 10.0),
                egui::Align2::LEFT_TOP,
                e,
                egui::FontId::proportional(14.0),
                Color32::RED,
            );
        }

        // Navigation: scroll zooms at the pointer, any drag pans.
        if resp.hovered() {
            let (scroll, pinch) = ui.input(|i| (i.smooth_scroll_delta().y, i.zoom_delta()));
            if (scroll != 0.0 || pinch != 1.0)
                && let Some(q) = resp.hover_pos()
            {
                let before = to_paper(q);
                let f = (scroll as f64 * 0.0015).exp() * pinch as f64;
                p.zoom = (p.zoom * f).clamp(0.05, 200.0);
                p.center = [
                    before[0] - (q.x - rect.center().x) as f64 / p.zoom,
                    before[1] - (q.y - rect.center().y) as f64 / p.zoom,
                ];
            }
        }
        if resp.dragged() {
            let d = resp.drag_delta();
            p.center[0] -= d.x as f64 / p.zoom;
            p.center[1] -= d.y as f64 / p.zoom;
        }
        let frame = p.info.as_ref().map(|i| i.frame);

        // Hover and click: the element there, picked on the model.
        let pointer = resp.hover_pos();
        let world_at = |q: egui::Pos2| frame.map(|f| f.to_world(to_paper(q)));
        if let Some(q) = pointer
            && !resp.dragged()
        {
            self.pointer_world = world_at(q);
            if self.hover_at != pointer {
                self.hover_at = pointer;
                let h = self.paper_pick(to_paper(q));
                self.set_hovered(h);
            }
        }
        if resp.double_clicked() {
            // The model, there.
            if let Some(w) = pointer.and_then(world_at) {
                self.pointer_world = Some(w);
            }
            self.set_on_paper(false);
            if let (Some(w), Some(l)) = (self.pointer_world, &self.loaded) {
                let o = l.scene.origin_mm;
                self.camera.target.x = (w[0] - o[0]) as f32;
                self.camera.target.y = (w[1] - o[1]) as f32;
            }
            return;
        }
        if resp.clicked()
            && let Some(q) = resp.interact_pointer_pos()
        {
            let add = ui.input(|i| i.modifiers.command);
            match self.paper_pick(to_paper(q)) {
                Some(i) if add => self.toggle_selected(i),
                Some(i) => {
                    self.clear_selection();
                    self.selected = Some(i);
                }
                None if !add => self.clear_selection(),
                None => {}
            }
        }

        // What is selected and under the pointer: their boxes on the paper.
        let Some(frame) = frame else { return };
        let accent = {
            let a = self.opts.style.selection.0;
            Color32::from_rgb(a[0], a[1], a[2])
        };
        let outline = |i: usize| -> Option<egui::Rect> {
            let l = self.loaded.as_ref()?;
            let b = l.scene.elements.get(i)?.bounds();
            if b.is_empty() {
                return None;
            }
            let o = l.scene.origin_mm;
            let mut r = egui::Rect::NOTHING;
            for k in 0..8 {
                let c = [0, 1, 2]
                    .map(|a| f64::from(if k >> a & 1 == 0 { b.min[a] } else { b.max[a] }) + o[a]);
                r.extend_with(to_screen(frame.to_paper(c)));
            }
            Some(r)
        };
        for i in self.selection() {
            if let Some(r) = outline(i) {
                painter.rect_stroke(
                    r,
                    0.0,
                    egui::Stroke::new(2.0, accent),
                    egui::StrokeKind::Outside,
                );
            }
        }
        if let Some(i) = self.hovered
            && let Some(r) = outline(i)
        {
            painter.rect_stroke(
                r,
                0.0,
                egui::Stroke::new(1.0, accent.gamma_multiply(0.6)),
                egui::StrokeKind::Outside,
            );
        }
    }
}

impl PaperView {
    fn new(view: GlobalId) -> Self {
        PaperView {
            view,
            key: 0,
            prepared: None,
            info: None,
            size: (0.0, 0.0),
            error: None,
            center: [0.0, 0.0],
            zoom: 1.0,
            fit: true,
            texture: None,
            drawn_ms: 0.0,
            look_at: None,
            job: None,
        }
    }

    /// A drawing finished: shown if it is the one wanted now (`key`), else dropped — the model
    /// or the view changed while it was made, and another is due.
    fn receive(&mut self, key: u64) {
        if let Some(job) = &self.job
            && let Ok((done, drawn, ms)) = job.rx.try_recv()
        {
            self.job = None;
            if done == key {
                self.install(key, drawn, ms);
            }
        }
    }

    /// Show a finished drawing.
    fn install(&mut self, key: u64, drawn: Drawn, ms: f64) {
        self.key = key;
        self.texture = None;
        self.drawn_ms = ms;
        match drawn {
            Ok((prepared, info, size)) => {
                if self.size != size {
                    self.fit = true;
                }
                self.prepared = Some(prepared);
                self.info = Some(info);
                self.size = size;
                self.error = None;
            }
            Err(e) => {
                self.prepared = None;
                self.error = Some(e);
            }
        }
    }

    /// Seconds a drawing has been under way, when one is (the picture shown is then stale).
    pub(crate) fn drawing_for(&self) -> Option<f64> {
        self.job.as_ref().map(|j| j.since.elapsed().as_secs_f64())
    }

    /// Fit the drawing in the view at the next frame.
    pub(crate) fn fit_next(&mut self) {
        self.fit = true;
    }

    /// Paper mm per screen point, as a scale: what 1:N the screen shows the drawing at.
    pub(crate) fn shown_scale(&self, view_scale: u32) -> Option<f64> {
        // zoom: screen points per paper mm, a point ~0.35 mm: one paper mm is zoom × 0.35 mm on
        // screen, so the model is shown at 1:(scale ÷ that).
        (self.zoom > 0.0).then(|| f64::from(view_scale) / (self.zoom * 0.3528))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// At 1:100 a paper mm is 100 model mm; seen at 2.835 screen points per paper mm (1 mm on
    /// screen, a point being 0.3528 mm), the model is seen at 1:100; twice as zoomed, 1:50.
    /// A drawing made for what the view was is dropped when it has changed since; the one
    /// wanted is shown.
    #[test]
    fn a_stale_drawing_is_dropped() {
        let mut p = PaperView::new(GlobalId::new());
        let job = |p: &mut PaperView, key: u64| {
            let (tx, rx) = std::sync::mpsc::channel();
            tx.send((key, Err("drawn".to_string()), 5.0)).unwrap();
            p.job = Some(Job {
                key,
                rx,
                since: web_time::Instant::now(),
            });
        };
        job(&mut p, 1);
        p.receive(2);
        assert!(
            p.job.is_none() && p.key == 0 && p.error.is_none(),
            "dropped"
        );
        job(&mut p, 2);
        p.receive(2);
        assert_eq!((p.key, p.error.as_deref()), (2, Some("drawn")));
        assert_eq!(p.drawn_ms, 5.0);
    }

    #[test]
    fn the_scale_the_screen_shows() {
        let mut p = PaperView::new(GlobalId::new());
        p.zoom = 1.0 / 0.3528;
        assert!((p.shown_scale(100).unwrap() - 100.0).abs() < 1e-9);
        p.zoom *= 2.0;
        assert!((p.shown_scale(100).unwrap() - 50.0).abs() < 1e-9);
    }
}
