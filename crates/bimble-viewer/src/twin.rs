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
/// The last frame's RGBA8 bytes, handed from the UI thread to the server thread.
type Frame = Arc<Mutex<Vec<u8>>>;

const CALL_TIMEOUT: Duration = Duration::from_secs(5);
const ALLOWED_ORIGINS: [&str; 2] = ["http://localhost:5173", "http://127.0.0.1:5173"];

/// The default origins (Vite on 5173) and those of `BIMBLE_ORIGINS` (comma-separated, as the
/// C core's host): a Vite on another port (`scripts/twin-dev.sh`, the e2e). Loopback `http`
/// origins only; any other entry is ignored with a message.
fn allowed_origins(extra: Option<&str>) -> Vec<String> {
    let mut v: Vec<String> = ALLOWED_ORIGINS.iter().map(|s| s.to_string()).collect();
    for o in extra
        .unwrap_or("")
        .split(',')
        .map(str::trim)
        .filter(|o| !o.is_empty())
    {
        let port = o
            .strip_prefix("http://localhost:")
            .or_else(|| o.strip_prefix("http://127.0.0.1:"));
        if port.is_some_and(|p| p.parse::<u16>().is_ok()) {
            if !v.iter().any(|a| a == o) {
                v.push(o.to_string());
            }
        } else {
            eprintln!(
                "bimble twin: BIMBLE_ORIGINS: {o:?} is not http://localhost:<port> or http://127.0.0.1:<port>; ignored"
            );
        }
    }
    v
}

/// The UI-thread end: pending requests and the last published change.
pub(crate) struct Twin {
    rx: Receiver<(Request, Reply)>,
    change: Change,
    frame: Frame,
    last: Option<String>,
}

