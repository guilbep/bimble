//! Hide and isolate elements (#54), as in Revit: for now (temporary, this session, a status chip
//! to undo it) or in the view (saved with it, by GlobalId), and Reveal hidden to see and unhide
//! what is hidden. They act on the selection: an element clicked, others added with Ctrl/Cmd.

use crate::Viewer;
use crate::ribbon::Action;
use eframe::egui;
use std::collections::BTreeSet;

/// Hidden or isolated for now, by GlobalId (cleared on another view or model).
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Temporary {
    pub isolate: bool,
    pub elements: BTreeSet<String>,
}

impl Viewer {
    /// The selected elements: the one clicked, and those added with Ctrl/Cmd.
    pub(crate) fn selection(&self) -> BTreeSet<usize> {
        self.selected
            .into_iter()
            .chain(self.also_selected.iter().copied())
            .collect()
    }

    pub(crate) fn clear_selection(&mut self) {
        self.selected = None;
        self.also_selected.clear();
        self.needs_upload = true;
    }

    /// A click with Ctrl/Cmd: the element joins the selection, or leaves it if it was in.
    pub(crate) fn toggle_selected(&mut self, i: usize) {
        if self.selected == Some(i) {
            self.selected = self.also_selected.pop_first();
        } else if !self.also_selected.remove(&i) {
            match self.selected {
                None => self.selected = Some(i),
                Some(_) => {
                    self.also_selected.insert(i);
                }
            }
        }
        self.needs_upload = true;
    }

    /// Hidden by element (in the view, or for now), whatever its class, storey or material.
    pub(crate) fn hidden_element(&self, i: usize) -> bool {
        let Some(l) = &self.loaded else { return false };
        hidden_by(
            &l.scene.elements[i].global_id,
            &self.hidden_ids,
            &self.isolated_ids,
            self.temporary.as_ref(),
        )
    }

    pub(crate) fn hide_or_isolate(&mut self, a: Action) {
        let Some(l) = &self.loaded else { return };
        let ids: BTreeSet<String> = self
            .selection()
            .iter()
            .map(|&i| l.scene.elements[i].global_id.clone())
            .collect();
        if ids.is_empty() {
            return;
        }
        match a {
            Action::HideNow | Action::IsolateNow => {
                let isolate = a == Action::IsolateNow;
                // Hiding more adds to what is hidden for now; isolating replaces it.
                let elements = match (&self.temporary, isolate) {
                    (Some(t), false) if !t.isolate => t.elements.union(&ids).cloned().collect(),
                    _ => ids,
                };
                self.temporary = Some(Temporary { isolate, elements });
            }
            Action::HideInView | Action::IsolateInView | Action::Unhide => {
                match a {
                    Action::HideInView => self.hidden_ids.extend(ids),
                    Action::IsolateInView => {
                        self.isolated_ids = ids;
                        self.hidden_ids.clear();
                    }
                    _ => {
                        // Unhide: shown again in the view, and for now.
                        for g in &ids {
                            self.hidden_ids.remove(g);
                        }
                        if !self.isolated_ids.is_empty() {
                            self.isolated_ids.extend(ids.iter().cloned());
                        }
                        if let Some(t) = &mut self.temporary {
                            if t.isolate {
                                t.elements.extend(ids);
                            } else {
                                t.elements.retain(|g| !ids.contains(g));
                            }
                        }
                    }
                }
                self.save_visibility();
            }
            _ => return,
        }
        if !self.reveal_hidden {
            self.clear_selection();
        }
        self.needs_upload = true;
    }

    /// The right-click menu of the view: the same commands as the ribbon's Visibility group.
    pub(crate) fn visibility_menu(&self, ui: &mut egui::Ui, run: &mut Option<Action>) {
        for a in [
            Action::HideNow,
            Action::IsolateNow,
            Action::HideInView,
            Action::IsolateInView,
            Action::Unhide,
            Action::RevealHidden,
        ] {
            if ui
                .add_enabled(self.action_enabled(a), egui::Button::new(a.label()))
                .clicked()
            {
                *run = Some(a);
                ui.close();
            }
        }
    }
}

/// Whether an element is hidden by element: hidden in the view, outside the view's isolation,
/// or hidden or outside the isolation for now.
pub(crate) fn hidden_by(
    g: &str,
    hidden: &BTreeSet<String>,
    isolated: &BTreeSet<String>,
    now: Option<&Temporary>,
) -> bool {
    hidden.contains(g)
        || (!isolated.is_empty() && !isolated.contains(g))
        || now.is_some_and(|t| t.elements.contains(g) != t.isolate)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hidden_in_the_view_isolated_or_for_now() {
        let set = |v: &[&str]| {
            v.iter()
                .map(|s| s.to_string())
                .collect::<BTreeSet<String>>()
        };
        let none = set(&[]);
        // Nothing hidden: everything shows.
        assert!(!hidden_by("a", &none, &none, None));
        // Hidden in the view.
        assert!(hidden_by("a", &set(&["a"]), &none, None));
        assert!(!hidden_by("b", &set(&["a"]), &none, None));
        // Isolated in the view: only those show.
        assert!(!hidden_by("a", &none, &set(&["a"]), None));
        assert!(hidden_by("b", &none, &set(&["a"]), None));
        // For now: hidden ones hide, isolating hides the others; on top of the view's.
        let hide = Temporary {
            isolate: false,
            elements: set(&["a"]),
        };
        let iso = Temporary {
            isolate: true,
            elements: set(&["a"]),
        };
        assert!(hidden_by("a", &none, &none, Some(&hide)));
        assert!(!hidden_by("b", &none, &none, Some(&hide)));
        assert!(!hidden_by("a", &none, &none, Some(&iso)));
        assert!(hidden_by("b", &none, &none, Some(&iso)));
        assert!(hidden_by("a", &set(&["a"]), &none, Some(&iso)));
    }
}
