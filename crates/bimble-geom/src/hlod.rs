//! Simplified stand-ins for what is far (HLOD, ADR 0036 #9): an octree over the scene, each
//! cell with a proxy mesh standing for every element under it, and the error of that proxy in
//! mm. A renderer draws a cell's proxy instead of its elements when the error projects under a
//! pixel (3D Tiles' `geometricError`), else descends; under a leaf it draws the elements.
//!
//! Why an octree and not the box tree's nodes (`bvh`): cubes halve per level, so the error does
//! too (one error per depth, like 3D Tiles' implicit tiling or map zoom levels), where the box
//! tree's median splits give boxes of any shape and two to three times the levels.
//!
//! Simplification is vertex clustering on a grid (Rossignac & Borrel 1993): every vertex moves
//! to the mean of the vertices in its grid cell, and triangles that lose a corner are dropped.
//! Flat-shaded BIM boxes larger than a grid cell keep their exact corners. All grids are one
//! lattice (the root cube's corner, power-of-two sizes), so a parent clusters its children's
//! proxies by halving their grid keys, and that equals clustering the originals: the error
//! does not add up level after level. Each original triangle is read once, by its leaf.
//!
//! The error is one-directional. Every point of a proxy is within `error_mm` of the original
//! (it is a weighted mean of points each that close to its own original). The other way does
//! not hold for what is thinner than a grid cell: a pipe thinner than the grid in two axes
//! collapses into degenerate triangles and is left out. It is then thinner than the error, so
//! under a pixel when the error is.
//!
//! Proxies are kept per material family (the renderer's style slot): families never merge.
//! They ignore what the viewer hides (classes, families, the section box): a cell with
//! something hidden under it must be descended.
//!
//! Untrusted input: depth, proxy size and coarsening are bounded; non-finite vertices are
//! skipped; no recursion.

use crate::bvh::Aabb;
use crate::{EdgeNormals, Mesh, Segment};
use bimble_core::MaterialCategory;
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet};

/// How the tree is cut.
#[derive(Clone, Copy, Debug)]
pub struct Options {
    /// A cell with more drawn triangles than this (copies counted) is split.
    pub leaf_triangles: usize,
    /// Grid cells along a tree cell's side: the error is `side / resolution × √3`.
    pub resolution: u32,
    /// Most triangles in one proxy; the grid is coarsened (doubled) until it fits.
    pub max_proxy_triangles: usize,
    /// A proxy is kept only when it has at most this share of the triangles it stands for.
    pub max_ratio: f64,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            leaf_triangles: 65_536,
            resolution: 64,
            max_proxy_triangles: 65_536,
            max_ratio: 0.25,
        }
    }
}

/// Deepest cell (the root is 0).
pub const MAX_DEPTH: u32 = 16;

/// One octree cell.
#[derive(Clone, Debug)]
pub struct Cell {
    pub depth: u32,
    /// The placed bounds of its elements (scene coordinates); they may reach out of its cube.
    pub bounds: Aabb,
    /// Most a proxy point is from the original surface, mm (see the module notes).
    pub error_mm: f32,
    /// The proxy, one mesh per material family (scene coordinates, with crease edges). Empty
    /// when it would not be much smaller than what it stands for: descend instead.
    pub proxy: Vec<(MaterialCategory, Mesh)>,
    /// Indices into `Hlod::cells`; empty for a leaf.
    pub children: Vec<u32>,
    /// A leaf's elements (indices into `Scene::elements`); empty for an inner cell.
    pub elements: Vec<u32>,
    /// Triangles drawn without the proxy (every element under it, copies counted).
    pub original_triangles: u64,
}

impl Cell {
    pub fn proxy_triangles(&self) -> usize {
        self.proxy.iter().map(|(_, m)| m.triangle_count()).sum()
    }

    /// Bytes of the proxy as stored (positions, indices, edges and their normals).
    pub fn proxy_bytes(&self) -> usize {
        self.proxy
            .iter()
            .map(|(_, m)| {
                m.positions.len() * size_of::<[f32; 3]>()
                    + m.indices.len() * size_of::<u32>()
                    + m.edges.len() * size_of::<Segment>()
                    + m.edge_normals.len() * size_of::<EdgeNormals>()
            })
            .sum()
    }
}

/// The tree: `cells[0]` is the root (no cells for an empty scene).
#[derive(Clone, Debug, Default)]
pub struct Hlod {
    pub cells: Vec<Cell>,
}

