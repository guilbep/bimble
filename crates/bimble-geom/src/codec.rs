//! Compact bytes for a `Mesh` (ADR 0036 #8): for caches and downloads, not the GPU.
//!
//! Positions are quantized to 1/16 mm steps from the mesh's lower corner and coded as the
//! difference from the previous vertex (zigzag, LEB128 varint). A mesh keeps its raw f32 bits when
//! a vertex isn't finite, it spans over 268 km (u32 steps), or, as a guard, a vertex would come
//! back more than [`TOLERANCE_MM`] away (the encoder decodes each one as the decoder will).
//! Indices are coded as the difference from the previous index. Edge endpoints that are vertices
//! become indices (decoded bit for bit like the vertex); others are quantized like positions.
//! Edge normals become indices into the mesh's table of distinct normals (a box has six; w is
//! unused, decoded 0).
//!
//! `decode` takes untrusted bytes: it never panics, allocates at most a small multiple of the
//! input length, and rejects anything left over.

use crate::Mesh;
use std::collections::HashMap;

/// The farthest a decoded vertex or edge endpoint may be from the original, mm.
pub const TOLERANCE_MM: f64 = 0.05;
const STEP: f64 = 1.0 / 16.0;
const MAGIC: &[u8; 4] = b"BMG1";

#[derive(Debug, PartialEq)]
pub enum Error {
    Truncated,
    BadMagic,
    Invalid(&'static str),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Truncated => write!(f, "mesh bytes cut short"),
            Error::BadMagic => write!(f, "not Bimble mesh bytes"),
            Error::Invalid(why) => write!(f, "invalid mesh bytes: {why}"),
        }
    }
}

impl std::error::Error for Error {}

