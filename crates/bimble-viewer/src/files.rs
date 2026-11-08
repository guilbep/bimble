//! Picking and saving files on the desktop and in a browser.
//!
//! On the desktop, dialogs are synchronous and models are read from their path (in parallel, on a
//! thread). In a browser, the file picker is asynchronous and gives the file's bytes, and saving
//! is a download. Both end up as a `Picked` the viewer handles the same way.

use eframe::egui;
use std::path::PathBuf;
use std::sync::mpsc::{Receiver, Sender, channel};

/// What a picked file is for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Purpose {
    Open,
    /// An IFC file imported as an editable .bimble copy (ADR 0032).
    ImportIfc,
    Link,
    PlanTemplate,
    Style,
    /// A reference plan (DXF, vector PDF; DWG is refused with a hint to convert it).
    Reference,
}

impl Purpose {
    fn filter(self) -> (&'static str, &'static [&'static str]) {
        match self {
            Purpose::Open => (
                "Building models, views files",
                &["ifc", "IFC", "bimble", "json"],
            ),
            Purpose::ImportIfc => ("IFC file", &["ifc", "IFC"]),
            Purpose::Link => ("Building models", &["ifc", "IFC", "bimble"]),
            Purpose::PlanTemplate => ("Plan template", &["json"]),
            Purpose::Style => ("Bimble style", &["json"]),
            Purpose::Reference => (
                "Reference plan",
                &["dxf", "DXF", "pdf", "PDF", "dwg", "DWG"],
            ),
        }
    }
}

/// A file the user picked: its path on the desktop, its content in a browser.
pub struct Picked {
    pub purpose: Purpose,
    pub name: String,
    pub path: Option<PathBuf>,
    pub bytes: Option<Vec<u8>>,
}

impl Picked {
    /// The file's content: from memory in a browser, read from disk on the desktop.
    pub fn content(&self) -> Result<Vec<u8>, String> {
        match (&self.bytes, &self.path) {
            (Some(b), _) => Ok(b.clone()),
            (None, Some(p)) => std::fs::read(p).map_err(|e| format!("{}: {e}", p.display())),
            (None, None) => Err("no file".into()),
        }
    }

    /// Path for display and for naming outputs (just the name in a browser).
    pub fn path_or_name(&self) -> PathBuf {
        self.path
            .clone()
            .unwrap_or_else(|| PathBuf::from(&self.name))
    }
}

pub struct Files {
    tx: Sender<Picked>,
    rx: Receiver<Picked>,
}

impl Default for Files {
    fn default() -> Self {
        let (tx, rx) = channel();
        Files { tx, rx }
    }
}

impl Files {
    /// Ask for a file. The result arrives through `poll` (at once on the desktop, after the
    /// browser's picker closes on the web).
    #[cfg(not(target_arch = "wasm32"))]
    pub fn pick(&self, purpose: Purpose, _ctx: &egui::Context) {
        let (label, exts) = purpose.filter();
        if let Some(p) = rfd::FileDialog::new().add_filter(label, exts).pick_file() {
            let _ = self.tx.send(Picked {
                purpose,
                name: p
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default(),
                path: Some(p),
                bytes: None,
            });
        }
    }

    #[cfg(target_arch = "wasm32")]
    pub fn pick(&self, purpose: Purpose, ctx: &egui::Context) {
        let (label, exts) = purpose.filter();
        let dialog = rfd::AsyncFileDialog::new().add_filter(label, exts);
        let tx = self.tx.clone();
        let ctx = ctx.clone();
        wasm_bindgen_futures::spawn_local(async move {
            if let Some(f) = dialog.pick_file().await {
                let _ = tx.send(Picked {
                    purpose,
                    name: f.file_name(),
                    path: None,
                    bytes: Some(f.read().await),
                });
                ctx.request_repaint();
            }
        });
    }

    /// A file dropped on the window: opened as a model.
    #[cfg(not(target_arch = "wasm32"))]
    pub fn dropped(&self, f: egui::DroppedFileHandle, _ctx: &egui::Context) {
        let path = f.path().to_path_buf();
        let _ = self.tx.send(Picked {
            purpose: Purpose::Open,
            name: path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
            path: Some(path),
            bytes: None,
        });
    }

    #[cfg(target_arch = "wasm32")]
    pub fn dropped(&self, f: egui::DroppedFileHandle, ctx: &egui::Context) {
        let tx = self.tx.clone();
        let ctx = ctx.clone();
        wasm_bindgen_futures::spawn_local(async move {
            let name = f
                .path()
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            if let Ok(bytes) = f.bytes_async().await {
                let _ = tx.send(Picked {
                    purpose: Purpose::Open,
                    name,
                    path: None,
                    bytes: Some(bytes),
                });
                ctx.request_repaint();
            }
        });
    }

    pub fn poll(&self) -> Option<Picked> {
        self.rx.try_recv().ok()
    }
}

/// Save bytes: a save dialog on the desktop (returns where it went, None if cancelled), a
/// download in a browser.
#[cfg(not(target_arch = "wasm32"))]
pub fn save(
    suggested: &str,
    filters: &[(&str, &[&str])],
    bytes: &[u8],
) -> Result<Option<String>, String> {
    let mut d = rfd::FileDialog::new().set_file_name(suggested);
    for (label, exts) in filters {
        d = d.add_filter(*label, exts);
    }
    let Some(p) = d.save_file() else {
        return Ok(None);
    };
    bimble_core::file::write_atomic(&p, bytes).map_err(|e| format!("{}: {e}", p.display()))?;
    Ok(Some(p.display().to_string()))
}

#[cfg(target_arch = "wasm32")]
pub fn save(
    suggested: &str,
    _filters: &[(&str, &[&str])],
    bytes: &[u8],
) -> Result<Option<String>, String> {
    download(suggested, bytes)?;
    Ok(Some(format!("{suggested} (downloaded)")))
}

/// Offer bytes as a download named `name`.
#[cfg(target_arch = "wasm32")]
pub fn download(name: &str, bytes: &[u8]) -> Result<(), String> {
    use wasm_bindgen::JsCast;
    let err = |e: wasm_bindgen::JsValue| format!("download failed: {e:?}");
    let array = js_sys::Uint8Array::from(bytes);
    let parts = js_sys::Array::of1(&array);
    let blob = web_sys::Blob::new_with_u8_array_sequence(&parts).map_err(err)?;
    let url = web_sys::Url::create_object_url_with_blob(&blob).map_err(err)?;
    let doc = web_sys::window()
        .and_then(|w| w.document())
        .ok_or("no document")?;
    let a: web_sys::HtmlAnchorElement = doc
        .create_element("a")
        .map_err(err)?
        .dyn_into()
        .map_err(|_| "not an anchor".to_string())?;
    a.set_href(&url);
    a.set_download(name);
    a.click();
    web_sys::Url::revoke_object_url(&url).map_err(err)?;
    Ok(())
}