/// Builds the tree and its proxies (leaves first, then each depth, cells in parallel).
pub fn build(scene: &crate::Scene, opt: &Options) -> Hlod {
    let boxes = crate::bvh::element_boxes(scene);
    let all = boxes.iter().fold(Aabb::EMPTY, |b, x| b.union(x));
    if all.is_empty() {
        return Hlod::default();
    }
    let side = (0..3)
        .map(|k| f64::from(all.max[k] - all.min[k]))
        .fold(1.0, f64::max);
    let origin = all.min.map(f64::from);
    let resolution = f64::from(opt.resolution.max(1));
    let tris: Vec<u64> = scene
        .elements
        .iter()
        .map(|e| e.triangle_count() as u64)
        .collect();

    // Top-down: split by the octant of each element's centre. `corner` is the cube's integer
    // position at its depth.
    let mut cells: Vec<Cell> = vec![];
    let mut corners: Vec<[u64; 3]> = vec![];
    let root: Vec<u32> = (0..boxes.len() as u32)
        .filter(|&i| !boxes[i as usize].is_empty())
        .collect();
    let mut stack = vec![(0u32, [0u64; 3], root, None::<usize>)];
    while let Some((depth, corner, mut elements, parent)) = stack.pop() {
        let me = cells.len();
        if let Some(p) = parent {
            cells[p].children.push(me as u32);
        }
        let original: u64 = elements.iter().map(|&i| tris[i as usize]).sum();
        let bounds = elements
            .iter()
            .fold(Aabb::EMPTY, |b, &i| b.union(&boxes[i as usize]));
        corners.push(corner);
        let split = original > opt.leaf_triangles as u64 && depth < MAX_DEPTH && elements.len() > 1;
        cells.push(Cell {
            depth,
            bounds,
            original_triangles: original,
            elements: if split {
                vec![]
            } else {
                std::mem::take(&mut elements)
            },
            error_mm: 0.0,
            proxy: vec![],
            children: vec![],
        });
        if !split {
            continue;
        }
        let half = side / f64::from(1u32 << (depth + 1));
        let mut octants: [Vec<u32>; 8] = Default::default();
        for i in elements {
            let b = &boxes[i as usize];
            let o = (0..3).fold(0, |o, k| {
                let c = (f64::from(b.min[k]) + f64::from(b.max[k])) / 2.0 - origin[k];
                let mid = (2 * corner[k] + 1) as f64 * half;
                o | (usize::from(c >= mid) << k)
            });
            octants[o].push(i);
        }
        for (o, els) in octants.into_iter().enumerate() {
            if !els.is_empty() {
                let c = [0, 1, 2].map(|k| 2 * corner[k] + (o as u64 >> k & 1));
                stack.push((depth + 1, c, els, Some(me)));
            }
        }
    }

    // Bottom-up, deepest first. A cell's clusters live until its parent has used them.
    let lattice = Lattice {
        origin,
        g0: side / resolution / f64::from(1u32 << MAX_DEPTH),
    };
    let mut clusters: Vec<Option<Clusters>> = vec![None; cells.len()];
    let deepest = cells.iter().map(|c| c.depth).max().unwrap_or(0);
    for depth in (0..=deepest).rev() {
        let at: Vec<usize> = (0..cells.len())
            .filter(|&i| cells[i].depth == depth)
            .collect();
        let built: Vec<Clusters> = at
            .par_iter()
            .map(|&i| {
                let c = &cells[i];
                let e = MAX_DEPTH - depth;
                let mut g = Grid::new(e);
                let max = opt.max_proxy_triangles;
                for &el in &c.elements {
                    g.add_element(&scene.elements[el as usize], &lattice, max);
                }
                let children: Vec<&Clusters> = c
                    .children
                    .iter()
                    .filter_map(|&ch| clusters[ch as usize].as_ref())
                    .collect();
                // A child coarsened to fit its cap: the parent is no finer.
                g.e = children.iter().map(|cl| cl.e).fold(g.e, u32::max);
                for cl in children {
                    g.add_clusters(cl);
                    g.fit(max);
                }
                g.finish()
            })
            .collect();
        for (&i, cl) in at.iter().zip(built) {
            for ch in cells[i].children.clone() {
                clusters[ch as usize] = None;
            }
            let c = &mut cells[i];
            c.error_mm = (lattice.size(cl.e) * 3f64.sqrt()) as f32;
            if cl.triangles() as f64 <= opt.max_ratio * c.original_triangles as f64 {
                c.proxy = cl.meshes();
            }
            clusters[i] = Some(cl);
        }
    }
    Hlod { cells }
}

