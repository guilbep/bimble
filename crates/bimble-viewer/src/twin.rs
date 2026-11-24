//! Dev-only "native twin" (native only): when `BIMBLE_SERVE=<port>` is set, the viewer serves
//! the bridge calls over HTTP (`serve.rs`) so the Vue lab can drive a native process.
//! Requests go to the UI thread over a channel; `Viewer::ui` serves them with `Viewer::call`.

use crate::serve::{self, Handler, Request};
use eframe::egui;
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::time::Duration;

type Reply = Sender<Result<String, String>>;
type Change = Arc<(Mutex<(u64, String)>, Condvar)>;

const CALL_TIMEOUT: Duration = Duration::from_secs(5);
const ALLOWED_ORIGINS: [&str; 2] = ["http://localhost:5173", "http://127.0.0.1:5173"];

/// The UI-thread end: pending requests and the last published change.
pub(crate) struct Twin {
    rx: Receiver<(Request, Reply)>,
    change: Change,
    last: Option<String>,
}

struct ServerSide {
    tx: Sender<(Request, Reply)>,
    ctx: egui::Context,
    change: Change,
}

fn lock(c: &Change) -> MutexGuard<'_, (u64, String)> {
    c.0.lock().unwrap_or_else(|e| e.into_inner())
}

impl Handler for ServerSide {
    fn call(&self, req: Request) -> Result<String, String> {
        let (reply, answer) = channel();
        self.tx
            .send((req, reply))
            .map_err(|_| "viewer closed".to_string())?;
        self.ctx.request_repaint();
        answer
            .recv_timeout(CALL_TIMEOUT)
            .unwrap_or_else(|_| Err("viewer busy (timeout)".to_string()))
    }

    fn wait_change(&self, since: u64, timeout: Duration) -> (u64, String) {
        let (_, cv) = &*self.change;
        let guard = lock(&self.change);
        let (guard, _) = cv
            .wait_timeout_while(guard, timeout, |(seq, _)| *seq <= since)
            .unwrap_or_else(|e| e.into_inner());
        if guard.0 > since {
            (guard.0, guard.1.clone())
        } else {
            (guard.0, "null".to_string())
        }
    }
}

fn random_token() -> String {
    use std::hash::{BuildHasher, Hasher};
    let mut h = std::collections::hash_map::RandomState::new().build_hasher();
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    h.write_u128(now);
    let a = h.finish();
    h.write_u64(a);
    format!("{a:016x}{:016x}", h.finish())
}

/// Starts the server when `BIMBLE_SERVE` is set; None (no behaviour change) otherwise.
pub(crate) fn from_env(ctx: &egui::Context) -> Option<Twin> {
    start(ctx, "")
}

fn start(ctx: &egui::Context, note: &str) -> Option<Twin> {
    let port = std::env::var("BIMBLE_SERVE").ok()?;
    let port: u16 = match port.trim().parse() {
        Ok(p) => p,
        Err(_) => {
            eprintln!("bimble twin: BIMBLE_SERVE={port:?} is not a port; not serving");
            return None;
        }
    };
    let token = std::env::var("BIMBLE_TOKEN")
        .ok()
        .filter(|t| !t.is_empty())
        .unwrap_or_else(random_token);
    let (tx, rx) = channel();
    let change: Change = Arc::new((Mutex::new((0, "null".to_string())), Condvar::new()));
    let side = ServerSide {
        tx,
        ctx: ctx.clone(),
        change: change.clone(),
    };
    let cfg = serve::Config {
        port,
        token: token.clone(),
        allowed_origins: ALLOWED_ORIGINS.iter().map(|s| s.to_string()).collect(),
    };
    match serve::start(cfg, Arc::new(side)) {
        Ok(port) => {
            eprintln!("bimble twin: listening {port} token {token}{note}");
            Some(Twin {
                rx,
                change,
                last: None,
            })
        }
        Err(e) => {
            eprintln!("bimble twin: cannot listen on {port}: {e}");
            None
        }
    }
}

