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
            eprintln!("bimble twin: listening {port} token {token}");
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