/// The grids: cell size `g0 × 2^e` from one corner.
struct Lattice {
    origin: [f64; 3],
    g0: f64,
}

impl Lattice {
    fn size(&self, e: u32) -> f64 {
        self.g0 * 2f64.powi(e as i32)
    }

    fn key(&self, p: [f32; 3], e: u32) -> Option<Key> {
        let g = self.size(e);
        p.iter().all(|c| c.is_finite()).then(|| {
            // `as` saturates: a far-off vertex lands in an edge cell, never panics.
            [0, 1, 2].map(|k| ((f64::from(p[k]) - self.origin[k]) / g).floor() as i64)
        })
    }
}

type Key = [i64; 3];

/// A material family's clusters while building: sum and count of the vertices in each grid
/// cell, and the triangles between cells.
#[derive(Default)]
struct Family {
    verts: HashMap<Key, ([f64; 3], u64)>,
    tris: HashSet<[Key; 3]>,
}

impl Family {
    fn add_vertex(&mut self, k: Key, sum: [f64; 3], n: u64) {
        let v = self.verts.entry(k).or_insert(([0.0; 3], 0));
        v.0 = [0, 1, 2].map(|c| v.0[c] + sum[c]);
        v.1 += n;
    }

    /// Dropped when two corners share a cell; kept once whatever corner it starts from.
    fn add_triangle(&mut self, t: [Key; 3]) {
        if t[0] == t[1] || t[1] == t[2] || t[0] == t[2] {
            return;
        }
        let r = (0..3).min_by_key(|&i| t[i]).unwrap_or(0);
        self.tris.insert([t[r], t[(r + 1) % 3], t[(r + 2) % 3]]);
    }
}

struct Grid {
    e: u32,
    families: BTreeMap<u8, Family>,
}

fn family(c: MaterialCategory) -> u8 {
    MaterialCategory::ALL
        .iter()
        .position(|&x| x == c)
        .unwrap_or(MaterialCategory::ALL.len() - 1) as u8
}

impl Grid {
    fn new(e: u32) -> Grid {
        Grid {
            e,
            families: BTreeMap::new(),
        }
    }

    fn triangles(&self) -> usize {
        self.families.values().map(|f| f.tris.len()).sum()
    }

    /// An element's placed triangles (copies one at a time: nothing kept but the clusters),
    /// fitting the cap every `max` triangles so a huge element stays bounded too.
    fn add_element(&mut self, el: &crate::SceneElement, lattice: &Lattice, max: usize) {
        let m = el.world_mesh();
        for chunk in m.indices.as_chunks::<3>().0.chunks(max.max(1)) {
            self.add_triangles(el.category, &m.positions, chunk, lattice);
            self.fit(max);
        }
    }

    fn add_triangles(
        &mut self,
        category: MaterialCategory,
        positions: &[[f32; 3]],
        tris: &[[u32; 3]],
        lattice: &Lattice,
    ) {
        let f = self.families.entry(family(category)).or_default();
        // Each vertex counts once in its cluster's mean, however many triangles use it.
        let mut keys: HashMap<u32, Option<Key>> = HashMap::new();
        let e = self.e;
        let mut key = |i: u32| {
            *keys.entry(i).or_insert_with(|| {
                let p = *positions.get(i as usize)?;
                let k = lattice.key(p, e)?;
                f.add_vertex(k, p.map(f64::from), 1);
                Some(k)
            })
        };
        let keyed: Vec<[Option<Key>; 3]> = tris.iter().map(|t| t.map(&mut key)).collect();
        for k in keyed {
            if let [Some(a), Some(b), Some(c)] = k {
                f.add_triangle([a, b, c]);
            }
        }
    }

    /// A child's clusters, on this coarser grid.
    fn add_clusters(&mut self, cl: &Clusters) {
        let shift = (self.e.saturating_sub(cl.e)).min(63);
        let up = |k: Key| k.map(|c| c >> shift);
        for (fam, part) in &cl.families {
            let f = self.families.entry(*fam).or_default();
            for (k, sum, n) in &part.verts {
                f.add_vertex(up(*k), *sum, *n);
            }
            for t in &part.tris {
                f.add_triangle(t.map(|i| up(part.verts[i as usize].0)));
            }
        }
    }

    /// Coarsens the grid until the triangles fit (each step halves the cells per axis; at
    /// a few cells per side little is left, so this ends within a few dozen steps).
    fn fit(&mut self, max: usize) {
        while self.triangles() > max && self.e < 62 {
            self.e += 1;
            for f in self.families.values_mut() {
                let old = std::mem::take(f);
                let up = |k: Key| k.map(|c| c >> 1);
                for (k, (sum, n)) in old.verts {
                    f.add_vertex(up(k), sum, n);
                }
                for t in old.tris {
                    f.add_triangle(t.map(up));
                }
            }
        }
    }