impl Twin {
    /// The next pending request: first without waiting, then up to 1 ms for a burst.
    pub(crate) fn next(&self, waited: bool) -> Option<(Request, Reply)> {
        if !waited {
            return self.rx.try_recv().ok();
        }
        self.rx.recv_timeout(Duration::from_millis(1)).ok()
    }

    /// Publishes `json` if it differs from the last one: bumps seq and wakes the long-polls.
    pub(crate) fn publish(&mut self, json: String) {
        if self.last.as_deref() == Some(json.as_str()) {
            return;
        }
        self.last = Some(json.clone());
        let mut g = lock(&self.change);
        g.0 += 1;
        g.1 = json;
        drop(g);
        self.change.1.notify_all();
    }
}

/// True when `BIMBLE_SERVE` is set and `BIMBLE_HEADLESS=1`: serve without a window.
pub(crate) fn headless_requested() -> bool {
    std::env::var_os("BIMBLE_SERVE").is_some()
        && std::env::var("BIMBLE_HEADLESS").is_ok_and(|v| v == "1")
}

/// Runs a future that wgpu resolves without an event loop (native adapter and device requests).
fn block_on<F: std::future::Future>(f: F) -> F::Output {
    let mut f = std::pin::pin!(f);
    let mut cx = std::task::Context::from_waker(std::task::Waker::noop());
    loop {
        if let std::task::Poll::Ready(v) = f.as_mut().poll(&mut cx) {
            return v;
        }
        std::thread::yield_now();
    }
}

/// The twin without a window: no eframe, no frames. A plain loop waits up to 50 ms for a
/// request, finishes a pending load (`poll_loading`), serves the call and publishes the change.
/// Edits rebuild the scene inside `call` (`try_apply`, `undo_redo`); GPU uploads are skipped
/// because nothing is drawn.
pub(crate) fn run_headless(
    file: Option<std::path::PathBuf>,
    style: crate::GraphicStyle,
    style_path: Option<std::path::PathBuf>,
) -> Result<(), String> {
    use eframe::egui_wgpu::{self, wgpu};
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::from_env().unwrap_or(wgpu::Backends::all()),
        ..wgpu::InstanceDescriptor::new_without_display_handle()
    });
    let adapter = block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::LowPower,
        ..Default::default()
    }))
    .map_err(|e| format!("no GPU adapter: {e}"))?;
    let (device, queue) = block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        label: Some("bimble twin headless"),
        required_limits: wgpu::Limits::downlevel_defaults().using_resolution(adapter.limits()),
        ..Default::default()
    }))
    .map_err(|e| format!("no GPU device: {e}"))?;
    let format = wgpu::TextureFormat::Rgba8Unorm;
    let renderer = egui_wgpu::Renderer::new(&device, format, Default::default());
    let rs = egui_wgpu::RenderState {
        adapter,
        available_adapters: Vec::new(),
        instance,
        device,
        queue,
        target_format: format,
        renderer: Arc::new(egui::mutex::RwLock::new(renderer)),
    };
    let ctx = egui::Context::default();
    let mut app = crate::Viewer::new(&rs);
    eprintln!("bimble twin: adapter {}", app.adapter);
    app.opts.style = style;
    app.style_baseline = app.opts.style.clone();
    app.style_path = style_path;
    app.viewport_only = true;
    if let Some(f) = file {
        app.open(f, &ctx);
    }
    let mut twin = start(&ctx, " (headless)").ok_or("the twin server did not start")?;
    loop {
        let req = twin.rx.recv_timeout(Duration::from_millis(50));
        app.poll_loading();
        match req {
            Ok((req, reply)) => {
                let _ = reply.send(app.call(&ctx, &req.op, &req.name, &req.body));
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Ok(()),
        }
        twin.publish(app.change_json());
    }
}
