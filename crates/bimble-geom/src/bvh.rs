//! A bounding-volume hierarchy over boxes (a BoxTree: Barequet, Chazelle & Guibas 1996;
//! Zachmann): each node's box holds its two children's. Built over element bounds it answers
//! what touches a ray (picking, snapping), a box (searching, culling) or another tree (the
//! broad phase of clash detection between models) without looking at every element.

/// An axis-aligned box (scene coordinates, mm).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Aabb {
    pub min: [f32; 3],
    pub max: [f32; 3],
}

impl Aabb {
    pub const EMPTY: Aabb = Aabb {
        min: [f32::INFINITY; 3],
        max: [f32::NEG_INFINITY; 3],
    };

    pub fn of_points(ps: &[[f32; 3]]) -> Aabb {
        ps.iter()
            .fold(Aabb::EMPTY, |b, p| b.union(&Aabb { min: *p, max: *p }))
    }

    pub fn union(&self, o: &Aabb) -> Aabb {
        Aabb {
            min: [0, 1, 2].map(|k| self.min[k].min(o.min[k])),
            max: [0, 1, 2].map(|k| self.max[k].max(o.max[k])),
        }
    }

    pub fn is_empty(&self) -> bool {
        (0..3).any(|k| self.min[k] > self.max[k])
    }

    pub fn overlaps(&self, o: &Aabb) -> bool {
        (0..3).all(|k| self.min[k] <= o.max[k] && o.min[k] <= self.max[k])
    }

    fn centre(&self, k: usize) -> f32 {
        (self.min[k] + self.max[k]) * 0.5
    }

    /// Where a ray (origin, 1 / direction) enters the box, if it does before `t_max`.
    pub fn ray_entry(&self, o: [f32; 3], inv_d: [f32; 3], t_max: f32) -> Option<f32> {
        let (mut t0, mut t1) = (0.0f32, t_max);
        for k in 0..3 {
            let (a, b) = (
                (self.min[k] - o[k]) * inv_d[k],
                (self.max[k] - o[k]) * inv_d[k],
            );
            // NaN (0 × ∞, a ray along a face) keeps the other bound.
            t0 = t0.max(a.min(b));
            t1 = t1.min(a.max(b));
        }
        (t0 <= t1).then_some(t0)
    }
}

#[derive(Clone, Copy, Debug)]
struct Node {
    bounds: Aabb,
    /// Leaf: first item in `items`; inner: index of the left child (the right one follows it).
    first: u32,
    /// Items in a leaf; 0 for an inner node.
    count: u32,
}

/// The tree. Items are indices into the boxes it was built from; empty boxes are left out.
#[derive(Clone, Debug, Default)]
pub struct Bvh {
    nodes: Vec<Node>,
    items: Vec<u32>,
    /// The box of each of `items`, in the same order.
    boxes: Vec<Aabb>,
}

const LEAF: usize = 4;

/// The bounds of each element of a scene, for a tree over them (empty for an empty mesh).
pub fn element_boxes(scene: &crate::Scene) -> Vec<Aabb> {
    scene
        .elements
        .iter()
        .map(|e| Aabb::of_points(&e.mesh.positions))
        .collect()
}