    /// Sorted (deterministic) and compact.
    fn finish(self) -> Clusters {
        let families = self
            .families
            .into_iter()
            .filter(|(_, f)| !f.tris.is_empty())
            .map(|(fam, f)| {
                let mut verts: Vec<(Key, [f64; 3], u64)> =
                    f.verts.into_iter().map(|(k, (s, n))| (k, s, n)).collect();
                verts.sort_unstable_by_key(|v| v.0);
                let at = |k: &Key| verts.binary_search_by_key(k, |v| v.0).unwrap_or(0) as u32;
                let mut tris: Vec<[u32; 3]> = f.tris.iter().map(|t| t.map(|k| at(&k))).collect();
                tris.sort_unstable();
                (fam, Part { verts, tris })
            })
            .collect();
        Clusters {
            e: self.e,
            families,
        }
    }
}

/// A cell's clusters, kept for its parent.
#[derive(Clone)]
struct Clusters {
    e: u32,
    families: Vec<(u8, Part)>,
}

#[derive(Clone)]
struct Part {
    /// Grid key, sum of the original vertices in it, their count.
    verts: Vec<(Key, [f64; 3], u64)>,
    tris: Vec<[u32; 3]>,
}

impl Clusters {
    fn triangles(&self) -> usize {
        self.families.iter().map(|(_, p)| p.tris.len()).sum()
    }

