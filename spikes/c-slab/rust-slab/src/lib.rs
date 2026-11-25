//! Rust port of spikes/c-slab/slab.c (one IfcSlab mesh), same algorithm and output bits.
//! corners_only, earcut (convex quads only), extrude_tris, mesh_from_tris (local) or
//! Scene::from_model + MeshBuilder::build (scene), and crease_edges_with_normals.
//! No allocation: fixed-capacity arrays, capped at SLAB_MAX_PTS outline points.

pub const SLAB_MAX_PTS: usize = 64;
pub const SLAB_MAX_TRIS: usize = 2 * (SLAB_MAX_PTS - 2) + 2 * SLAB_MAX_PTS;
pub const SLAB_MAX_VERTS: usize = 2 * SLAB_MAX_PTS;
pub const SLAB_MAX_EDGES: usize = 3 * SLAB_MAX_TRIS;

#[repr(C)]
pub struct SlabMesh {
    pub origin: [f64; 3],
    pub nv: usize,
    pub nt: usize,
    pub ne: usize,
    pub pos: [[f32; 3]; SLAB_MAX_VERTS],
    pub idx: [u32; SLAB_MAX_TRIS * 3],
    pub edges: [[f32; 6]; SLAB_MAX_EDGES],
    pub normals: [[i8; 8]; SLAB_MAX_EDGES],
}

impl SlabMesh {
    pub const ZERO: SlabMesh = SlabMesh {
        origin: [0.0; 3],
        nv: 0,
        nt: 0,
        ne: 0,
        pos: [[0.0; 3]; SLAB_MAX_VERTS],
        idx: [0; SLAB_MAX_TRIS * 3],
        edges: [[0.0; 6]; SLAB_MAX_EDGES],
        normals: [[0; 8]; SLAB_MAX_EDGES],
    };
}

type Tri = [[f64; 3]; 3];

/// Drop repeated and collinear points (lib.rs corners_only). Returns the new count.
fn corners_only(v: &mut [[f64; 2]], mut n: usize) -> usize {
    loop {
        if n < 3 {
            return n;
        }
        let mut drop = None;
        for i in 0..n {
            let (a, b, c) = (v[(i + n - 1) % n], v[i], v[(i + 1) % n]);
            let (ux, uy, wx, wy) = (c[0] - a[0], c[1] - a[1], b[0] - a[0], b[1] - a[1]);
            let len = (ux * ux + uy * uy).sqrt();
            let same = (b[0] - a[0]).abs() < 1e-9 && (b[1] - a[1]).abs() < 1e-9;
            let l2 = len * len;
            let along = (wx * ux + wy * uy) / if l2 > 1e-30 { l2 } else { 1e-30 };
            if same
                || (len > 0.0
                    && (ux * wy - uy * wx).abs() / len < 1e-6
                    && (0.0..=1.0).contains(&along))
            {
                drop = Some(i);
                break;
            }
        }
        let Some(d) = drop else { return n };
        v.copy_within(d + 1..n, d);
        n -= 1;
    }
}

/// Cap triangulation. NOT a general earcut: a CCW convex quad gets earcutr's [2,3,0, 0,1,2];
/// other polygons get a fan from vertex 0 (convex outlines only).
fn triangulate(n: usize, t: &mut [usize]) -> usize {
    if n == 4 {
        t[..6].copy_from_slice(&[2, 3, 0, 0, 1, 2]);
        return 2;
    }
    let mut k = 0;
    for i in 1..n - 1 {
        t[k..k + 3].copy_from_slice(&[0, i, i + 1]);
        k += 3;
    }
    n - 2
}

fn p3(q: [f64; 2], z: f64) -> [f64; 3] {
    [q[0], q[1], z]
}

