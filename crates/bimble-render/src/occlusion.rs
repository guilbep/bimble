//! Occlusion culling on the CPU (ADR 0036 #5): the largest simple elements (walls, slabs) are
//! drawn coarsely into a small depth buffer each frame, and an element whose box lies wholly
//! behind them is not drawn. No GPU read-back and no frame of delay, so it works the same on
//! WebGL2. Conservative: an occluder covers only the pixels it covers whole, at the farthest
//! depth it has in each, and a tested box is widened by a pixel, so what can be seen is never
//! left out.

use glam::{Mat4, Vec3, Vec4};

/// Width of the depth buffer (pixels); its height follows the view's aspect.
const WIDTH: usize = 160;

pub(crate) struct DepthBuf {
    w: usize,
    h: usize,
    /// Per pixel: the farthest depth (0..1, DirectX convention) behind which an occluder
    /// surely hides everything; 1 where there is none.
    depth: Vec<f32>,
    m: Mat4,
}

impl DepthBuf {
    /// Occluders (triangles in layer coordinates, grouped per element by `elements`) seen
    /// through `m` (clip space, depth 0..1).
    pub(crate) fn new(
        m: Mat4,
        aspect: f32,
        occluders: &[[Vec3; 3]],
        elements: &[std::ops::Range<u32>],
    ) -> DepthBuf {
        let w = WIDTH;
        let h = ((WIDTH as f32 / aspect.max(0.1)).round() as usize).clamp(8, 4 * WIDTH);
        let mut b = DepthBuf {
            w,
            h,
            depth: vec![1.0; w * h],
            m,
        };
        let mut scratch = Vec::new();
        for r in elements {
            b.raster(&occluders[r.start as usize..r.end as usize], &mut scratch);
        }
        b
    }

    /// Screen position (pixels) and depth of a point, None if at or behind the eye plane.
    fn project(&self, p: Vec3) -> Option<(f32, f32, f32)> {
        let c = self.m * Vec4::new(p.x, p.y, p.z, 1.0);
        if c.w <= 1e-3 {
            return None;
        }
        let (x, y, z) = (c.x / c.w, c.y / c.w, c.z / c.w);
        Some((
            (x * 0.5 + 0.5) * self.w as f32,
            (0.5 - y * 0.5) * self.h as f32,
            z,
        ))
    }

    /// One occluder element. Its triangles are sampled at pixel centres, each pixel taking the
    /// farthest depth its plane reaches over the pixel; then only pixels whose four neighbours
    /// the element also covers are kept (so the element covers them whole: a gap between two
    /// occluders narrower than a pixel stays open, and the seams between an element's own
    /// triangles don't).
    fn raster(&mut self, tris: &[[Vec3; 3]], scratch: &mut Vec<f32>) {
        let mut pts = Vec::with_capacity(tris.len());
        for t in tris {
            // An element crossing the eye plane is skipped: leaving an occluder out is safe.
            let (Some(a), Some(b), Some(c)) =
                (self.project(t[0]), self.project(t[1]), self.project(t[2]))
            else {
                return;
            };
            pts.push([a, b, c]);
        }
        let (mut x0, mut y0, mut x1, mut y1) = (
            f32::INFINITY,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::NEG_INFINITY,
        );
        for p in pts.iter().flatten() {
            (x0, y0, x1, y1) = (x0.min(p.0), y0.min(p.1), x1.max(p.0), y1.max(p.1));
        }
        let xa = x0.floor().max(0.0) as usize;
        let xb = (x1.ceil().max(0.0) as usize).min(self.w);
        let ya = y0.floor().max(0.0) as usize;
        let yb = (y1.ceil().max(0.0) as usize).min(self.h);
        if xa + 2 >= xb || ya + 2 >= yb {
            return;
        }
        let bw = xb - xa;
        let uncovered = f32::NEG_INFINITY;
        scratch.clear();
        scratch.resize(bw * (yb - ya), uncovered);
        for [a, b, c] in pts {
            let area = (b.0 - a.0) * (c.1 - a.1) - (b.1 - a.1) * (c.0 - a.0);
            if area.abs() < 1e-6 {
                continue;
            }
            // Depth is affine on screen: z = zx x + zy y + z0.
            let zx = ((b.2 - a.2) * (c.1 - a.1) - (c.2 - a.2) * (b.1 - a.1)) / area;
            let zy = ((c.2 - a.2) * (b.0 - a.0) - (b.2 - a.2) * (c.0 - a.0)) / area;
            let reach = 0.5 * (zx.abs() + zy.abs());
            let sgn = area.signum();
            let ty0 = a.1.min(b.1).min(c.1).floor().max(ya as f32) as usize;
            let ty1 = (a.1.max(b.1).max(c.1).ceil().max(0.0) as usize).min(yb);
            for y in ty0..ty1 {
                let py = y as f32 + 0.5;
                // The pixel centres inside all three edges are one span on this row.
                let (mut lo, mut hi) = (f32::NEG_INFINITY, f32::INFINITY);
                for (p, q) in [(a, b), (b, c), (c, a)] {
                    let d = (q.1 - p.1) * sgn;
                    let cst = ((q.0 - p.0) * (py - p.1) + (q.1 - p.1) * p.0) * sgn;
                    if d > 0.0 {
                        hi = hi.min(cst / d);
                    } else if d < 0.0 {
                        lo = lo.max(cst / d);
                    } else if cst < 0.0 {
                        hi = f32::NEG_INFINITY;
                    }
                }
                let sa = (lo - 0.5).ceil().max(xa as f32);
                let sb = (hi - 0.5).floor().min(xb as f32 - 1.0);
                if sa > sb {
                    continue;
                }
                for x in sa as usize..=sb as usize {
                    let px = x as f32 + 0.5;
                    let z = a.2 + zx * (px - a.0) + zy * (py - a.1) + reach;
                    let s = &mut scratch[(y - ya) * bw + x - xa];
                    *s = s.max(z);
                }
            }
        }
        for y in ya + 1..yb - 1 {
            for x in xa + 1..xb - 1 {
                let at = |x: usize, y: usize| scratch[(y - ya) * bw + x - xa];
                let z = at(x, y)
                    .max(at(x - 1, y))
                    .max(at(x + 1, y))
                    .max(at(x, y - 1))
                    .max(at(x, y + 1));
                if z > uncovered && (0.0..=1.0).contains(&z) {
                    let d = &mut self.depth[y * self.w + x];
                    *d = d.min(z);
                }
            }
        }
    }

