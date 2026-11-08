//! Comparing two models by geometry ("on voit les différences").
//!
//! Two offices model the same building with different GlobalIds, splits and thicknesses: a slab
//! in one piece here, three there; a wall 180 mm thick instead of 200. So elements are matched by
//! space, not by id: an element is *matched* when its bounding box (grown by a tolerance) is
//! mostly covered by elements of the other model.

use crate::Scene;
use std::collections::HashMap;

/// An axis-aligned box, world mm.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Aabb {
    pub min: [f64; 3],
    pub max: [f64; 3],
}

impl Aabb {
    fn grown(&self, t: f64) -> Aabb {
        Aabb {
            min: self.min.map(|v| v - t),
            max: self.max.map(|v| v + t),
        }
    }
    /// Volume, with flat or thin sides counted as 1 mm (a sheet still has an area to cover).
    fn measure(&self) -> f64 {
        (0..3)
            .map(|k| (self.max[k] - self.min[k]).max(1.0))
            .product()
    }
    fn overlap(&self, o: &Aabb) -> f64 {
        (0..3)
            .map(|k| {
                let lo = self.min[k].max(o.min[k]);
                let hi = self.max[k].min(o.max[k]);
                // Touching or flat overlaps count as 1 mm, like `measure`.
                if hi < lo { 0.0 } else { (hi - lo).max(1.0) }
            })
            .product()
    }
}

/// World bounding box of each element.
pub fn element_boxes(scene: &Scene) -> Vec<Option<Aabb>> {
    let o = scene.origin_mm;
    scene
        .elements
        .iter()
        .map(|e| {
            let s = e.bounds();
            (!s.is_empty()).then(|| Aabb {
                min: [0, 1, 2].map(|k| s.min[k] as f64 + o[k]),
                max: [0, 1, 2].map(|k| s.max[k] as f64 + o[k]),
            })
        })
        .collect()
}

/// Matching settings.
#[derive(Clone, Copy, Debug)]
pub struct Tolerance {
    /// Boxes are grown by this much before comparing (mm): absorbs small offsets and thickness
    /// differences.
    pub grow_mm: f64,
    /// Share of an element's box that must be covered by the other model (0..1).
    pub min_cover: f64,
}

impl Default for Tolerance {
    fn default() -> Self {
        Tolerance {
            grow_mm: 20.0,
            min_cover: 0.8,
        }
    }
}

/// Grid cell size for candidate search, mm.
const CELL: f64 = 2000.0;
/// Boxes spanning more cells than this go to a list checked against everything.
const MAX_CELLS: i64 = 4096;

struct Grid {
    cells: HashMap<(i64, i64, i64), Vec<usize>>,
    big: Vec<usize>,
}

fn cell_range(b: &Aabb) -> [(i64, i64); 3] {
    [0, 1, 2].map(|k| {
        (
            (b.min[k] / CELL).floor() as i64,
            (b.max[k] / CELL).floor() as i64,
        )
    })
}

impl Grid {
    fn new(boxes: &[Option<Aabb>]) -> Grid {
        let mut g = Grid {
            cells: HashMap::new(),
            big: Vec::new(),
        };
        for (i, b) in boxes.iter().enumerate() {
            let Some(b) = b else { continue };
            let r = cell_range(b);
            let n: i64 = r.iter().map(|(a, b)| b - a + 1).product();
            if n > MAX_CELLS {
                g.big.push(i);
                continue;
            }
            for x in r[0].0..=r[0].1 {
                for y in r[1].0..=r[1].1 {
                    for z in r[2].0..=r[2].1 {
                        g.cells.entry((x, y, z)).or_default().push(i);
                    }
                }
            }
        }
        g
    }

    /// Indices whose boxes may overlap `b` (each once).
    fn candidates(&self, b: &Aabb) -> Vec<usize> {
        let r = cell_range(b);
        let mut out = self.big.clone();
        let n: i64 = r.iter().map(|(a, b)| b - a + 1).product();
        if n > MAX_CELLS {
            // A huge query: every indexed box.
            out.extend(self.cells.values().flatten().copied());
        } else {
            for x in r[0].0..=r[0].1 {
                for y in r[1].0..=r[1].1 {
                    for z in r[2].0..=r[2].1 {
                        if let Some(v) = self.cells.get(&(x, y, z)) {
                            out.extend_from_slice(v);
                        }
                    }
                }
            }
        }
        out.sort_unstable();
        out.dedup();
        out
    }
}

/// For each element of `a` and of `b`: whether the other model has something in its place.
pub fn match_models(a: &Scene, b: &Scene, t: Tolerance) -> (Vec<bool>, Vec<bool>) {
    let (ba, bb) = (element_boxes(a), element_boxes(b));
    (covered(&ba, &bb, t), covered(&bb, &ba, t))
}

