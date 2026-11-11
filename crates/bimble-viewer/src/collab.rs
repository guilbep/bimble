//! Working together in the viewer (ADR 0038, #55): choose the shared folder, Synchronize
//! (save, commit what isn't, fetch, merge, push, give back what was borrowed), borrow elements
//! on their first change, and say whose an element is when it belongs to someone else.
//! The desktop only: the history lives next to the file.

use crate::Viewer;
use crate::ribbon::Action;
use bimble_core::remote;
use bimble_core::repo::{Merged, Prefer, Repo, RepoError};
use eframe::egui;
use web_time::{Duration, Instant};

/// After this long without synchronizing, a chip says so (the office: "twice a day").
pub(crate) const REMIND_AFTER: Duration = Duration::from_secs(4 * 3600);

impl Viewer {
    /// The shared folder of the open model's history, if it has one.
    fn shared_folder(&self) -> Option<std::path::PathBuf> {
        let l = self.loaded.as_ref()?;
        l.edit.as_ref()?;
        let r = Repo::open(&l.path).ok()?;
        remote::remote_of(&r).ok().flatten()
    }

    /// Borrow on first change when the model is shared (after opening it, or choosing the
    /// folder). Since when this model hasn't synchronized: from now.
    pub(crate) fn start_borrowing(&mut self) {
        let folder = self.shared_folder();
        let who = self.settings.author();
        let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) else {
            return;
        };
        ed.borrowing = folder.map(|remote| crate::edit::Borrowing {
            remote,
            who,
            mine: Default::default(),
        });
        self.synced_at = ed.borrowing.as_ref().map(|_| Instant::now());
    }

    /// Hours since the last synchronize, when it is time to (a chip).
    pub(crate) fn sync_due(&self) -> Option<u64> {
        let t = self.synced_at?;
        (t.elapsed() >= REMIND_AFTER).then(|| t.elapsed().as_secs() / 3600)
    }

    pub(crate) fn run_collab(&mut self, a: Action) {
        let msg = match a {
            Action::SharedFolder => self.choose_shared_folder(),
            Action::Synchronize => self.synchronize(),
            _ => return,
        };
        if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
            ed.message = Some(msg.clone());
        }
        self.collab_message = Some(msg);
    }

    /// The local history, started with a first commit if the model has none.
    fn local_history(&mut self) -> Result<Repo, String> {
        let author = self.settings.author();
        let l = self.loaded.as_mut().ok_or("no model open")?;
        let ed = l
            .edit
            .as_mut()
            .ok_or("working together is for .bimble models")?;
        match Repo::open(&l.path) {
            Ok(r) => Ok(r),
            Err(RepoError::NoRepo(_)) => {
                bimble_core::file::save(&ed.model, &l.path).map_err(|e| e.to_string())?;
                ed.dirty = false;
                let r = Repo::init(&l.path).map_err(|e| e.to_string())?;
                r.commit(&ed.model, "Start", &author, bimble_core::date::now_secs())
                    .map_err(|e| e.to_string())?;
                Ok(r)
            }
            Err(e) => Err(e.to_string()),
        }
    }

    #[cfg(not(target_arch = "wasm32"))]
    fn choose_shared_folder(&mut self) -> String {
        let Some(dir) = rfd::FileDialog::new()
            .set_title("The folder others synchronize with (a network drive, a synced folder)")
            .pick_folder()
        else {
            return "No folder chosen.".into();
        };
        let who = self.settings.author();
        let Some(l) = self.loaded.as_mut() else {
            return String::new();
        };
        let Some(ed) = l.edit.as_mut() else {
            return "Working together is for .bimble models.".into();
        };
        // On file first: the history starts from what is saved.
        if ed.dirty {
            if let Err(e) = bimble_core::file::save(&ed.model, &l.path) {
                return e.to_string();
            }
            ed.dirty = false;
        }
        let now = bimble_core::date::now_secs();
        let msg = match remote::join(&dir, &l.path, &ed.model, &who, now) {
            Err(e) => return e.to_string(),
            Ok(remote::Joined::First) => format!(
                "Shared through {}: your work is the first there. Synchronize to send it.",
                dir.display()
            ),
            Ok(remote::Joined::Linked) => format!("Shared through {}.", dir.display()),
            Ok(remote::Joined::Copied(shared)) => {
                // This copy becomes the shared model; what it was is kept beside it.
                let kept = l.path.with_extension("before-sharing.bimble");
                let backup = if *shared != ed.model {
                    match bimble_core::file::save(&ed.model, &kept) {
                        Ok(()) => format!(" Your copy as it was: {}.", kept.display()),
                        Err(e) => return e.to_string(),
                    }
                } else {
                    String::new()
                };
                ed.model = *shared;
                ed.history = bimble_core::History::default();
                ed.form = None;
                if let Err(e) = bimble_core::file::save(&ed.model, &l.path) {
                    return e.to_string();
                }
                self.rebuild_scene(None);
                format!("Now the shared model, from {}.{backup}", dir.display())
            }
        };
        self.start_borrowing();
        msg
    }

    #[cfg(target_arch = "wasm32")]
    fn choose_shared_folder(&mut self) -> String {
        "Working together needs the desktop version.".into()
    }

    /// Save, commit what isn't, then synchronize with the shared folder.
    fn synchronize(&mut self) -> String {
        // As whom the elements were borrowed: what Synchronize gives back (a name changed in
        // Settings since opening takes effect after this sync).
        let author = self
            .loaded
            .as_ref()
            .and_then(|l| l.edit.as_ref())
            .and_then(|e| e.borrowing.as_ref())
            .map_or_else(|| self.settings.author(), |b| b.who.clone());
        let now = bimble_core::date::now_secs();
        let local = match self.local_history() {
            Ok(r) => r,
            Err(e) => return e,
        };
        let Some(dir) = remote::remote_of(&local).ok().flatten() else {
            return "Choose the shared folder first (Collaborate → Shared folder…).".into();
        };
        let Some(l) = self.loaded.as_mut() else {
            return String::new();
        };
        let Some(ed) = l.edit.as_mut() else {
            return String::new();
        };
        // Everything on file and in the history first: synchronize works on commits.
        if ed.dirty {
            if let Err(e) = bimble_core::file::save(&ed.model, &l.path) {
                return e.to_string();
            }
            ed.dirty = false;
        }
        // Open, never create (an unmounted share must not become a new, empty history).
        let shared = match Repo::open_dir(&dir) {
            Ok(r) => r,
            Err(e) => return format!("{}: {e}", dir.display()),
        };
        match remote::sync_working(&local, &shared, &ed.model, &author, now, Prefer::Neither) {
            Ok(Ok((how, model))) => {
                ed.model = model;
                ed.history = bimble_core::History::default();
                ed.form = None;
                if let Some(b) = &mut ed.borrowing {
                    b.mine.clear();
                }
                let saved = bimble_core::file::save(&ed.model, &l.path);
                self.rebuild_scene(None);
                // Nothing borrowed now: borrow from here on as who Settings says.
                self.start_borrowing();
                if self.history_window {
                    self.refresh_history();
                }
                match saved {
                    Err(e) => e.to_string(),
                    Ok(()) => match how {
                        Merged::UpToDate => "Synchronized: nothing new from the others.".into(),
                        _ => "Synchronized: the others' work merged with yours.".into(),
                    },
                }
            }
            Ok(Err(conflicts)) => {
                let list: Vec<String> = conflicts.iter().take(8).map(|c| c.to_string()).collect();
                format!(
                    "Not synchronized, nothing sent: {} conflict(s). Talk to the others, then \
                     synchronize again (or from the command line with --prefer):\n{}",
                    conflicts.len(),
                    list.join("\n")
                )
            }
            Err(e) => e.to_string(),
        }
    }

    /// "This element belongs to …": Synchronize or Cancel (the office's wording, #51).
    pub(crate) fn belongs_ui(&mut self, ctx: &egui::Context) {
        let Some(list) = self
            .loaded
            .as_ref()
            .and_then(|l| l.edit.as_ref())
            .and_then(|e| e.belongs.clone())
        else {
            return;
        };
        let mut done = None;
        egui::Window::new("Borrowed by someone else")
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ctx, |ui| {
                for l in &list {
                    ui.label(l);
                }
                ui.add_space(4.0);
                ui.weak(
                    "It is theirs until they synchronize. Synchronize to get their work (and \
                     send yours), or ask them.",
                );
                ui.horizontal(|ui| {
                    if ui.button("Synchronize").clicked() {
                        done = Some(true);
                    }
                    if ui.button("Cancel").clicked() {
                        done = Some(false);
                    }
                });
            });
        if let Some(sync) = done {
            if let Some(ed) = self.loaded.as_mut().and_then(|l| l.edit.as_mut()) {
                ed.belongs = None;
            }
            if sync {
                self.run_collab(Action::Synchronize);
            }
        }
    }
}
