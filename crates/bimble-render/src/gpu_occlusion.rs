//! Occlusion culling with GPU queries (ADR 0036 #1), reusing last frame's answers (the idea of
//! CHC++, at the coarsest grain): each merged cluster and each shape draw is wrapped in an
//! occlusion query; one that showed no pixel is left out of the next frame, and only its box is
//! drawn (no colour, no depth) to learn when it shows again. Answers arrive a frame late, so a
//! newly uncovered part pops in a frame later; the viewer draws again until nothing comes back.
//! A box is larger than its part: once the part's own draw found it hidden from a view, its box
//! seen from that same view doesn't bring it back (else a still view would redraw forever).
//! Native backends only: wgpu's WebGL2 backend reads query results too early.

use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

/// What a query asked about: a cluster or a shape draw (their index), by its own draw or by its
/// box.
#[derive(Clone, Copy, PartialEq, Debug)]
pub(crate) enum Item {
    Cluster(u32),
    Shape(u32),
    ClusterBox(u32),
    ShapeBox(u32),
}

pub(crate) struct State {
    /// Shown last time it was asked: per cluster, then per shape draw.
    pub(crate) clusters: Vec<bool>,
    pub(crate) shapes: Vec<bool>,
    /// The view from which a part's own draw last found it hidden (see the module notes).
    hidden_at: Vec<Option<u64>>,
    pub(crate) set: wgpu::QuerySet,
    pub(crate) capacity: u32,
    resolve: wgpu::Buffer,
    read: wgpu::Buffer,
    /// The queries of a frame whose answers aren't read yet: what each asked, from which view,
    /// and whether they arrived (0: not yet, 1: yes, 2: failed).
    pending: Option<(Vec<Item>, u64, Arc<AtomicU8>)>,
}

impl State {
    pub(crate) fn new(device: &wgpu::Device, clusters: usize, shapes: usize) -> Option<State> {
        // A query set holds at most 4096 queries (wgpu); a frame asks no more than that (what
        // isn't asked keeps its last answer).
        let capacity = 4096u32;
        if clusters + shapes == 0 {
            return None;
        }
        let bytes = capacity as u64 * 8;
        Some(State {
            clusters: vec![true; clusters],
            shapes: vec![true; shapes],
            hidden_at: vec![None; clusters + shapes],
            set: device.create_query_set(&wgpu::QuerySetDescriptor {
                label: Some("bimble occlusion queries"),
                ty: wgpu::QueryType::Occlusion,
                count: capacity,
            }),
            capacity,
            resolve: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("bimble occlusion resolve"),
                size: bytes,
                usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            }),
            read: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("bimble occlusion read"),
                size: bytes,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            pending: None,
        })
    }

    /// Read the last answers if they have arrived: None while they haven't, else whether
    /// something hidden came back into view (the picture misses it: draw again). What turned
    /// hidden needs no redraw: it was drawn and showed nothing.
    pub(crate) fn harvest(&mut self, device: &wgpu::Device) -> Option<bool> {
        let (items, view, ready) = self.pending.as_ref()?;
        let _ = device.poll(wgpu::PollType::Poll);
        let state = ready.load(Ordering::Acquire);
        if state == 0 {
            return None;
        }
        let (view, nc) = (*view, self.clusters.len());
        let mut back = false;
        if state == 1 {
            {
                let slice = self.read.slice(..items.len() as u64 * 8);
                if let Ok(data) = slice.get_mapped_range() {
                    let counts: &[u64] = bytemuck::cast_slice(&data);
                    // A part may be asked several times (a cluster drawn in runs): it shows if
                    // any of its own draws did.
                    let mut seen: Vec<Option<bool>> = vec![None; self.hidden_at.len()];
                    for (it, &n) in items.iter().zip(counts) {
                        let (k, own) = match *it {
                            Item::Cluster(c) => (c as usize, true),
                            Item::Shape(s) => (nc + s as usize, true),
                            Item::ClusterBox(c) => (c as usize, false),
                            Item::ShapeBox(s) => (nc + s as usize, false),
                        };
                        let Some(slot) = seen.get_mut(k) else {
                            continue;
                        };
                        if own {
                            *slot = Some(slot.unwrap_or(false) || n > 0);
                        } else if n > 0 && self.hidden_at[k] != Some(view) {
                            *slot = Some(true);
                        }
                    }
                    for (k, s) in seen.into_iter().enumerate() {
                        let Some(shown) = s else { continue };
                        let v = if k < nc {
                            &mut self.clusters[k]
                        } else {
                            &mut self.shapes[k - nc]
                        };
                        back |= shown && !*v;
                        *v = shown;
                        if !shown {
                            self.hidden_at[k] = Some(view);
                        } else if self.hidden_at[k] != Some(view) {
                            self.hidden_at[k] = None;
                        }
                    }
                }
            }
            self.read.unmap();
        }
        self.pending = None;
        Some(back)
    }

    /// Whether answers are still awaited.
    pub(crate) fn waiting(&self) -> bool {
        self.pending.is_some()
    }

    /// After the frame's queries `items` were encoded into `enc` (indices 0..items.len()).
    pub(crate) fn resolve(&self, enc: &mut wgpu::CommandEncoder, n: u32) {
        if n == 0 {
            return;
        }
        enc.resolve_query_set(&self.set, 0..n, &self.resolve, 0);
        enc.copy_buffer_to_buffer(&self.resolve, 0, &self.read, 0, n as u64 * 8);
    }

    /// After the frame was submitted: ask for its answers. `view` identifies the camera.
    pub(crate) fn wait_for(&mut self, items: Vec<Item>, view: u64) {
        if items.is_empty() {
            return;
        }
        let ready = Arc::new(AtomicU8::new(0));
        let flag = ready.clone();
        self.read
            .slice(..items.len() as u64 * 8)
            .map_async(wgpu::MapMode::Read, move |r| {
                flag.store(if r.is_ok() { 1 } else { 2 }, Ordering::Release);
            });
        self.pending = Some((items, view, ready));
    }
}
