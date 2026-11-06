//! Bimble in a browser. `web/index.html` loads this module; it starts the viewer in the page's
//! `<canvas id="bimble">`. Build with `scripts/build-web.sh` (ADR 0020).

#[cfg(target_arch = "wasm32")]
mod web {
    use wasm_bindgen::JsCast;
    use wasm_bindgen::prelude::*;

    /// Show a message in the page (the `#status` element), e.g. when the start fails.
    fn status(text: &str) {
        if let Some(el) = web_sys::window()
            .and_then(|w| w.document())
            .and_then(|d| d.get_element_by_id("status"))
        {
            el.set_text_content(Some(text));
        }
    }

    #[wasm_bindgen(start)]
    pub fn start() {
        // wgpu's and eframe's messages go to the browser console.
        let _ = eframe::WebLogger::init(log::LevelFilter::Info);
        // Panics go to the browser console (and the page), not into the void.
        std::panic::set_hook(Box::new(|info| {
            let msg = info.to_string();
            web_sys::console::error_1(&msg.clone().into());
            status(&format!("Bimble stopped: {msg}"));
        }));
        wasm_bindgen_futures::spawn_local(async {
            let canvas = web_sys::window()
                .and_then(|w| w.document())
                .and_then(|d| d.get_element_by_id("bimble"))
                .and_then(|e| e.dyn_into::<web_sys::HtmlCanvasElement>().ok());
            let Some(canvas) = canvas else {
                status("No <canvas id=\"bimble\"> in the page.");
                return;
            };
            match bimble_viewer::start_web(canvas).await {
                Ok(()) => status(""),
                Err(e) => {
                    web_sys::console::error_1(&e.clone().into());
                    status(&format!("Bimble could not start: {e}"));
                }
            }
        });
    }
}
