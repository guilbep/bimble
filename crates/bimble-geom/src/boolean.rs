//! Boolean operations on closed triangle meshes, through the manifold3d kernel (ADR 0010).
//!
//! Input and output are triangle soups in f64 (mm, world). Shared vertices are welded exactly
//! (same bits), and everything is shifted to a local origin first so site coordinates keep full
//! precision. Inputs that aren't closed 2-manifolds are rejected with an error: callers then draw
//! the element uncut and report it, rather than guessing.

#[cfg(not(target_arch = "wasm32"))]
use manifold_csg::Manifold;

pub type Tri = [[f64; 3]; 3];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    Difference,
    Union,
    Intersection,
}

#[derive(Debug, Clone, PartialEq)]
pub struct BooleanError(pub String);

impl std::fmt::Display for BooleanError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for BooleanError {}

fn bounds(sets: &[&[Tri]]) -> Option<([f64; 3], [f64; 3])> {
    let mut it = sets.iter().flat_map(|s| s.iter()).flat_map(|t| t.iter());
    let first = *it.next()?;
    Some(it.fold((first, first), |(mut lo, mut hi), p| {
        for k in 0..3 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
        (lo, hi)
    }))
}

/// Shared vertices welded exactly (same bits after shifting to `origin`): positions and indices.
fn weld(tris: &[Tri], origin: [f64; 3]) -> (Vec<f64>, Vec<u64>) {
    let mut index: std::collections::HashMap<[u64; 3], u64> = std::collections::HashMap::new();
    let mut verts: Vec<f64> = Vec::new();
    let mut idx: Vec<u64> = Vec::with_capacity(tris.len() * 3);
    for t in tris {
        for p in t {
            // `+ 0.0` turns -0.0 into 0.0 so equal coordinates weld.
            let q = [
                p[0] - origin[0] + 0.0,
                p[1] - origin[1] + 0.0,
                p[2] - origin[2] + 0.0,
            ];
            let key = q.map(f64::to_bits);
            let next = (verts.len() / 3) as u64;
            let i = *index.entry(key).or_insert_with(|| {
                verts.extend_from_slice(&q);
                next
            });
            idx.push(i);
        }
    }
    (verts, idx)
}

#[cfg(not(target_arch = "wasm32"))]
fn to_manifold(tris: &[Tri], origin: [f64; 3]) -> Result<Manifold, BooleanError> {
    let (verts, idx) = weld(tris, origin);
    let m = Manifold::from_mesh_f64(&verts, 3, &idx)
        .map_err(|e| BooleanError(format!("not a closed solid: {e}")))?;
    m.status()
        .map_err(|e| BooleanError(format!("not a closed solid: {e}")))?;
    Ok(m)
}

#[cfg(not(target_arch = "wasm32"))]
fn from_manifold(m: &Manifold, origin: [f64; 3]) -> Vec<Tri> {
    let (props, n, idx) = m.to_mesh_f64();
    let v = |i: u64| {
        let i = i as usize * n;
        [
            props[i] + origin[0],
            props[i + 1] + origin[1],
            props[i + 2] + origin[2],
        ]
    };
    idx.as_chunks::<3>()
        .0
        .iter()
        .map(|t| [v(t[0]), v(t[1]), v(t[2])])
        .collect()
}

/// `a op (union of b)`. Each operand must be one or more closed, non-overlapping solids.
pub fn apply(op: Op, a: &[Tri], b: &[&[Tri]]) -> Result<Vec<Tri>, BooleanError> {
    let b: Vec<&[Tri]> = b.iter().copied().filter(|s| !s.is_empty()).collect();
    if b.is_empty() {
        return Ok(match op {
            Op::Intersection => Vec::new(),
            _ => a.to_vec(),
        });
    }
    let mut all = vec![a];
    all.extend(b.iter().copied());
    let Some((lo, hi)) = bounds(&all) else {
        return Ok(Vec::new());
    };
    // Coordinates relative to the middle keep full precision with site coordinates.
    let origin = [0, 1, 2].map(|k| ((lo[k] + hi[k]) / 2.0).round());
    match kernel(op, a, &b, origin) {
        // Solids exported with corners a hair apart (seen in Revit Breps: 0.03–0.07 mm) are
        // not closed for the kernel: once more with points that close welded together, as
        // IfcOpenShell sews them. Valid solids never take this path.
        Err(e) if e.0.starts_with("not a closed solid") => {
            let a = snap(a, SEW_MM);
            let b: Vec<Vec<Tri>> = b.iter().map(|s| snap(s, SEW_MM)).collect();
            let refs: Vec<&[Tri]> = b.iter().map(Vec::as_slice).collect();
            kernel(op, &a, &refs, origin)
        }
        r => r,
    }
}