fn put(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        out.push(v as u8 | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

fn zig(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

fn unzig(v: u64) -> i64 {
    (v >> 1) as i64 ^ -((v & 1) as i64)
}

/// Quantization of a mesh: its lower corner, or `None` for raw f32.
fn quantizer(m: &Mesh) -> Option<[f32; 3]> {
    let pts = m.positions.iter().chain(m.edges.iter().flatten());
    let mut lo = [f32::INFINITY; 3];
    let mut hi = [f32::NEG_INFINITY; 3];
    for p in pts.clone() {
        for k in 0..3 {
            if !p[k].is_finite() {
                return None;
            }
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    if lo[0] > hi[0] {
        return Some([0.0; 3]); // nothing to place
    }
    if (0..3).any(|k| (hi[k] as f64 - lo[k] as f64) / STEP >= u32::MAX as f64) {
        return None;
    }
    let ok = pts.into_iter().all(|p| {
        (0..3).all(|k| {
            (dequant(lo[k], quant(lo[k], p[k])) as f64 - p[k] as f64).abs() <= TOLERANCE_MM
        })
    });
    ok.then_some(lo)
}

fn quant(lo: f32, c: f32) -> i64 {
    ((c as f64 - lo as f64) / STEP).round() as i64
}

fn dequant(lo: f32, k: i64) -> f32 {
    (lo as f64 + k as f64 * STEP) as f32
}

pub fn encode(m: &Mesh) -> Vec<u8> {
    let q = quantizer(m);
    let mut out = Vec::with_capacity(16 + m.positions.len() * 4 + m.indices.len() * 2);
    out.extend_from_slice(MAGIC);
    out.push(q.is_some() as u8);
    for n in [
        m.positions.len(),
        m.indices.len(),
        m.edges.len(),
        m.edge_normals.len().min(m.edges.len()),
    ] {
        put(&mut out, n as u64);
    }
    let point = |out: &mut Vec<u8>, prev: &mut [i64; 3], p: &[f32; 3]| match q {
        Some(lo) => {
            for k in 0..3 {
                let v = quant(lo[k], p[k]);
                put(out, zig(v - prev[k]));
                prev[k] = v;
            }
        }
        None => p
            .iter()
            .for_each(|c| out.extend_from_slice(&c.to_le_bytes())),
    };
    if let Some(lo) = q {
        lo.iter()
            .for_each(|c| out.extend_from_slice(&c.to_le_bytes()));
    }
    let mut prev = [0i64; 3];
    for p in &m.positions {
        point(&mut out, &mut prev, p);
    }
    let mut last = 0i64;
    for &i in &m.indices {
        put(&mut out, zig(i as i64 - last));
        last = i as i64;
    }
    // Endpoint codes: even = vertex index (zigzag delta from the last one, doubled), odd = a
    // point that is no vertex, coded like positions after it.
    let mut at: HashMap<[u32; 3], u32> = HashMap::with_capacity(m.positions.len());
    for (i, p) in m.positions.iter().enumerate().rev() {
        at.insert(p.map(f32::to_bits), i as u32);
    }
    let (mut last, mut prev) = (0i64, [0i64; 3]);
    for p in m.edges.iter().flatten() {
        match at.get(&p.map(f32::to_bits)) {
            Some(&i) => {
                put(&mut out, zig(i as i64 - last) << 1);
                last = i as i64;
            }
            None => {
                out.push(1);
                point(&mut out, &mut prev, p);
            }
        }
    }
    let normals = || m.edge_normals.iter().take(m.edges.len()).flatten();
    let mut table: HashMap<[i8; 3], u64> = HashMap::new();
    let mut codes = Vec::with_capacity(m.edge_normals.len() * 2);
    for n in normals() {
        let n = [n[0], n[1], n[2]];
        let next = table.len() as u64;
        let i = *table.entry(n).or_insert(next);
        if i == next {
            codes.push(n);
        }
    }
    put(&mut out, codes.len() as u64);
    out.extend(codes.iter().flatten().map(|&c| c as u8));
    for n in normals() {
        put(&mut out, table[&[n[0], n[1], n[2]]]);
    }
    out
}

struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        if n > self.0.len() {
            return Err(Error::Truncated);
        }
        let (a, b) = self.0.split_at(n);
        self.0 = b;
        Ok(a)
    }
    fn byte(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }
    fn f32(&mut self) -> Result<f32, Error> {
        let b = self.take(4)?;
        Ok(f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn varint(&mut self) -> Result<u64, Error> {
        let mut v = 0u64;
        for shift in (0..64).step_by(7) {
            let b = self.byte()?;
            v |= ((b & 0x7f) as u64) << shift;
            if b < 0x80 {
                return Ok(v);
            }
        }
        Err(Error::Invalid("varint too long"))
    }
}

pub fn decode(bytes: &[u8]) -> Result<Mesh, Error> {
    let mut r = Reader(bytes);
    if r.take(4).map_err(|_| Error::BadMagic)? != MAGIC {
        return Err(Error::BadMagic);
    }
    let quantized = match r.byte()? {
        0 => false,
        1 => true,
        _ => return Err(Error::Invalid("unknown mode")),
    };
    let point_min = if quantized { 3 } else { 12 };
    // Counts are checked one after another against what is left; each item takes at least
    // `point_min`, 1, 2 (two endpoints) and 2 bytes (two table indices).
    let nv = r.varint()?;
    let ni = r.varint()?;
    let ne = r.varint()?;
    let nn = r.varint()?;
    let need = nv
        .saturating_mul(point_min as u64)
        .saturating_add(ni)
        .saturating_add(ne.saturating_mul(2))
        .saturating_add(nn.saturating_mul(2));
    if need > r.0.len() as u64 {
        return Err(Error::Truncated);
    }
    if nn > ne || ni % 3 != 0 {
        return Err(Error::Invalid("counts"));
    }
    let (nv, ni, ne, nn) = (nv as usize, ni as usize, ne as usize, nn as usize);
    let lo = if quantized {
        [r.f32()?, r.f32()?, r.f32()?]
    } else {
        [0.0; 3]
    };
    let point = |r: &mut Reader, prev: &mut [i64; 3]| -> Result<[f32; 3], Error> {
        let mut p = [0f32; 3];
        for k in 0..3 {
            if quantized {
                prev[k] = prev[k].wrapping_add(unzig(r.varint()?));
                p[k] = dequant(lo[k], prev[k]);
            } else {
                p[k] = r.f32()?;
            }
        }
        Ok(p)
    };
    let mut m = Mesh {
        positions: Vec::with_capacity(nv),
        indices: Vec::with_capacity(ni),
        edges: Vec::with_capacity(ne),
        edge_normals: Vec::with_capacity(nn),
    };
    let mut prev = [0i64; 3];
    for _ in 0..nv {
        m.positions.push(point(&mut r, &mut prev)?);
    }
    let mut last = 0i64;
    for _ in 0..ni {
        last = last.wrapping_add(unzig(r.varint()?));
        if !(0..nv as i64).contains(&last) {
            return Err(Error::Invalid("index out of range"));
        }
        m.indices.push(last as u32);
    }
    let (mut last, mut prev) = (0i64, [0i64; 3]);
    let mut end = |r: &mut Reader| -> Result<[f32; 3], Error> {
        let c = r.varint()?;
        if c == 1 {
            return point(r, &mut prev);
        }
        if c & 1 == 1 {
            return Err(Error::Invalid("edge code"));
        }
        last = last.wrapping_add(unzig(c >> 1));
        let i = usize::try_from(last).map_err(|_| Error::Invalid("edge vertex"))?;
        m.positions
            .get(i)
            .copied()
            .ok_or(Error::Invalid("edge vertex"))
    };
    for _ in 0..ne {
        let e = [end(&mut r)?, end(&mut r)?];
        m.edges.push(e);
    }
    let nt = r.varint()?;
    if nt > (r.0.len() / 3) as u64 {
        return Err(Error::Truncated);
    }
    let table = r.take(nt as usize * 3)?;
    let normal = |r: &mut Reader| -> Result<[i8; 4], Error> {
        let i = r.varint()?;
        if i >= nt {
            return Err(Error::Invalid("normal"));
        }
        let n = &table[i as usize * 3..];
        Ok([n[0] as i8, n[1] as i8, n[2] as i8, 0])
    };
    for _ in 0..nn {
        let n = [normal(&mut r)?, normal(&mut r)?];
        m.edge_normals.push(n);
    }
    if !r.0.is_empty() {
        return Err(Error::Invalid("trailing bytes"));
    }
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scene::Geometry;

    fn scene_meshes() -> Vec<Mesh> {
        let model = bimble_core::demo::concrete_frame(&Default::default());
        let scene = crate::Scene::from_model(&model);
        scene
            .elements
            .iter()
            .map(|e| match &e.geometry {
                Geometry::Own(m) => m.clone(),
                Geometry::Instance { shape, .. } => (**shape).clone(),
            })
            .collect()
    }

    fn check(m: &Mesh) {
        let d = decode(&encode(m)).expect("round trip");
        assert_eq!(d.indices, m.indices);
        assert_eq!(d.positions.len(), m.positions.len());
        assert_eq!(d.edges.len(), m.edges.len());
        let near = |a: &[f32; 3], b: &[f32; 3]| {
            (0..3).all(|k| (a[k] as f64 - b[k] as f64).abs() <= TOLERANCE_MM)
        };
        for (a, b) in m.positions.iter().zip(&d.positions) {
            assert!(near(a, b), "{a:?} → {b:?}");
        }
        for (e, de) in m.edges.iter().zip(&d.edges) {
            for (p, dp) in e.iter().zip(de) {
                assert!(near(p, dp));
                // An endpoint on a vertex decodes to that vertex, bit for bit.
                if let Some(i) = m.positions.iter().position(|v| v == p) {
                    assert_eq!(dp.map(f32::to_bits), d.positions[i].map(f32::to_bits));
                }
            }
        }
        let n = m.edge_normals.len().min(m.edges.len());
        for (a, b) in m.edge_normals[..n].iter().zip(&d.edge_normals) {
            assert_eq!(a.map(|v| [v[0], v[1], v[2]]), b.map(|v| [v[0], v[1], v[2]]));
        }
    }

    fn odd_meshes() -> Vec<Mesh> {
        let far = Mesh {
            // 300 km across: more 1/16 mm steps than a u32 holds, so kept raw.
            positions: vec![[0.0, 0.0, 0.0], [3.0e8, 0.0, 0.0], [0.0, 1.0, 5.0]],
            indices: vec![0, 1, 2],
            edges: vec![[[0.0, 0.0, 0.0], [0.0, 1.0, 5.0]]],
            edge_normals: vec![],
        };
        let odd = Mesh {
            // Out-of-order indices, a degenerate triangle, an edge off the vertices, -0.0.
            positions: vec![[-0.0, 3.3, -7.1], [100.03, 3.3, 1e5], [5.0, -9e4, 0.01]],
            indices: vec![2, 0, 1, 1, 1, 1, 0, 2, 1],
            edges: vec![
                [[-0.0, 3.3, -7.1], [50.0, 50.0, 50.0]],
                [[5.0, -9e4, 0.01]; 2],
            ],
            edge_normals: vec![[[0, 0, 127, 0], [-127, 0, 0, 0]]; 2],
        };
        let nan = Mesh {
            positions: vec![[f32::NAN, 0.0, 0.0]],
            ..Default::default()
        };
        vec![Mesh::default(), far, odd, nan]
    }

    #[test]
    fn round_trip_within_tolerance() {
        let meshes = scene_meshes();
        assert!(meshes.iter().any(|m| !m.edges.is_empty()));
        for m in meshes.iter().chain(&odd_meshes()[..3]) {
            check(m);
        }
        // Too wide or not finite: kept raw (mode byte 0), exact.
        let far = &odd_meshes()[1];
        assert_eq!(encode(far)[4], 0);
        assert_eq!(decode(&encode(far)).unwrap().positions, far.positions);
        assert_eq!(encode(&scene_meshes()[0])[4], 1);
        assert_eq!(encode(&odd_meshes()[3])[4], 0);
        let nan = decode(&encode(&odd_meshes()[3])).unwrap();
        assert!(nan.positions[0][0].is_nan());
    }

    #[test]
    fn corrupt_bytes_are_errors_not_panics() {
        let mut samples: Vec<Vec<u8>> = scene_meshes().iter().take(4).map(encode).collect();
        samples.extend(odd_meshes().iter().map(encode));
        for bytes in &samples {
            for n in 0..bytes.len() {
                assert!(
                    decode(&bytes[..n]).is_err(),
                    "prefix {n} of {}",
                    bytes.len()
                );
            }
            let mut extra = bytes.clone();
            extra.push(0);
            assert!(decode(&extra).is_err());
            for i in 0..bytes.len() {
                for flip in [0x01u8, 0x80, 0xff] {
                    let mut b = bytes.clone();
                    b[i] ^= flip;
                    let _ = decode(&b); // may decode to another mesh; must not panic
                }
            }
        }
        // Random bytes behind a valid header (LCG, fixed seed).
        let mut s = 0x2545_f491_4f6c_dd1du64;
        for len in 0..2000 {
            let mut b = MAGIC.to_vec();
            b.extend((0..len % 97).map(|_| {
                s = s
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                (s >> 56) as u8
            }));
            let _ = decode(&b);
        }
        // A huge count with no bytes behind it is refused before allocating.
        let mut b = MAGIC.to_vec();
        b.push(1);
        b.extend([0xff, 0xff, 0xff, 0xff, 0x0f, 0, 0, 0]);
        assert_eq!(decode(&b).err(), Some(Error::Truncated));
    }
}
