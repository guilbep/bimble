//! Simplified stand-ins for far parts of the model, drawn in place of their elements (ADR 0036
//! #9, the renderer half): `bimble_geom::hlod` builds them per octree cell; here they go to the
//! GPU once per model, and each frame the tree is cut where a cell's error stays under a pixel
//! budget. Elements under a drawn proxy are skipped.

use crate::{Faces, IndexedChunks, LineVertex, Style, style_gpu};
use bimble_geom::hlod::Hlod;
use bimble_geom::{Scene, SceneElement};
use glam::Vec3;

/// Where something sits in a list of buffers: (buffer, range).
pub(crate) type Run = (usize, std::ops::Range<u32>);

pub(crate) struct ProxyCell {
    pub(crate) bounds: [Vec3; 2],
    pub(crate) error_mm: f32,
    pub(crate) children: Vec<u32>,
    /// Its proxy, faces and edges: (buffer, index or vertex range); None: descend.
    pub(crate) draw: Option<(Run, Run)>,
    /// Leaves under it, by their numbers: `leaves.0..leaves.1`.
    pub(crate) leaves: (u32, u32),
}

/// Proxies of a model on the GPU (shared by every upload of it: restyling doesn't rebuild them).
pub struct GpuProxies {
    pub(crate) cells: Vec<ProxyCell>,
    pub(crate) faces: Vec<Faces>,
    pub(crate) lines: Vec<(wgpu::Buffer, u32)>,
    /// Per scene element, the number of its leaf (`u32::MAX`: none).
    pub(crate) leaf_of: Vec<u32>,
    pub(crate) leaf_count: u32,
}

impl GpuProxies {
    /// Bytes on the GPU.
    pub fn gpu_bytes(&self) -> u64 {
        self.faces
            .iter()
            .map(|f| f.vertices.size() + f.indices.size())
            .chain(self.lines.iter().map(|(b, _)| b.size()))
            .sum()
    }

    pub(crate) fn upload(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        scene: &Scene,
        hlod: &Hlod,
    ) -> GpuProxies {
        let mut faces = IndexedChunks::new(device, queue, "bimble proxy faces");
        let mut lines = crate::Chunks::<[f32; 3]>::new(device, queue, "bimble proxy lines");
        let mut cells: Vec<ProxyCell> = Vec::with_capacity(hlod.cells.len());
        for c in &hlod.cells {
            let (mut fv, mut fi, mut lv) = (Vec::new(), Vec::new(), Vec::new());
            for (cat, m) in &c.proxy {
                crate::push_faces(m, style_gpu::slot(*cat), &mut fv, &mut fi);
                crate::push_lines(m, false, |_: LineVertex| {}, |p| lv.push(p));
            }
            let draw = if fi.is_empty() {
                None
            } else {
                faces.push_run(&fv, &fi).zip(lines.push_run(&lv))
            };
            cells.push(ProxyCell {
                bounds: [Vec3::from(c.bounds.min), Vec3::from(c.bounds.max)],
                error_mm: c.error_mm,
                children: c.children.clone(),
                draw,
                leaves: (0, 0),
            });
        }
        // Leaves numbered in depth-first order, so each cell's are a range.
        let mut leaf_of = vec![u32::MAX; scene.elements.len()];
        let mut next = 0u32;
        // (cell, children visited)
        let mut stack = vec![(0u32, false)];
        while let Some((k, done)) = stack.pop() {
            let Some(cell) = cells.get(k as usize) else {
                continue;
            };
            if done {
                let end = cell
                    .children
                    .iter()
                    .map(|&c| cells[c as usize].leaves.1)
                    .max()
                    .unwrap_or(next);
                let start = cell
                    .children
                    .iter()
                    .map(|&c| cells[c as usize].leaves.0)
                    .min()
                    .unwrap_or(next);
                cells[k as usize].leaves = (start, end);
                continue;
            }
            if cell.children.is_empty() {
                for &e in &hlod.cells[k as usize].elements {
                    if let Some(l) = leaf_of.get_mut(e as usize) {
                        *l = next;
                    }
                }
                cells[k as usize].leaves = (next, next + 1);
                next += 1;
                continue;
            }
            stack.push((k, true));
            for &c in cell.children.iter().rev() {
                stack.push((c, false));
            }
        }
        GpuProxies {
            cells,
            faces: faces.finish(),
            lines: lines.finish(),
            leaf_of,
            leaf_count: next,
        }
    }

    /// Which cells may stand for their elements in an upload: those under which every element
    /// is drawn plainly (a hidden, selected or compared element must show as it is).
    pub(crate) fn usable(
        &self,
        scene: &Scene,
        style: impl Fn(usize, &SceneElement) -> Style,
    ) -> Vec<bool> {
        let mut plain_leaf = vec![true; self.leaf_count as usize];
        for (i, e) in scene.elements.iter().enumerate() {
            let l = self.leaf_of.get(i).copied().unwrap_or(u32::MAX);
            if l != u32::MAX && style(i, e) != Style::Normal {
                plain_leaf[l as usize] = false;
            }
        }
        self.cells
            .iter()
            .map(|c| {
                plain_leaf[c.leaves.0 as usize..c.leaves.1 as usize]
                    .iter()
                    .all(|&p| p)
            })
            .collect()
    }
}