struct ServerSide {
    tx: Sender<(Request, Reply)>,
    ctx: egui::Context,
    change: Change,
    frame: Frame,
    /// One frame at a time: the bytes in `frame` belong to the request holding this.
    frame_guard: Mutex<()>,
    /// Random per process: seq starts again at 0 when the twin restarts (ADR 0066 dev loop).
    epoch: String,
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
        // since > seq: the client counted another process (a restarted twin). Answer at once
        // with the current change and the epoch, so it resets rather than waits.
        if since > guard.0 {
            return (guard.0, guard.1.clone());
        }
        let (guard, _) = cv
            .wait_timeout_while(guard, timeout, |(seq, _)| *seq <= since)
            .unwrap_or_else(|e| e.into_inner());
        if guard.0 > since {
            (guard.0, guard.1.clone())
        } else {
            (guard.0, "null".to_string())
        }
    }

    fn epoch(&self) -> Option<&str> {
        Some(&self.epoch)
    }

    fn frame(&self, w: u32, h: u32) -> Result<(Vec<u8>, String), String> {
        let _one = self.frame_guard.lock().unwrap_or_else(|e| e.into_inner());
        let timing = self.call(Request {
            op: "frame".into(),
            name: "-".into(),
            body: format!("{w},{h}"),
        })?;
        let bytes = std::mem::take(&mut *self.frame.lock().unwrap_or_else(|e| e.into_inner()));
        Ok((bytes, timing))
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
    let frame: Frame = Arc::default();
    let side = ServerSide {
        tx,
        ctx: ctx.clone(),
        change: change.clone(),
        frame: frame.clone(),
        frame_guard: Mutex::new(()),
        epoch: random_token(),
    };
    let cfg = serve::Config {
        port,
        token: token.clone(),
        allowed_origins: allowed_origins(std::env::var("BIMBLE_ORIGINS").ok().as_deref()),
    };
    match serve::start(cfg, Arc::new(side)) {
        Ok(port) => {
            eprintln!("bimble twin: listening {port} token {token}{note}");
            Some(Twin {
                rx,
                change,
                frame,
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

/// The twin without a window: no eframe. A plain loop waits up to 50 ms for a request,
/// finishes a pending load (`poll_loading`), serves the call and publishes the change.
/// Edits rebuild the scene inside `call` (`try_apply`, `undo_redo`). The view ops (`frame`,
/// `orbit`, `pan`, `zoom`, viewport spike) are served here, not by `call`: `frame` draws the
/// 3D view offscreen and reads it back, the GPU upload happening on the first frame.
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
    // Every frame is the whole picture: no lean (simplified) picture while the camera moves.
    app.settings.moving_fps = 0;
    if let Some(f) = file {
        app.open(f, &ctx);
    }
    let mut twin = start(&ctx, " (headless)").ok_or("the twin server did not start")?;
    loop {
        let req = twin.rx.recv_timeout(Duration::from_millis(50));
        app.poll_loading();
        match req {
            Ok((req, reply)) => {
                if let Some(r) = view_op(&mut app, &twin.frame, &req) {
                    let _ = reply.send(r);
                    continue; // the change JSON has no camera: nothing to publish
                }
                let _ = reply.send(app.call(&ctx, &req.op, &req.name, &req.body));
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Ok(()),
        }
        app.sync_tabs(); // as wasm does before `bridge::notify`: the "tab" field stays fresh
        twin.publish(app.change_json());
    }
}

/// The view ops of the headless twin; None for every other op (served by `Viewer::call`).
fn view_op(
    app: &mut crate::Viewer,
    frame: &Frame,
    req: &Request,
) -> Option<Result<String, String>> {
    let num = |v: &serde_json::Value, k: &str| -> Result<f32, String> {
        v.get(k)
            .and_then(serde_json::Value::as_f64)
            .filter(|x| x.is_finite())
            .map(|x| x as f32)
            .ok_or_else(|| format!("{}: missing number {k:?}", req.op))
    };
    let json = || -> Result<serde_json::Value, String> {
        serde_json::from_str(&req.body).map_err(|e| format!("{}: bad JSON: {e}", req.op))
    };
    let r = match req.op.as_str() {
        "frame" => render_frame(app, frame, &req.body),
        "orbit" => json().and_then(|v| {
            app.camera
                .orbit(num(&v, "dx")? * 0.008, num(&v, "dy")? * 0.008);
            app.needs_render = true;
            Ok(String::new())
        }),
        "pan" => json().and_then(|v| {
            app.camera.pan(num(&v, "dx")?, num(&v, "dy")?);
            app.needs_render = true;
            Ok(String::new())
        }),
        "zoom" => json().and_then(|v| {
            app.camera.zoom((-num(&v, "scroll")? * 0.0015).exp());
            app.needs_render = true;
            Ok(String::new())
        }),
        _ => return None,
    };
    Some(r)
}

/// Draws the current 3D view at `w,h` px and puts its RGBA8 rows (top first, no padding) in
/// `frame`. Replies with the timing: `render=<ms>;read=<ms>`.
fn render_frame(app: &mut crate::Viewer, frame: &Frame, body: &str) -> Result<String, String> {
    let size = body
        .split_once(',')
        .and_then(|(w, h)| Some((w.trim().parse::<u32>().ok()?, h.trim().parse::<u32>().ok()?)))
        .filter(|&(w, h)| (1..=4096).contains(&w) && (1..=4096).contains(&h))
        .ok_or_else(|| format!("frame: bad size {body:?}"))?;
    if app.fit_pending {
        app.fit(size.0 as f32 / size.1 as f32);
        app.fit_pending = false;
    }
    let t0 = std::time::Instant::now();
    app.render_view(size, 1.0);
    let t1 = std::time::Instant::now();
    let target = app.target.as_ref().ok_or("frame: no target")?;
    let bytes = bimble_render::read_texture(&app.device, &app.queue, &target.color)
        .map_err(|e| format!("frame: readback: {e}"))?;
    let t2 = std::time::Instant::now();
    *frame.lock().unwrap_or_else(|e| e.into_inner()) = bytes;
    let ms = |a: std::time::Instant, b: std::time::Instant| (b - a).as_secs_f64() * 1e3;
    Ok(format!("render={:.2};read={:.2}", ms(t0, t1), ms(t1, t2)))
}

#[cfg(test)]
mod tests {
    use super::allowed_origins;

    #[test]
    fn origins_default_and_loopback_extras_only() {
        assert_eq!(
            allowed_origins(None),
            ["http://localhost:5173", "http://127.0.0.1:5173"]
        );
        let v = allowed_origins(Some(
            " http://localhost:5203,http://127.0.0.1:5203,http://evil.example:5203,https://localhost:1,http://localhost:x,http://localhost:5173,",
        ));
        assert_eq!(
            v,
            [
                "http://localhost:5173",
                "http://127.0.0.1:5173",
                "http://localhost:5203",
                "http://127.0.0.1:5203"
            ]
        );
    }
}
