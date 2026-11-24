//! Native dump of the slab mesh: `slab_dump local|scene`, same text as main_native.c.
use rust_slab::{SlabMesh, slab_mesh};

fn main() {
    let arg = std::env::args().nth(1).unwrap_or_default();
    if arg != "local" && arg != "scene" {
        eprintln!("usage: slab local|scene");
        std::process::exit(2);
    }
    let scene = arg == "scene";
    let outline = [[0.0, 0.0], [6000.0, 0.0], [6000.0, 4000.0], [0.0, 4000.0]];
    let mut m = Box::new(SlabMesh::ZERO);
    if slab_mesh(&outline, 220.0, &[1000.0, 2000.0, 3000.0], &mut m, scene).is_err() {
        eprintln!("slab does not mesh");
        std::process::exit(1);
    }
    let mut s = String::new();
    use std::fmt::Write;
    if scene {
        let o = m.origin.map(f64::to_bits);
        writeln!(s, "origin {:016x} {:016x} {:016x}", o[0], o[1], o[2]).unwrap();
    }
    for p in &m.pos[..m.nv] {
        writeln!(
            s,
            "v {:08x} {:08x} {:08x}",
            p[0].to_bits(),
            p[1].to_bits(),
            p[2].to_bits()
        )
        .unwrap();
    }
    for t in m.idx[..3 * m.nt].chunks(3) {
        writeln!(s, "i {} {} {}", t[0], t[1], t[2]).unwrap();
    }
    for e in &m.edges[..m.ne] {
        s.push('e');
        for c in e {
            write!(s, " {:08x}", c.to_bits()).unwrap();
        }
        s.push('\n');
    }
    if scene {
        for n in &m.normals[..m.ne] {
            s.push('n');
            for c in n {
                write!(s, " {c}").unwrap();
            }
            s.push('\n');
        }
    }
    print!("{s}");
}