fn covered(these: &[Option<Aabb>], others: &[Option<Aabb>], t: Tolerance) -> Vec<bool> {
    let grid = Grid::new(others);
    these
        .iter()
        .map(|b| {
            let Some(b) = b else { return false };
            let g = b.grown(t.grow_mm);
            let need = g.measure() * t.min_cover;
            let mut have = 0.0;
            for j in grid.candidates(&g) {
                if let Some(o) = &others[j] {
                    have += g.overlap(&o.grown(t.grow_mm));
                    if have >= need {
                        return true;
                    }
                }
            }
            false
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ElementInfo, MeshBuilder, SceneBuilder};
    use bimble_core::MaterialCategory;

    fn boxes(list: &[([f64; 3], [f64; 3])]) -> Scene {
        let mut sb = SceneBuilder::default();
        for (k, (lo, hi)) in list.iter().enumerate() {
            let p = |i: usize| {
                [
                    if i & 1 == 0 { lo[0] } else { hi[0] },
                    if i & 2 == 0 { lo[1] } else { hi[1] },
                    if i & 4 == 0 { lo[2] } else { hi[2] },
                ]
            };
            let mut mb = MeshBuilder::default();
            for q in [
                [0, 2, 3, 1],
                [4, 5, 7, 6],
                [0, 1, 5, 4],
                [2, 6, 7, 3],
                [0, 4, 6, 2],
                [1, 3, 7, 5],
            ] {
                mb.add_triangle([p(q[0]), p(q[1]), p(q[2])]);
                mb.add_triangle([p(q[0]), p(q[2]), p(q[3])]);
            }
            sb.push(
                ElementInfo {
                    global_id: format!("e{k}"),
                    class: "IfcWall".into(),
                    name: String::new(),
                    storey: String::new(),
                    material: String::new(),
                    category: MaterialCategory::Concrete,
                },
                mb,
            );
        }
        sb.finish()
    }

    #[test]
    fn same_walls_with_small_differences_match_and_extra_ones_dont() {
        // Ours: two walls (200 mm) and a column only we have.
        let ours = boxes(&[
            ([0.0, 0.0, 0.0], [5000.0, 200.0, 3000.0]),
            ([0.0, 4000.0, 0.0], [5000.0, 4200.0, 3000.0]),
            ([8000.0, 8000.0, 0.0], [8300.0, 8300.0, 3000.0]),
        ]);
        // Architect: the same walls 180 mm thick and 10 mm off, plus a wall we don't have.
        let theirs = boxes(&[
            ([10.0, 10.0, 0.0], [5010.0, 190.0, 3000.0]),
            ([0.0, 4010.0, 0.0], [5000.0, 4190.0, 3000.0]),
            ([20000.0, 0.0, 0.0], [25000.0, 200.0, 3000.0]),
        ]);
        let (a, b) = match_models(&ours, &theirs, Tolerance::default());
        assert_eq!(a, vec![true, true, false]);
        assert_eq!(b, vec![true, true, false]);
    }

    #[test]
    fn a_slab_split_differently_matches_both_ways() {
        let ours = boxes(&[([0.0, 0.0, 0.0], [12000.0, 6000.0, 250.0])]);
        let theirs = boxes(&[
            ([0.0, 0.0, 0.0], [4000.0, 6000.0, 250.0]),
            ([4000.0, 0.0, 0.0], [8000.0, 6000.0, 250.0]),
            ([8000.0, 0.0, 0.0], [12000.0, 6000.0, 250.0]),
        ]);
        let (a, b) = match_models(&ours, &theirs, Tolerance::default());
        assert_eq!(a, vec![true]);
        assert_eq!(b, vec![true, true, true]);
    }

    #[test]
    fn site_coordinates_are_compared_in_world_space() {
        // Same box, but the scenes have different origins (the builder picks the bbox centre).
        let ours = boxes(&[(
            [2_600_000.0, 1_200_000.0, 400.0],
            [2_600_300.0, 1_200_300.0, 3400.0],
        )]);
        let theirs = boxes(&[
            (
                [2_600_000.0, 1_200_000.0, 400.0],
                [2_600_300.0, 1_200_300.0, 3400.0],
            ),
            (
                [2_610_000.0, 1_200_000.0, 400.0],
                [2_610_300.0, 1_200_300.0, 3400.0],
            ),
        ]);
        assert_ne!(ours.origin_mm, theirs.origin_mm);
        let (a, b) = match_models(&ours, &theirs, Tolerance::default());
        assert_eq!(a, vec![true]);
        assert_eq!(b, vec![true, false]);
    }

    #[test]
    fn huge_elements_go_through_the_big_list() {
        // A 1 km terrain box spans more than MAX_CELLS cells: still found.
        let ours = boxes(&[([0.0, 0.0, -2000.0], [1_000_000.0, 1_000_000.0, 0.0])]);
        let theirs = boxes(&[([0.0, 0.0, -2000.0], [1_000_000.0, 1_000_000.0, 0.0])]);
        let (a, _) = match_models(&ours, &theirs, Tolerance::default());
        assert_eq!(a, vec![true]);
    }
}
