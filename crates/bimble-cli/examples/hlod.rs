//! Measures the proxies of spatial cells (ADR 0036 #9) on model files:
//! `cargo run --release -p bimble-cli --example hlod -- <file.ifc|.bimble>...`
//! Optional: `--leaf N --resolution N --ratio R` to try other cuts.

use bimble_geom::hlod::{self, Options};
use std::time::Instant;

fn main() {
    let mut opt = Options::default();
    let mut files = vec![];
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut num = || {
            args.next()
                .and_then(|v| v.parse::<f64>().ok())
                .expect("a number")
        };
        match a.as_str() {
            "--leaf" => opt.leaf_triangles = num() as usize,
            "--resolution" => opt.resolution = num() as u32,
            "--ratio" => opt.max_ratio = num(),
            _ => files.push(a),
        }
    }
    for f in files {
        let size = std::fs::metadata(&f).map(|m| m.len()).unwrap_or(0);
        let scene = match bimble_viewer::load_scene(std::path::Path::new(&f)) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("{f}: {e}");
                continue;
            }
        };
        let t = Instant::now();
        let h = hlod::build(&scene, &opt);
        let ms = t.elapsed().as_secs_f64() * 1e3;
        let drawn: usize = scene.elements.iter().map(|e| e.triangle_count()).sum();
        println!(
            "{} MB: {} elements, {:.2} M triangles drawn, {:.2} M stored; build {:.0} ms",
            size / 1_000_000,
            scene.elements.len(),
            drawn as f64 / 1e6,
            scene.stored_triangles() as f64 / 1e6,
            ms
        );
        println!("depth  cells  proxies  stands for  proxy tris     edges  error mm  proxy MB");
        let deepest = h.cells.iter().map(|c| c.depth).max().unwrap_or(0);
        let (mut all_tris, mut all_bytes) = (0, 0);
        for d in 0..=deepest {
            let at: Vec<_> = h.cells.iter().filter(|c| c.depth == d).collect();
            let with: Vec<_> = at.iter().filter(|c| !c.proxy.is_empty()).collect();
            let tris: usize = with.iter().map(|c| c.proxy_triangles()).sum();
            let bytes: usize = with.iter().map(|c| c.proxy_bytes()).sum();
            let edges: usize = with
                .iter()
                .flat_map(|c| &c.proxy)
                .map(|(_, m)| m.edges.len())
                .sum();
            let stands: u64 = with.iter().map(|c| c.original_triangles).sum();
            let (lo, hi) = with.iter().fold((f32::MAX, 0f32), |(l, h), c| {
                (l.min(c.error_mm), h.max(c.error_mm))
            });
            all_tris += tris;
            all_bytes += bytes;
            println!(
                "{d:>5}  {:>5}  {:>7}  {:>8.2} M  {:>8.3} M  {:>6.3} M  {}  {:>8.1}",
                at.len(),
                with.len(),
                stands as f64 / 1e6,
                tris as f64 / 1e6,
                edges as f64 / 1e6,
                if with.is_empty() {
                    "       -".to_string()
                } else if lo == hi {
                    format!("{lo:>8.0}")
                } else {
                    format!("{lo:.0}-{hi:.0}")
                },
                bytes as f64 / 1e6
            );
        }
        println!(
            "total: {} cells, {:.2} M proxy triangles, {:.1} MB\n",
            h.cells.len(),
            all_tris as f64 / 1e6,
            all_bytes as f64 / 1e6
        );
    }
}