/// extrude_tris: bottom cap reversed, top cap, then two triangles per side.
fn extrude(pts: &[[f64; 2]], n: usize, depth: f64, tris: &mut [Tri]) -> usize {
    let mut t = [0usize; 3 * SLAB_MAX_PTS];
    let nt = triangulate(n, &mut t);
    let mut m = 0;
    for pass in 0..2 {
        let z = if pass == 1 { depth } else { 0.0 };
        for i in 0..nt {
            let (a, mut b, mut c) = (t[3 * i], t[3 * i + 1], t[3 * i + 2]);
            if pass == 0 {
                std::mem::swap(&mut b, &mut c);
            }
            tris[m] = [p3(pts[a], z), p3(pts[b], z), p3(pts[c], z)];
            m += 1;
        }
    }
    for i in 0..n {
        let j = (i + 1) % n;
        tris[m] = [p3(pts[i], 0.0), p3(pts[j], 0.0), p3(pts[j], depth)];
        tris[m + 1] = [p3(pts[i], 0.0), p3(pts[j], depth), p3(pts[i], depth)];
        m += 2;
    }
    m
}

// ---- crease_edges_with_normals (scene.rs) ----
#[derive(Clone, Copy)]
struct EdgeUse {
    k: [i64; 6],
    count: u32,
    crease: bool,
    normal: [f32; 3],
    other: [f32; 3],
    seg: [f32; 6],
}

fn q8(c: f32) -> i8 {
    let x = c.clamp(-1.0, 1.0);
    (x * 127.0).round() as i8
}

fn pack(n: [f32; 3], sign: f32, o: &mut [i8]) {
    o[0] = q8(sign * n[0]);
    o[1] = q8(sign * n[1]);
    o[2] = q8(sign * n[2]);
    o[3] = 0;
}

fn segless(a: &[f32; 6], b: &[f32; 6]) -> bool {
    for i in 0..6 {
        if a[i] != b[i] {
            return a[i] < b[i];
        }
    }
    false
}

fn crease_edges(m: &mut SlabMesh, angle_deg: f32, want_normals: bool) {
    let cos_limit = (angle_deg * (std::f32::consts::PI / 180.0)).cos();
    let empty = EdgeUse {
        k: [0; 6],
        count: 0,
        crease: false,
        normal: [0.0; 3],
        other: [0.0; 3],
        seg: [0.0; 6],
    };
    let mut uses = [empty; SLAB_MAX_EDGES];
    let mut nu = 0;
    for t in 0..m.nt {
        let tr = [
            m.idx[3 * t] as usize,
            m.idx[3 * t + 1] as usize,
            m.idx[3 * t + 2] as usize,
        ];
        let (a0, b0, c0) = (m.pos[tr[0]], m.pos[tr[1]], m.pos[tr[2]]);
        let u = [b0[0] - a0[0], b0[1] - a0[1], b0[2] - a0[2]];
        let v = [c0[0] - a0[0], c0[1] - a0[1], c0[2] - a0[2]];
        let cr = [
            u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0],
        ];
        let l = (cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]).sqrt();
        let n = if l > 1e-12 {
            [cr[0] / l, cr[1] / l, cr[2] / l]
        } else {
            [0.0, 0.0, 1.0]
        };
        for k in 0..3 {
            let (a, b) = (m.pos[tr[k]], m.pos[tr[(k + 1) % 3]]);
            let ka = a.map(|c| (c * 100.0).round() as i64);
            let kb = b.map(|c| (c * 100.0).round() as i64);
            if ka == kb {
                continue;
            }
            let (lo, hi) = if ka < kb { (ka, kb) } else { (kb, ka) };
            let key = [lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]];
            let i = match uses[..nu].iter().position(|e| e.k == key) {
                Some(i) => i,
                None => {
                    let s = [a[0], a[1], a[2], b[0], b[1], b[2]];
                    uses[nu] = EdgeUse {
                        k: key,
                        count: 0,
                        crease: false,
                        normal: n,
                        other: n,
                        seg: s,
                    };
                    nu += 1;
                    nu - 1
                }
            };
            let e = &mut uses[i];
            if e.count > 0 {
                let c = e.normal[0] * n[0] + e.normal[1] * n[1] + e.normal[2] * n[2];
                if c < cos_limit {
                    e.crease = true;
                }
                e.other = n;
            }
            e.count += 1;
        }
    }
    m.ne = 0;
    for e in &uses[..nu] {
        if !(e.count == 1 || e.crease) {
            continue;
        }
        // insertion sort by the 6 flattened coordinates (Rust sorts after a HashMap)
        let mut j = m.ne;
        while j > 0 && segless(&e.seg, &m.edges[j - 1]) {
            m.edges[j] = m.edges[j - 1];
            m.normals[j] = m.normals[j - 1];
            j -= 1;
        }
        m.edges[j] = e.seg;
        let o = &mut m.normals[j];
        *o = [0; 8];
        if want_normals {
            if e.count == 1 {
                pack(e.normal, 1.0, &mut o[..4]);
                pack(e.normal, -1.0, &mut o[4..]);
            } else if e.count == 2 {
                pack(e.normal, 1.0, &mut o[..4]);
                pack(e.other, 1.0, &mut o[4..]);
            }
        }
        m.ne += 1;
    }
}

