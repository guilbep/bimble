//! Everything the viewer is doing that isn't its default, said explicitly (ADR 0028).
//!
//! The window shows each such state as a chip in the status bar, with a way back to the default;
//! what isn't saved is listed in the title row and asked about before closing. This module is
//! the pure part: from a snapshot of the viewer's state to the chips and the unsaved list, so it
//! can be tested without a window.

/// What the viewer is doing, as far as being explicit about it goes.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Snapshot {
    /// Placing an element: its kind's name.
    pub placing: Option<&'static str>,
    pub measuring: bool,
    /// A sheet tool other than Select: its name.
    pub sheet_tool: Option<&'static str>,
    pub section_box: bool,
    pub edges: bool,
    pub hatching: bool,
    pub orthographic: bool,
    /// Hidden in the active view: families, classes, storeys, links, elements; then how many
    /// elements are isolated (#54); then materials.
    pub hidden: [usize; 7],
    /// Hidden or isolated for now (#54): isolating, and how many elements.
    pub temporary: Option<(bool, usize)>,
    /// Reveal hidden is on (#54).
    pub revealing: bool,
    /// Working together and not synchronized for this many hours (ADR 0038).
    pub sync_due: Option<u64>,
    /// Measurements taken and kept.
    pub measurements: usize,
    /// The plan options (what plans show, unit, title block…) differ from the template loaded.
    pub plan_options_changed: bool,
    /// Unsaved: the model's edits, an IFC's views, the graphic standards.
    pub model_dirty: bool,
    pub views_dirty: bool,
    pub standards_changed: bool,
    /// Only part of a model too large for this machine is shown (ADR 0033): which part.
    pub partial: Option<String>,
}

/// A state that isn't the default, and how to leave it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Chip {
    Placing,
    Measuring,
    SheetTool,
    SectionBox,
    EdgesOff,
    HatchingOff,
    Orthographic,
    Hidden,
    Temporary,
    Revealing,
    SyncDue,
    Measurements,
    PlanOptions,
    Partial,
}

impl Chip {
    /// What clicking its × does.
    pub fn reset_hint(self) -> &'static str {
        match self {
            Chip::Placing => "Stop placing (Esc)",
            Chip::Measuring => "Stop measuring (M)",
            Chip::SheetTool => "Back to Select",
            Chip::SectionBox => "Turn the section box off (B)",
            Chip::EdgesOff => "Show edges again (E)",
            Chip::HatchingOff => "Show hatching again (H)",
            Chip::Orthographic => "Back to perspective (O)",
            Chip::Hidden => "Show everything in this view (saved with the view)",
            Chip::Temporary => "Show again what is hidden for now",
            Chip::Revealing => "Stop revealing hidden elements",
            Chip::SyncDue => "Synchronize now: send your work, get the others'",
            Chip::Measurements => "Delete the measurements",
            Chip::PlanOptions => "Back to the plan template's options",
            Chip::Partial if cfg!(target_arch = "wasm32") => {
                "Open the file again (Open…) to choose another part"
            }
            Chip::Partial => "Open the file again to choose another part",
        }
    }
}

fn plural(n: usize, one: &str, many: &str) -> String {
    format!("{n} {}", if n == 1 { one } else { many })
}

