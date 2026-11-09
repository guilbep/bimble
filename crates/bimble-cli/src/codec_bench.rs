//! `bimble codec-bench`: what the geometry codec (ADR 0036 #8) does on real models.
//! Each own mesh and each shared shape once, single-threaded. Raw is the bytes in memory:
//! positions 12 B, indices 4 B, edges 24 B and their normals 8 B. Models are named by size only
//! (client files must not appear in printed results).

use bimble_geom::Mesh;
use bimble_geom::codec;
use bimble_geom::scene::Geometry;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::time::Instant;

fn raw(m: &Mesh) -> Vec<u8> {
    let mut b = Vec::with_capacity(m.positions.len() * 12 + m.indices.len() * 4);
    m.positions
        .iter()
        .flatten()
        .for_each(|c| b.extend(c.to_le_bytes()));
    m.indices.iter().for_each(|i| b.extend(i.to_le_bytes()));
    m.edges
        .iter()
        .flatten()
        .flatten()
        .for_each(|c| b.extend(c.to_le_bytes()));
    let normals = m
        .edge_normals
        .iter()
        .take(m.edges.len())
        .flatten()
        .flatten();
    b.extend(normals.map(|&c| c as u8));
    b
}

fn gzip(b: &[u8]) -> Vec<u8> {
    let mut z = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
    z.write_all(b).expect("in memory");
    z.finish().expect("in memory")
}

/// Seconds to gunzip `z`.
fn gunzip_s(z: &[u8]) -> f64 {
    let t = Instant::now();
    let mut out = Vec::new();
    flate2::read::GzDecoder::new(z)
        .read_to_end(&mut out)
        .expect("in memory");
    std::hint::black_box(out);
    t.elapsed().as_secs_f64()
}

pub fn run(files: Vec<PathBuf>) -> Result<(), String> {
    println!(
        "model | meshes | raw MB | encoded MB | ratio | encode MB/s | decode MB/s | gzip: raw / encoded MB | gunzip+decode MB/s"
    );
    for f in files {
        let size = std::fs::metadata(&f).map_err(|e| e.to_string())?.len();
        let scene = crate::load_scene(&f)?;
        let mut seen = std::collections::HashSet::new();
        let meshes: Vec<&Mesh> = scene
            .elements
            .iter()
            .filter_map(|e| match &e.geometry {
                Geometry::Own(m) => Some(m),
                Geometry::Instance { shape, .. } => seen
                    .insert(std::sync::Arc::as_ptr(shape))
                    .then_some(&**shape),
            })
            .collect();
        let raw_all: Vec<u8> = meshes.iter().flat_map(|m| raw(m)).collect();
        let t = Instant::now();
        let enc: Vec<Vec<u8>> = meshes.iter().map(|m| codec::encode(m)).collect();
        let t_enc = t.elapsed().as_secs_f64();
        let t = Instant::now();
        for b in &enc {
            std::hint::black_box(codec::decode(b).map_err(|e| e.to_string())?);
        }
        let t_dec = t.elapsed().as_secs_f64();
        let enc_all: Vec<u8> = enc.concat();
        let mb = |n: usize| n as f64 / 1e6;
        let r = raw_all.len();
        let gz = gzip(&enc_all);
        println!(
            "{} MB | {} | {:.1} | {:.1} | {:.2} | {:.0} | {:.0} | {:.1} / {:.1} | {:.0}",
            size / 1_000_000,
            meshes.len(),
            mb(r),
            mb(enc_all.len()),
            r as f64 / enc_all.len() as f64,
            mb(r) / t_enc,
            mb(r) / t_dec,
            mb(gzip(&raw_all).len()),
            mb(gz.len()),
            mb(r) / (gunzip_s(&gz) + t_dec),
        );
    }
    Ok(())
}