impl Bvh {
    /// Median split on the longest axis: O(n log n), depth ~log2(n / 4).
    pub fn build(boxes: &[Aabb]) -> Bvh {
        let mut items: Vec<u32> = (0..boxes.len() as u32)
            .filter(|&i| !boxes[i as usize].is_empty())
            .collect();
        let mut nodes = Vec::with_capacity(2 * items.len() / LEAF + 1);
        if items.is_empty() {
            return Bvh {
                nodes,
                items,
                boxes: vec![],
            };
        }
        let bounds = |s: &[u32]| {
            s.iter()
                .fold(Aabb::EMPTY, |b, &i| b.union(&boxes[i as usize]))
        };
        nodes.push(Node {
            bounds: bounds(&items),
            first: 0,
            count: items.len() as u32,
        });
        // Explicit stack: no recursion on untrusted sizes.
        let mut stack = vec![0usize];
        while let Some(n) = stack.pop() {
            let (first, count) = (nodes[n].first as usize, nodes[n].count as usize);
            if count <= LEAF {
                continue;
            }
            let s = &mut items[first..first + count];
            let b = nodes[n].bounds;
            let k = (0..3)
                .max_by(|&a, &c| (b.max[a] - b.min[a]).total_cmp(&(b.max[c] - b.min[c])))
                .unwrap_or(0);
            let mid = count / 2;
            s.select_nth_unstable_by(mid, |&a, &c| {
                boxes[a as usize]
                    .centre(k)
                    .total_cmp(&boxes[c as usize].centre(k))
            });
            let left = nodes.len();
            for (f, c) in [(first, mid), (first + mid, count - mid)] {
                nodes.push(Node {
                    bounds: bounds(&items[f..f + c]),
                    first: f as u32,
                    count: c as u32,
                });
            }
            nodes[n].first = left as u32;
            nodes[n].count = 0;
            stack.extend([left, left + 1]);
        }
        let boxes = items.iter().map(|&i| boxes[i as usize]).collect();
        Bvh {
            nodes,
            items,
            boxes,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// Items whose boxes a ray meets, nearest box first. `hit(item, entry)` tests an item and
    /// returns the distance of a hit; once something is hit, boxes entered beyond it are
    /// skipped. Returns the nearest hit; between equally near ones, the smallest `T` (an
    /// element index, say), whatever the order they were found in.
    pub fn ray<T: PartialOrd>(
        &self,
        o: [f32; 3],
        d: [f32; 3],
        mut hit: impl FnMut(u32, f32) -> Option<(f32, T)>,
    ) -> Option<(f32, T)> {
        let inv = d.map(|c| 1.0 / c);
        let mut best: Option<(f32, T)> = None;
        let mut stack: Vec<(usize, f32)> = Vec::new();
        if let Some(t) = self
            .nodes
            .first()
            .and_then(|n| n.bounds.ray_entry(o, inv, f32::INFINITY))
        {
            stack.push((0, t));
        }
        while let Some((n, t)) = stack.pop() {
            let limit = best.as_ref().map_or(f32::INFINITY, |b| b.0);
            if t > limit {
                continue;
            }
            let node = &self.nodes[n];
            if node.count > 0 {
                for &i in &self.items[node.first as usize..(node.first + node.count) as usize] {
                    if let Some(h) = hit(i, t)
                        && best
                            .as_ref()
                            .is_none_or(|b| h.0 < b.0 || (h.0 == b.0 && h.1 < b.1))
                    {
                        best = Some(h);
                    }
                }
                continue;
            }
            let l = node.first as usize;
            let limit = best.as_ref().map_or(f32::INFINITY, |b| b.0);
            let near = [l, l + 1].map(|c| self.nodes[c].bounds.ray_entry(o, inv, limit));
            // Push the farther child first, so the nearer one is popped first.
            match near {
                [Some(a), Some(b)] if a <= b => stack.extend([(l + 1, b), (l, a)]),
                [Some(a), Some(b)] => stack.extend([(l, a), (l + 1, b)]),
                [Some(a), None] => stack.push((l, a)),
                [None, Some(b)] => stack.push((l + 1, b)),
                [None, None] => {}
            }
        }
        best
    }

    /// Items whose boxes overlap `b` (searching, culling).
    pub fn overlapping(&self, b: &Aabb, mut f: impl FnMut(u32)) {
        let mut stack = if self.nodes.is_empty() {
            vec![]
        } else {
            vec![0usize]
        };
        while let Some(n) = stack.pop() {
            let node = &self.nodes[n];
            if !node.bounds.overlaps(b) {
                continue;
            }
            if node.count > 0 {
                for k in node.first as usize..(node.first + node.count) as usize {
                    if self.boxes[k].overlaps(b) {
                        f(self.items[k]);
                    }
                }
            } else {
                stack.extend([node.first as usize, node.first as usize + 1]);
            }
        }
    }

    /// Pairs (item here, item there) whose boxes overlap: the broad phase of clash detection
    /// between two models.
    pub fn pairs(&self, other: &Bvh, mut f: impl FnMut(u32, u32)) {
        if self.nodes.is_empty() || other.nodes.is_empty() {
            return;
        }
        let mut stack = vec![(0usize, 0usize)];
        while let Some((a, b)) = stack.pop() {
            let (na, nb) = (&self.nodes[a], &other.nodes[b]);
            if !na.bounds.overlaps(&nb.bounds) {
                continue;
            }
            match (na.count > 0, nb.count > 0) {
                (true, true) => {
                    for i in na.first as usize..(na.first + na.count) as usize {
                        for j in nb.first as usize..(nb.first + nb.count) as usize {
                            if self.boxes[i].overlaps(&other.boxes[j]) {
                                f(self.items[i], other.items[j]);
                            }
                        }
                    }
                }
                // Descend the inner node (the larger one when both are).
                (false, true) => stack.extend([(na.first as usize, b), (na.first as usize + 1, b)]),
                (true, false) => stack.extend([(a, nb.first as usize), (a, nb.first as usize + 1)]),
                (false, false) => {
                    let vol = |x: &Aabb| (0..3).map(|k| x.max[k] - x.min[k]).product::<f32>();
                    if vol(&na.bounds) >= vol(&nb.bounds) {
                        stack.extend([(na.first as usize, b), (na.first as usize + 1, b)]);
                    } else {
                        stack.extend([(a, nb.first as usize), (a, nb.first as usize + 1)]);
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A grid of unit boxes with gaps, and a deterministic generator for queries.
    fn boxes(n: usize, seed: f32) -> Vec<Aabb> {
        (0..n)
            .map(|i| {
                let f = i as f32;
                let p = [
                    (f * 7.31 + seed) % 97.0,
                    (f * 3.17) % 89.0,
                    (f * 1.93 + seed) % 31.0,
                ];
                Aabb {
                    min: p,
                    max: p.map(|c| c + 1.0 + (f % 3.0)),
                }
            })
            .collect()
    }

    /// The tree answers exactly what checking every box answers.
    #[test]
    fn queries_match_brute_force() {
        let a = boxes(5000, 0.0);
        let b = boxes(700, 13.7);
        let (ta, tb) = (Bvh::build(&a), Bvh::build(&b));

        let q = Aabb {
            min: [10.0, 20.0, 5.0],
            max: [40.0, 35.0, 12.0],
        };
        let mut got = vec![];
        ta.overlapping(&q, |i| got.push(i));
        got.sort();
        let want: Vec<u32> = (0..a.len() as u32)
            .filter(|&i| a[i as usize].overlaps(&q))
            .collect();
        assert_eq!(got, want);

        let mut got = vec![];
        ta.pairs(&tb, |i, j| got.push((i, j)));
        got.sort();
        let mut want = vec![];
        for (i, x) in a.iter().enumerate() {
            for (j, y) in b.iter().enumerate() {
                if x.overlaps(y) {
                    want.push((i as u32, j as u32));
                }
            }
        }
        assert_eq!(got, want);

        // Nearest box along rays: entry distance as the "hit".
        for r in 0..50 {
            let o = [-5.0, r as f32 * 1.7, r as f32 * 0.6];
            let d = [1.0, 0.1 * (r % 5) as f32 - 0.2, 0.0];
            let inv = d.map(|c| 1.0 / c);
            let got = ta.ray(o, d, |i, _| {
                a[i as usize]
                    .ray_entry(o, inv, f32::INFINITY)
                    .map(|t| (t, i))
            });
            let want = (0..a.len())
                .filter_map(|i| a[i].ray_entry(o, inv, f32::INFINITY).map(|t| (t, i as u32)))
                .min_by(|x, y| x.0.total_cmp(&y.0));
            assert_eq!(got.map(|g| g.0), want.map(|w| w.0), "ray {r}");
        }
    }
}