    /// Whether anything of a box (min, max corners; layer coordinates) may be seen: its nearest
    /// depth against the buffer, over its screen rectangle widened by a pixel.
    pub(crate) fn may_see(&self, b: &[Vec3; 2]) -> bool {
        let [lo, hi] = *b;
        if !(lo.is_finite() && hi.is_finite()) {
            return true;
        }
        let (mut x0, mut y0, mut zmin) = (f32::INFINITY, f32::INFINITY, f32::INFINITY);
        let (mut x1, mut y1) = (f32::NEG_INFINITY, f32::NEG_INFINITY);
        for i in 0..8 {
            let c = Vec3::new(
                if i & 1 == 0 { lo.x } else { hi.x },
                if i & 2 == 0 { lo.y } else { hi.y },
                if i & 4 == 0 { lo.z } else { hi.z },
            );
            let Some((x, y, z)) = self.project(c) else {
                return true;
            };
            (x0, x1, y0, y1, zmin) = (x0.min(x), x1.max(x), y0.min(y), y1.max(y), zmin.min(z));
        }
        if zmin <= 0.0 {
            return true;
        }
        let clamp = |v: f32, n: usize| (v.max(0.0) as usize).min(n);
        let (xa, xb) = (clamp(x0 - 1.0, self.w), clamp(x1 + 2.0, self.w));
        let (ya, yb) = (clamp(y0 - 1.0, self.h), clamp(y1 + 2.0, self.h));
        if xa >= xb || ya >= yb {
            // Off the buffer: the frustum test decides.
            return true;
        }
        (ya..yb).any(|y| {
            self.depth[y * self.w + xa..y * self.w + xb]
                .iter()
                .any(|&d| d >= zmin)
        })
    }
}

/// Occluders worth drawing: large elements made of few triangles (walls, slabs, columns),
/// largest first, up to a triangle budget.
pub(crate) const BUDGET: usize = 60_000;
/// An occluder has at most this many triangles (a box has 12, a wall with windows a few
/// hundred).
pub(crate) const MAX_TRIANGLES: usize = 512;

#[cfg(test)]
mod tests {
    use super::*;

    /// A wall in front hides a box behind it, not one beside it or in front of it.
    #[test]
    fn a_wall_hides_what_is_behind_it() {
        let m = crate::directx::perspective(1.0, 1.0, 1.0, 100.0)
            * crate::look_at_mat4(Vec3::new(0.0, 0.0, 10.0), Vec3::ZERO, Vec3::Y);
        // A 6 × 6 wall at z = 0, facing the eye.
        let (a, b, c, d) = (
            Vec3::new(-3.0, -3.0, 0.0),
            Vec3::new(3.0, -3.0, 0.0),
            Vec3::new(3.0, 3.0, 0.0),
            Vec3::new(-3.0, 3.0, 0.0),
        );
        let buf = DepthBuf::new(
            m,
            1.0,
            &[[a, b, c], [a, c, d]],
            std::slice::from_ref(&(0..2)),
        );
        let bx = |x: f32, y: f32, z: f32, r: f32| [Vec3::new(x, y, z) - r, Vec3::new(x, y, z) + r];
        assert!(!buf.may_see(&bx(0.0, 0.0, -5.0, 1.0)), "behind");
        assert!(buf.may_see(&bx(0.0, 0.0, 5.0, 1.0)), "in front");
        assert!(buf.may_see(&bx(6.0, 0.0, -5.0, 1.0)), "beside");
        assert!(
            buf.may_see(&bx(2.5, 0.0, -2.0, 1.0)),
            "peeking past its edge"
        );
        assert!(
            buf.may_see(&bx(0.0, 0.0, 0.0, 0.5)),
            "through it (the wall itself)"
        );
    }
}