/// The chips for a snapshot, with their labels, in a fixed order: what the pointer does first,
/// then how the view looks, then what is kept.
pub fn chips(s: &Snapshot) -> Vec<(Chip, String)> {
    let mut v = Vec::new();
    // What is loaded at all comes before everything else.
    if let Some(p) = &s.partial {
        v.push((Chip::Partial, format!("Showing part: {p}")));
    }
    if let Some(k) = s.placing {
        v.push((Chip::Placing, format!("Placing: {}", k.to_lowercase())));
    }
    if s.measuring {
        v.push((Chip::Measuring, "Measuring".into()));
    }
    if let Some(t) = s.sheet_tool {
        v.push((Chip::SheetTool, format!("Sheet tool: {}", t.to_lowercase())));
    }
    if s.section_box {
        v.push((Chip::SectionBox, "Section box".into()));
    }
    if !s.edges {
        v.push((Chip::EdgesOff, "Edges off".into()));
    }
    if !s.hatching {
        v.push((Chip::HatchingOff, "Hatching off".into()));
    }
    if s.orthographic {
        v.push((Chip::Orthographic, "Orthographic".into()));
    }
    let [f, c, st, l, e, iso, m] = s.hidden;
    if f + c + st + l + e + iso + m > 0 {
        let parts: Vec<String> = [
            (f, "family", "families"),
            (m, "material", "materials"),
            (c, "class", "classes"),
            (st, "storey", "storeys"),
            (l, "link", "links"),
            (e, "element", "elements"),
            (iso, "isolated", "isolated"),
        ]
        .into_iter()
        .filter(|(n, _, _)| *n > 0)
        .map(|(n, a, b)| plural(n, a, b))
        .collect();
        v.push((Chip::Hidden, format!("Hidden: {}", parts.join(", "))));
    }
    if let Some((isolate, n)) = s.temporary {
        let what = if isolate { "isolated" } else { "hidden" };
        v.push((Chip::Temporary, format!("For now: {n} {what}")));
    }
    if s.revealing {
        v.push((Chip::Revealing, "Revealing hidden".into()));
    }
    if let Some(h) = s.sync_due {
        v.push((Chip::SyncDue, format!("Not synchronized for {h} h")));
    }
    if s.measurements > 0 {
        v.push((
            Chip::Measurements,
            plural(s.measurements, "measurement", "measurements"),
        ));
    }
    if s.plan_options_changed {
        v.push((Chip::PlanOptions, "Plan options changed".into()));
    }
    v
}

/// What isn't saved, in words, for the title row and the closing question.
pub fn unsaved(s: &Snapshot) -> Vec<&'static str> {
    let mut v = Vec::new();
    if s.model_dirty {
        v.push("model");
    }
    if s.views_dirty {
        v.push("views");
    }
    if s.standards_changed {
        v.push("standards");
    }
    v
}

/// In a browser: whether leaving the page should ask first (unsaved work). The page's
/// `beforeunload` handler reads it; the viewer sets it every frame.
#[cfg(target_arch = "wasm32")]
pub static UNSAVED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// In a browser: ask before leaving the page while something isn't saved (downloaded).
#[cfg(target_arch = "wasm32")]
pub fn guard_page_unload() {
    use wasm_bindgen::JsCast;
    use wasm_bindgen::closure::Closure;
    let Some(w) = web_sys::window() else { return };
    let f =
        Closure::<dyn FnMut(web_sys::BeforeUnloadEvent)>::new(|e: web_sys::BeforeUnloadEvent| {
            if UNSAVED.load(std::sync::atomic::Ordering::Relaxed) {
                e.prevent_default();
                e.set_return_value("Not saved");
            }
        });
    w.set_onbeforeunload(Some(f.as_ref().unchecked_ref()));
    // The handler lives as long as the page.
    f.forget();
}