    /// Vertices at their cluster's mean; unused ones dropped; crease edges for the outlines.
    fn meshes(&self) -> Vec<(MaterialCategory, Mesh)> {
        self.families
            .iter()
            .map(|(fam, p)| {
                let mut used = vec![u32::MAX; p.verts.len()];
                let mut m = Mesh::default();
                for t in &p.tris {
                    for &i in t {
                        if used[i as usize] == u32::MAX {
                            used[i as usize] = m.positions.len() as u32;
                            let (_, s, n) = p.verts[i as usize];
                            m.positions.push(s.map(|c| (c / n.max(1) as f64) as f32));
                        }
                        m.indices.push(used[i as usize]);
                    }
                }
                (m.edges, m.edge_normals) = crate::scene::crease_edges_with_normals(&m, 20.0);
                (MaterialCategory::ALL[*fam as usize], m)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Scene;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    type P = [f64; 3];

    fn sub(a: P, b: P) -> P {
        [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
    }
    fn dot(a: P, b: P) -> f64 {
        a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    }

    /// Distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5).
    fn to_triangle(p: P, [a, b, c]: [P; 3]) -> f64 {
        let (ab, ac, ap) = (sub(b, a), sub(c, a), sub(p, a));
        let at = |u: f64, v: f64| [0, 1, 2].map(|k| a[k] + u * ab[k] + v * ac[k]);
        let q = {
            let (d1, d2) = (dot(ab, ap), dot(ac, ap));
            let bp = sub(p, b);
            let (d3, d4) = (dot(ab, bp), dot(ac, bp));
            let cp = sub(p, c);
            let (d5, d6) = (dot(ab, cp), dot(ac, cp));
            let (va, vb, vc) = (d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2);
            if d1 <= 0.0 && d2 <= 0.0 {
                a
            } else if d3 >= 0.0 && d4 <= d3 {
                b
            } else if d6 >= 0.0 && d5 <= d6 {
                c
            } else if vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 {
                at(d1 / (d1 - d3), 0.0)
            } else if vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 {
                at(0.0, d2 / (d2 - d6))
            } else if va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0 {
                let w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                [0, 1, 2].map(|k| b[k] + w * (c[k] - b[k]))
            } else {
                let d = 1.0 / (va + vb + vc);
                at(vb * d, vc * d)
            }
        };
        let d = sub(p, q);
        dot(d, d).sqrt()
    }

    fn triangles(m: &Mesh) -> Vec<[P; 3]> {
        m.indices
            .as_chunks::<3>()
            .0
            .iter()
            .map(|t| t.map(|i| m.positions[i as usize].map(f64::from)))
            .collect()
    }

    /// Corners, edge midpoints and the centroid of each triangle.
    fn samples(ts: &[[P; 3]]) -> Vec<P> {
        let mix = |w: [f64; 3], t: &[P; 3]| {
            [0, 1, 2].map(|k| w[0] * t[0][k] + w[1] * t[1][k] + w[2] * t[2][k])
        };
        let ws = [
            [1.0, 0.0, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, 0.0, 1.0],
            [0.5, 0.5, 0.0],
            [0.0, 0.5, 0.5],
            [0.5, 0.0, 0.5],
            [1.0 / 3.0; 3],
        ];
        ts.iter().flat_map(|t| ws.map(|w| mix(w, t))).collect()
    }

    fn nearest(p: P, ts: &[[P; 3]]) -> f64 {
        ts.iter()
            .map(|&t| to_triangle(p, t))
            .fold(f64::INFINITY, f64::min)
    }

    fn frame() -> Scene {
        Scene::from_model(&concrete_frame(&FrameSpec {
            bays_x: 4,
            bays_y: 3,
            storeys: 3,
            ..Default::default()
        }))
    }

    /// Every proxy point is within its cell's error of the original; elements thicker than
    /// twice the error everywhere are within it of the proxy; triangles drop level by level.
    #[test]
    fn proxies_stay_within_their_error() {
        let scene = frame();
        let opt = Options {
            leaf_triangles: 64,
            resolution: 16,
            max_ratio: 1.0,
            ..Default::default()
        };
        let h = build(&scene, &opt);
        let deepest = h.cells.iter().map(|c| c.depth).max().unwrap();
        assert!(deepest >= 3, "{deepest} levels");
        // Elements under each cell (leaves hold them; children come after their parent).
        let mut under: Vec<Vec<u32>> = h.cells.iter().map(|c| c.elements.clone()).collect();
        for i in (0..h.cells.len()).rev() {
            for &c in &h.cells[i].children {
                let e = under[c as usize].clone();
                under[i].extend(e);
            }
        }

        for (i, c) in h.cells.iter().enumerate() {
            let proxy: Vec<[P; 3]> = c.proxy.iter().flat_map(|(_, m)| triangles(m)).collect();
            assert!(!proxy.is_empty(), "cell {i} has no proxy");
            let err = f64::from(c.error_mm) + 1e-3;
            // Against this cell's own elements: a proxy built from another cell's fails.
            let original: Vec<[P; 3]> = under[i]
                .iter()
                .flat_map(|&el| triangles(&scene.elements[el as usize].world_mesh()))
                .collect();
            let worst = samples(&proxy)
                .into_iter()
                .map(|p| nearest(p, &original))
                .fold(0.0, f64::max);
            assert!(worst <= err, "cell {i}: proxy {worst} mm off, error {err}");
            for &el in &under[i] {
                let e = &scene.elements[el as usize];
                let b = e.bounds();
                if (0..3).any(|k| f64::from(b.max[k] - b.min[k]) < 2.0 * err) {
                    continue;
                }
                let worst = samples(&triangles(&e.world_mesh()))
                    .into_iter()
                    .map(|p| nearest(p, &proxy))
                    .fold(0.0, f64::max);
                assert!(
                    worst <= err,
                    "cell {i}: element {} {worst} mm from the proxy, error {err}",
                    e.name
                );
            }
        }

        // What is drawn when every cell at depth `d` (or a shallower leaf) draws its proxy.
        let per_depth = |d: u32| -> usize {
            h.cells
                .iter()
                .filter(|c| c.depth == d || (c.depth < d && c.children.is_empty()))
                .map(Cell::proxy_triangles)
                .sum()
        };
        for d in 1..=deepest {
            assert!(per_depth(d - 1) <= per_depth(d), "depth {d}");
        }
        assert!(per_depth(0) < scene.triangle_count() / 2);
    }

    /// Cut to fit: the grid coarsens until the proxy is under the cap, and the error says so.
    #[test]
    fn proxies_fit_their_cap() {
        let scene = frame();
        let opt = Options {
            leaf_triangles: usize::MAX,
            resolution: 4096,
            max_proxy_triangles: 100,
            max_ratio: 1.0,
        };
        let h = build(&scene, &opt);
        assert_eq!(h.cells.len(), 1);
        let root = &h.cells[0];
        assert!(root.proxy_triangles() <= 100);
        assert_eq!(root.elements.len(), scene.elements.len());
        let side = (0..3)
            .map(|k| root.bounds.max[k] - root.bounds.min[k])
            .fold(0.0, f32::max);
        assert!(root.error_mm > side / 4096.0 * 3f32.sqrt());
    }

    #[test]
    fn empty_scene_has_no_cells() {
        assert!(
            build(&Scene::default(), &Options::default())
                .cells
                .is_empty()
        );
    }
}