/// How far apart two corners may be and still be one (mm) when a solid isn't closed exactly.
pub const SEW_MM: f64 = 0.1;

/// Triangles with every point within `tol` (each coordinate) of an earlier one moved onto it;
/// triangles that collapse are dropped. In order: the same input gives the same output.
pub fn snap(tris: &[Tri], tol: f64) -> Vec<Tri> {
    let cell = |c: f64| (c / tol).floor() as i64;
    let mut grid: std::collections::HashMap<[i64; 3], Vec<[f64; 3]>> = Default::default();
    let mut at = |p: [f64; 3]| -> [f64; 3] {
        let k = p.map(cell);
        for dx in -1..=1 {
            for dy in -1..=1 {
                for dz in -1..=1 {
                    if let Some(v) = grid.get(&[k[0] + dx, k[1] + dy, k[2] + dz])
                        && let Some(q) = v
                            .iter()
                            .find(|q| (0..3).all(|i| (q[i] - p[i]).abs() <= tol))
                    {
                        return *q;
                    }
                }
            }
        }
        grid.entry(k).or_default().push(p);
        p
    };
    tris.iter()
        .map(|t| t.map(&mut at))
        .filter(|t| t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
        .collect()
}

#[cfg(not(target_arch = "wasm32"))]
fn kernel(op: Op, a: &[Tri], b: &[&[Tri]], origin: [f64; 3]) -> Result<Vec<Tri>, BooleanError> {
    let ma = to_manifold(a, origin)?;
    let cutters: Vec<Manifold> = b
        .iter()
        .map(|s| to_manifold(s, origin))
        .collect::<Result<_, _>>()?;
    let mut cut = cutters[0].clone();
    for c in &cutters[1..] {
        cut = cut.union(c);
    }
    let r = match op {
        Op::Difference => ma.difference(&cut),
        Op::Union => ma.union(&cut),
        Op::Intersection => ma.intersection(&cut),
    };
    r.status()
        .map_err(|e| BooleanError(format!("boolean failed: {e}")))?;
    Ok(from_manifold(&r, origin))
}

/// In a browser, the same kernel through its official WebAssembly build (npm `manifold-3d`),
/// which the page loads and exposes as `bimbleBoolean` (web/manifold-bridge.js, ADR 0020).
/// It takes f32 positions: relative to `origin`, that is 1 µm or better up to ±8 m.
#[cfg(target_arch = "wasm32")]
mod js {
    use wasm_bindgen::prelude::*;

    #[wasm_bindgen]
    extern "C" {
        #[wasm_bindgen(js_name = bimbleBoolean, catch)]
        pub fn boolean(
            op: u32,
            a_pos: &[f32],
            a_tri: &[u32],
            cutters: &js_sys::Array,
        ) -> Result<js_sys::Array, JsValue>;
    }
}

#[cfg(target_arch = "wasm32")]
fn kernel(op: Op, a: &[Tri], b: &[&[Tri]], origin: [f64; 3]) -> Result<Vec<Tri>, BooleanError> {
    let flat = |tris: &[Tri]| -> Result<(Vec<f32>, Vec<u32>), BooleanError> {
        let (v, i) = weld(tris, origin);
        let i = i
            .into_iter()
            .map(u32::try_from)
            .collect::<Result<Vec<u32>, _>>()
            .map_err(|_| BooleanError("mesh too large".into()))?;
        Ok((v.into_iter().map(|x| x as f32).collect(), i))
    };
    let (ap, at) = flat(a)?;
    let cutters = js_sys::Array::new();
    for s in b {
        let (p, t) = flat(s)?;
        cutters.push(&js_sys::Array::of2(
            &js_sys::Float32Array::from(p.as_slice()),
            &js_sys::Uint32Array::from(t.as_slice()),
        ));
    }
    let code = match op {
        Op::Difference => 0,
        Op::Union => 1,
        Op::Intersection => 2,
    };
    let r = js::boolean(code, &ap, &at, &cutters).map_err(|e| {
        BooleanError(
            e.as_string()
                .or_else(|| js_sys::Error::from(e).message().as_string())
                .unwrap_or_else(|| "boolean failed".into()),
        )
    })?;
    let pos = js_sys::Float32Array::from(r.get(0)).to_vec();
    let tri = js_sys::Uint32Array::from(r.get(1)).to_vec();
    let v = |i: u32| -> Option<[f64; 3]> {
        let i = i as usize * 3;
        Some([
            *pos.get(i)? as f64 + origin[0],
            *pos.get(i + 1)? as f64 + origin[1],
            *pos.get(i + 2)? as f64 + origin[2],
        ])
    };
    tri.as_chunks::<3>()
        .0
        .iter()
        .map(|t| Some([v(t[0])?, v(t[1])?, v(t[2])?]))
        .collect::<Option<Vec<Tri>>>()
        .ok_or_else(|| BooleanError("bad mesh from the kernel".into()))
}

/// Closed box from `lo` to `hi`, outward-facing (for tests and half-space solids).
pub fn cuboid(lo: [f64; 3], hi: [f64; 3]) -> Vec<Tri> {
    let p = |x: usize, y: usize, z: usize| {
        [
            if x == 0 { lo[0] } else { hi[0] },
            if y == 0 { lo[1] } else { hi[1] },
            if z == 0 { lo[2] } else { hi[2] },
        ]
    };
    // Each face as two triangles, counter-clockwise seen from outside.
    let quads = [
        [p(0, 0, 0), p(0, 1, 0), p(1, 1, 0), p(1, 0, 0)], // -z
        [p(0, 0, 1), p(1, 0, 1), p(1, 1, 1), p(0, 1, 1)], // +z
        [p(0, 0, 0), p(1, 0, 0), p(1, 0, 1), p(0, 0, 1)], // -y
        [p(0, 1, 0), p(0, 1, 1), p(1, 1, 1), p(1, 1, 0)], // +y
        [p(0, 0, 0), p(0, 0, 1), p(0, 1, 1), p(0, 1, 0)], // -x
        [p(1, 0, 0), p(1, 1, 0), p(1, 1, 1), p(1, 0, 1)], // +x
    ];
    quads
        .iter()
        .flat_map(|q| [[q[0], q[1], q[2]], [q[0], q[2], q[3]]])
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn volume(tris: &[Tri]) -> f64 {
        let o = tris[0][0];
        tris.iter()
            .map(|t| {
                let [a, b, c] = t.map(|p| [p[0] - o[0], p[1] - o[1], p[2] - o[2]]);
                a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0])
            })
            .sum::<f64>()
            / 6.0
    }

    #[test]
    fn wall_minus_openings_is_exact_at_site_coordinates() {
        let (x, y) = (2_500_000_000.0, 1_100_000_000.0);
        let wall = cuboid([x, y, 0.0], [x + 6000.0, y + 200.0, 2800.0]);
        let door = cuboid([x + 1000.0, y - 10.0, 0.0], [x + 2200.0, y + 210.0, 2100.0]);
        let window = cuboid(
            [x + 3000.0, y - 10.0, 900.0],
            [x + 4200.0, y + 210.0, 2100.0],
        );
        let r = apply(Op::Difference, &wall, &[&door, &window]).unwrap();
        let want = 6000.0 * 200.0 * 2800.0 - 1200.0 * 200.0 * 2100.0 - 1200.0 * 200.0 * 1200.0;
        assert!(
            (volume(&r) - want).abs() / want < 1e-9,
            "{} vs {want}",
            volume(&r)
        );
    }

    /// A wall whose top corner is written twice, 0.05 mm apart (as in a Revit Brep): not
    /// closed for the kernel; sewn within 0.1 mm, its opening is cut. A real gap still fails.
    #[test]
    fn corners_a_hair_apart_are_sewn() {
        let (x, y) = (2_500_000_000.0, 1_100_000_000.0);
        let top = [x + 6000.0, y + 200.0, 2800.0];
        let mut wall = cuboid([x, y, 0.0], top);
        let k = wall.iter().position(|t| t.contains(&top)).unwrap();
        for p in &mut wall[k] {
            if *p == top {
                p[0] += 0.05;
            }
        }
        let door = cuboid([x + 1000.0, y - 10.0, 0.0], [x + 2200.0, y + 210.0, 2100.0]);
        let r = apply(Op::Difference, &wall, &[&door]).unwrap();
        let want = 6000.0 * 200.0 * 2800.0 - 1200.0 * 200.0 * 2100.0;
        // The corner is kept where it was first written: off by 0.05 mm, a few mm³.
        assert!((volume(&r) - want).abs() / want < 1e-5, "{}", volume(&r));
        for p in &mut wall[k] {
            if p[0] == top[0] + 0.05 {
                p[0] += 5.0;
            }
        }
        assert!(apply(Op::Difference, &wall, &[&door]).is_err());
    }

    #[test]
    fn open_mesh_is_rejected() {
        let mut open = cuboid([0.0; 3], [1.0; 3]);
        open.truncate(10); // drop one face
        let c = cuboid([0.5; 3], [2.0; 3]);
        assert!(apply(Op::Difference, &open, &[&c]).is_err());
    }

    #[test]
    fn nothing_to_cut_returns_the_input() {
        let a = cuboid([0.0; 3], [1.0; 3]);
        assert_eq!(apply(Op::Difference, &a, &[]).unwrap(), a);
    }
}