/// The `links` query (bridge): linked models (`(file name, shown)`) then the views' reference
/// plans, as the egui Links panel lists them. `kind` is the file extension ("ifc", "bimble") or the
/// reference's format ("pdf", "dxf"); models have no GlobalId, so their id is the file name.
#[cfg_attr(not(target_arch = "wasm32"), allow(dead_code))]
pub fn links_json(
    models: &[(String, bool)],
    refs: &[&bimble_core::view::Reference],
) -> serde_json::Value {
    let models = models.iter().map(|(name, visible)| {
        let kind = std::path::Path::new(name)
            .extension()
            .map(|e| e.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        serde_json::json!({ "id": name, "name": name, "kind": kind, "visible": visible })
    });
    let refs = refs.iter().map(|r| {
        serde_json::json!({
            "id": r.id.to_string(), "name": r.name, "kind": r.format, "visible": r.visible,
        })
    });
    serde_json::Value::Array(models.chain(refs).collect())
}

/// The `measurements` query (bridge): `[{id, label, value_mm}]` as the egui Measurements panel
/// lists them ("1. 2 500 mm", thin space); id is the position, as the panel's delete uses it.
#[cfg_attr(not(target_arch = "wasm32"), allow(dead_code))]
pub fn measurements_json(dims: &[crate::measure::Dimension]) -> serde_json::Value {
    dims.iter()
        .enumerate()
        .map(|(k, d)| {
            let mm = d.distance();
            serde_json::json!({
                "id": k,
                "label": format!("{}. {}", k + 1, crate::measure::format_mm(mm)),
                "value_mm": f64::from(mm),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn default_view() -> Snapshot {
        Snapshot {
            edges: true,
            hatching: true,
            ..Default::default()
        }
    }

    /// The default state says nothing: no chip, nothing unsaved.
    #[test]
    fn the_default_is_silent() {
        let s = default_view();
        assert!(chips(&s).is_empty());
        assert!(unsaved(&s).is_empty());
    }

    /// Every state that isn't the default gets a chip with a way back.
    #[test]
    fn every_non_default_state_is_said() {
        let s = Snapshot {
            placing: Some("Column"),
            measuring: true,
            sheet_tool: Some("Dimension"),
            section_box: true,
            edges: false,
            hatching: false,
            orthographic: true,
            hidden: [0, 2, 1, 1, 3, 2, 0],
            temporary: Some((true, 4)),
            revealing: true,
            sync_due: Some(5),
            measurements: 1,
            plan_options_changed: true,
            ..Default::default()
        };
        let c = chips(&s);
        let labels: Vec<&str> = c.iter().map(|(_, l)| l.as_str()).collect();
        assert_eq!(
            labels,
            [
                "Placing: column",
                "Measuring",
                "Sheet tool: dimension",
                "Section box",
                "Edges off",
                "Hatching off",
                "Orthographic",
                "Hidden: 2 classes, 1 storey, 1 link, 3 elements, 2 isolated",
                "For now: 4 isolated",
                "Revealing hidden",
                "Not synchronized for 5 h",
                "1 measurement",
                "Plan options changed",
            ]
        );
        assert!(c.iter().all(|(k, _)| !k.reset_hint().is_empty()));
    }

    #[test]
    fn unsaved_lists_each_kind() {
        let s = Snapshot {
            model_dirty: true,
            standards_changed: true,
            ..default_view()
        };
        assert_eq!(unsaved(&s), ["model", "standards"]);
        let s = Snapshot {
            views_dirty: true,
            ..default_view()
        };
        assert_eq!(unsaved(&s), ["views"]);
    }

    #[test]
    fn links_and_measurements_as_json() {
        use crate::measure::{Dimension, Snap, SnapKind};
        let r: bimble_core::view::Reference = serde_json::from_value(serde_json::json!({
            "id": "0aaaaaaaaaaaaaaaaaaaaa", "name": "ground.dxf", "format": "dxf",
            "offset_mm": [0.0, 0.0], "rotation_deg": 0.0, "polylines": [],
        }))
        .unwrap();
        assert_eq!(
            links_json(&[("Architecture.IFC".into(), false)], &[&r]),
            serde_json::json!([
                {"id": "Architecture.IFC", "name": "Architecture.IFC", "kind": "ifc", "visible": false},
                {"id": "0aaaaaaaaaaaaaaaaaaaaa", "name": "ground.dxf", "kind": "dxf", "visible": true},
            ])
        );
        let snap = |x: f32| Snap {
            point: glam::Vec3::new(x, 0.0, 0.0),
            kind: SnapKind::Vertex,
            normal: None,
            element: 0,
        };
        let d = Dimension {
            a: snap(0.0),
            b: snap(2500.0),
        };
        assert_eq!(
            measurements_json(&[d]),
            serde_json::json!([{"id": 0, "label": "1. 2\u{2009}500 mm", "value_mm": 2500.0}])
        );
    }
}
