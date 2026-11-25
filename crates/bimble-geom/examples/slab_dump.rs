//! Print the canonical mesh of one rectangular IfcSlab, bit for bit, as golden data for the
//! C port of the slab geometry (spikes/c-slab).
//!
//! `slab_dump local`: the mesh from `element_mesh`, in element coordinates.
//! `slab_dump scene`: a one-element model through `Scene::from_model`: origin, mesh, edge normals.

use bimble_core::{
    Command, Element, Generator, GlobalId, History, IfcClass, Material, MaterialCategory, Model,
    Placement, Storey,
};
use bimble_geom::{Mesh, Scene};

fn hex32(c: f32) -> String {
    format!("{:08x}", c.to_bits())
}

fn print_mesh(m: &Mesh) {
    for p in &m.positions {
        println!("v {} {} {}", hex32(p[0]), hex32(p[1]), hex32(p[2]));
    }
    for t in m.indices.as_chunks::<3>().0 {
        println!("i {} {} {}", t[0], t[1], t[2]);
    }
    for [a, b] in &m.edges {
        let s: Vec<String> = a.iter().chain(b).map(|&c| hex32(c)).collect();
        println!("e {}", s.join(" "));
    }
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_default();
    let storey = Storey {
        id: GlobalId::from_u128(1),
        name: "Level 0".into(),
        elevation: 0.0,
    };
    let material = Material {
        id: GlobalId::from_u128(2),
        name: "C30/37".into(),
        category: MaterialCategory::Concrete,
        props: Default::default(),
    };
    let slab = Element {
        id: GlobalId::from_u128(3),
        class: IfcClass::IfcSlab,
        name: "D1".into(),
        storey: storey.id,
        material: material.id,
        placement: Placement::at(1000.0, 2000.0, 3000.0),
        generator: Generator::Slab {
            outline: vec![[0.0, 0.0], [6000.0, 0.0], [6000.0, 4000.0], [0.0, 4000.0]],
            thickness: 220.0,
        },
        openings: Vec::new(),
        properties: Default::default(),
        ifc_entity: None,
    };
    match mode.as_str() {
        "local" => print_mesh(&bimble_geom::element_mesh(&slab).expect("slab meshes")),
        "scene" => {
            let mut m = Model::new("slab");
            let cmds = vec![
                Command::AddStorey(storey),
                Command::AddMaterial(material),
                Command::AddElement(slab),
            ];
            History::default()
                .apply(&mut m, cmds)
                .expect("slab model is valid");
            let scene = Scene::from_model(&m);
            assert!(scene.warnings.is_empty(), "{:?}", scene.warnings);
            let o = scene.origin_mm;
            println!(
                "origin {:016x} {:016x} {:016x}",
                o[0].to_bits(),
                o[1].to_bits(),
                o[2].to_bits()
            );
            let mesh = scene.elements[0].local_mesh();
            print_mesh(mesh);
            for n in &mesh.edge_normals {
                let s: Vec<String> = n.iter().flatten().map(|v| v.to_string()).collect();
                println!("n {}", s.join(" "));
            }
        }
        _ => {
            eprintln!("usage: slab_dump local|scene");
            std::process::exit(2);
        }
    }
}