/// mesh_from_tris: weld by f64 bits after +0.0, then positions to f32.
fn weld_local(tris: &[Tri], m: &mut SlabMesh) {
    let mut wpos = [[0.0f64; 3]; SLAB_MAX_VERTS];
    let mut nv = 0;
    for (t, tri) in tris.iter().enumerate() {
        for (k, q) in tri.iter().enumerate() {
            let p = q.map(|c| c + 0.0);
            let bits = p.map(f64::to_bits);
            let i = wpos[..nv]
                .iter()
                .position(|w| w.map(f64::to_bits) == bits)
                .unwrap_or_else(|| {
                    wpos[nv] = p;
                    nv += 1;
                    nv - 1
                });
            m.idx[3 * t + k] = i as u32;
        }
    }
    for i in 0..nv {
        m.pos[i] = wpos[i].map(|c| c as f32);
    }
    m.nv = nv;
    m.nt = tris.len();
}

/// Mesh an IfcSlab: CCW outline (mm), thickness along +Z, placement origin with default axes,
/// storey elevation 0. scene = false: element coordinates; true: relative to the scene origin.
#[allow(clippy::result_unit_err, clippy::neg_cmp_op_on_partial_ord)]
pub fn slab_mesh(
    outline: &[[f64; 2]],
    thickness: f64,
    at: &[f64; 3],
    out: &mut SlabMesh,
    scene: bool,
) -> Result<(), ()> {
    let n = outline.len();
    if !(3..=SLAB_MAX_PTS).contains(&n) || !(thickness > 0.0) || !thickness.is_finite() {
        return Err(());
    }
    let mut pts = [[0.0f64; 2]; SLAB_MAX_PTS];
    pts[..n].copy_from_slice(outline);
    let n = corners_only(&mut pts, n);
    if n < 3 {
        return Err(());
    }
    let mut tris = [[[0.0f64; 3]; 3]; SLAB_MAX_TRIS];
    let ntri = extrude(&pts, n, thickness, &mut tris);
    *out = SlabMesh::ZERO;
    weld_local(&tris[..ntri], out);
    if scene {
        // placement_matrix with default axes, storey elevation 0; y = z x x
        let (z, x) = ([0.0, 0.0, 1.0f64], [1.0, 0.0, 0.0f64]);
        let y = [
            z[1] * x[2] - z[2] * x[1],
            z[2] * x[0] - z[0] * x[2],
            z[0] * x[1] - z[1] * x[0],
        ];
        let mut mat = [x, y, z, *at];
        mat[3][2] = at[2] + 0.0; // + storey elevation (0)
        let mut world = [[[0.0f64; 3]; 3]; SLAB_MAX_TRIS];
        let mut nt = 0;
        for t in 0..out.nt {
            let mut finite = true;
            for k in 0..3 {
                let lp = out.pos[out.idx[3 * t + k] as usize];
                let p = lp.map(f64::from);
                for r in 0..3 {
                    world[nt][k][r] =
                        mat[0][r] * p[0] + mat[1][r] * p[1] + mat[2][r] * p[2] + mat[3][r];
                    finite &= world[nt][k][r].is_finite();
                }
            }
            if finite {
                nt += 1;
            }
        }
        let (mut lo, mut hi) = ([f64::INFINITY; 3], [f64::NEG_INFINITY; 3]);
        for tri in &world[..nt] {
            for p in tri {
                for r in 0..3 {
                    lo[r] = lo[r].min(p[r]);
                    hi[r] = hi[r].max(p[r]);
                }
            }
        }
        let org: [f64; 3] = std::array::from_fn(|r| {
            if lo[0].is_finite() {
                ((lo[r] + hi[r]) / 2.0).round()
            } else {
                0.0
            }
        });
        // MeshBuilder::build: skip degenerate triangles, weld by f32 bits
        let mut spos = [[0.0f32; 3]; SLAB_MAX_VERTS];
        let mut nv = 0;
        out.nt = 0;
        for w in &world[..nt] {
            let u: [f64; 3] = std::array::from_fn(|r| w[1][r] - w[0][r]);
            let v: [f64; 3] = std::array::from_fn(|r| w[2][r] - w[0][r]);
            let c = [
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            ];
            let l = (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]).sqrt();
            if l.is_nan() || l <= 0.0 {
                continue;
            }
            for k in 0..3 {
                let p: [f32; 3] = std::array::from_fn(|r| (w[k][r] - org[r]) as f32);
                let bits = p.map(f32::to_bits);
                let i = spos[..nv]
                    .iter()
                    .position(|s| s.map(f32::to_bits) == bits)
                    .unwrap_or_else(|| {
                        spos[nv] = p;
                        nv += 1;
                        nv - 1
                    });
                out.idx[3 * out.nt + k] = i as u32;
            }
            out.nt += 1;
        }
        out.pos[..nv].copy_from_slice(&spos[..nv]);
        out.nv = nv;
        out.origin = org;
    }
    crease_edges(out, 20.0, scene);
    Ok(())
}

// ---- exported wasm ABI (web/glue.js): slab_mesh(x0, y0, x1, y1, t) -> 1|0, then pointers and counts ----
#[cfg(target_arch = "wasm32")]
mod wasm {
    use super::*;

    static mut MESH: SlabMesh = SlabMesh::ZERO;

    const SPIKE_K: f64 = 1.0; // bench/rust_pixels_loop.mjs edits this: thickness multiplier

    #[allow(clippy::deref_addrof)] // `&mut MESH` would be a static_mut_refs error
    #[unsafe(no_mangle)]
    pub extern "C" fn slab_mesh(x0: f64, y0: f64, x1: f64, y1: f64, t: f64) -> i32 {
        let outline = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]];
        // SAFETY: wasm32 is single-threaded; the page reads MESH only between calls.
        let m = unsafe { &mut *(&raw mut MESH) };
        super::slab_mesh(&outline, t * SPIKE_K, &[1000.0, 2000.0, 3000.0], m, true).is_ok() as i32
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn pos_ptr() -> u32 {
        unsafe { (&raw const MESH.pos) as u32 }
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn pos_len() -> i32 {
        unsafe { MESH.nv as i32 }
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn idx_ptr() -> u32 {
        unsafe { (&raw const MESH.idx) as u32 }
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn idx_len() -> i32 {
        unsafe { 3 * MESH.nt as i32 }
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn edge_ptr() -> u32 {
        unsafe { (&raw const MESH.edges) as u32 }
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn edge_len() -> i32 {
        unsafe { MESH.ne as i32 }
    }
}
