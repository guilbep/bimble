//! Round trip: `.bimble` → IFC4 → import → `.bimble` (ADR 0032). Every element must come back
//! with the same GlobalId, class, name, storey, material and generator — the point of the
//! exercise: an IFC a colleague sends becomes an editable Bimble model.

use bimble_core::demo::concrete_frame;
use bimble_core::file;
use bimble_core::{Element, Generator, IfcClass, Model, Placement};

/// Export a model to IFC4 STEP text.
fn export(m: &Model) -> String {
    bimble_ifc::export(m, "roundtrip.ifc", "2026-11-08T10:00:00")
}

fn import(text: &str) -> (Model, bimble_ifc::import::ImportReport) {
    bimble_ifc::import::import_str(text).expect("import succeeds")
}

#[test]
fn frame_round_trips_element_by_element() {
    let m = concrete_frame(&Default::default());
    let (back, rep) = import(&export(&m));
    assert_eq!(rep.elements, m.elements.len(), "report: {rep:?}");
    assert!(rep.skipped.is_empty(), "report: {rep:?}");
    assert_eq!(rep.parametric, m.elements.len(), "report: {rep:?}");

    // Storeys: same ids, names and elevations.
    assert_eq!(back.storeys.len(), m.storeys.len());
    for (id, s) in &m.storeys {
        let b = &back.storeys[id];
        assert_eq!(b.name, s.name);
        assert!(
            (b.elevation - s.elevation).abs() < 1e-6,
            "{} vs {}",
            b.elevation,
            s.elevation
        );
    }

    // Elements: same ids, classes, names, generators, placements, materials.
    assert_eq!(back.elements.len(), m.elements.len());
    for (id, e) in &m.elements {
        let b: &Element = &back.elements[id];
        assert_eq!(b.class, e.class, "{}", e.name);
        assert_eq!(b.name, e.name, "{}", e.name);
        assert_eq!(b.storey, e.storey, "{}", e.name);
        // Materials are deduplicated by name on import with derived ids: compare names.
        assert_eq!(
            back.materials[&b.material].name, m.materials[&e.material].name,
            "{}",
            e.name
        );
        compare_placement(&b.placement, &e.placement, e);
        compare_generator(&b.generator, &e.generator, e);
    }
    back.validate().expect("imported model validates");
}

/// The demo frame, saved and reloaded through the real `.bimble` file format.
#[test]
fn imported_model_saves_and_loads() {
    let m = concrete_frame(&Default::default());
    let (back, _) = import(&export(&m));
    let path = std::env::temp_dir().join("bimble-roundtrip-test.bimble");
    file::save(&back, &path).expect("save");
    let loaded = file::load(&path).expect("load");
    assert_eq!(loaded.elements.len(), m.elements.len());
    std::fs::remove_file(&path).ok();
}

/// Non-rectangular extrusions and explicit meshes round trip as ExtrudedPolygon / Mesh.
#[test]
fn extruded_polygon_and_mesh_round_trip() {
    let mut m = Model::new("shapes");
    let st = bimble_core::Storey {
        id: bimble_core::GlobalId::derived(b"storey/test"),
        name: "Niveau 1".into(),
        elevation: 0.0,
    };
    m.storeys.insert(st.id, st);
    let mat = bimble_core::Material {
        id: bimble_core::GlobalId::derived(b"material/test"),
        name: "C30/37".into(),
        category: bimble_core::MaterialCategory::Concrete,
        props: Default::default(),
    };
    m.materials.insert(mat.id, mat);
    let mat_id = bimble_core::GlobalId::derived(b"material/test");
    let st_id = bimble_core::GlobalId::derived(b"storey/test");

    let l_shape = vec![
        [0.0, 0.0],
        [2000.0, 0.0],
        [2000.0, 400.0],
        [400.0, 400.0],
        [0.0, 1000.0],
    ];
    let mesh = Generator::Mesh {
        positions: vec![
            [0.0, 0.0, 0.0],
            [1000.0, 0.0, 0.0],
            [1000.0, 1000.0, 0.0],
            [0.0, 1000.0, 0.0],
            [500.0, 500.0, 600.0],
        ],
        indices: vec![0, 2, 1, 0, 3, 2, 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4],
    };
    let elements = [
        Element {
            id: bimble_core::GlobalId::derived(b"element/lshape"),
            class: IfcClass::IfcColumn,
            name: "L".into(),
            storey: st_id,
            material: mat_id,
            placement: Placement::at(100.0, 200.0, 300.0),
            generator: Generator::ExtrudedPolygon {
                outline: l_shape,
                voids: vec![],
                height: 2500.0,
            },
            openings: Vec::new(),
            properties: Default::default(),
        },
        Element {
            id: bimble_core::GlobalId::derived(b"element/mesh"),
            class: IfcClass::IfcBeam,
            name: "M".into(),
            storey: st_id,
            material: mat_id,
            placement: Placement::at(0.0, 0.0, 0.0),
            generator: mesh,
            openings: Vec::new(),
            properties: Default::default(),
        },
    ];
    for e in &elements {
        m.elements.insert(e.id, e.clone());
    }
    m.validate().expect("model validates");

    let (back, rep) = import(&export(&m));
    assert_eq!(rep.elements, 2, "report: {rep:?}");
    assert_eq!(rep.extruded, 1, "report: {rep:?}");
    assert_eq!(rep.meshes, 1, "report: {rep:?}");

    let l = &back.elements[&elements[0].id];
    assert!(
        matches!(l.generator, Generator::ExtrudedPolygon { .. }),
        "{:?}",
        l.generator
    );
    let Generator::ExtrudedPolygon {
        outline, height, ..
    } = &l.generator
    else {
        unreachable!()
    };
    assert_eq!(outline.len(), 5);
    assert!((*height - 2500.0).abs() < 1e-6, "{height}");

    let mb = &back.elements[&elements[1].id];
    assert!(
        matches!(mb.generator, Generator::Mesh { .. }),
        "{:?}",
        mb.generator
    );
    let Generator::Mesh { positions, indices } = &mb.generator else {
        unreachable!()
    };
    assert_eq!(positions.len(), 5);
    assert_eq!(indices.len(), 18);
}

/// A wall with a window opening and property sets: everything comes back, and the opening is
/// cut from the wall's triangles each time (ADR 0032).
#[test]
fn openings_and_properties_round_trip() {
    let mut m = Model::new("wall with window");
    let st = bimble_core::Storey {
        id: bimble_core::GlobalId::derived(b"storey/test"),
        name: "Niveau 1".into(),
        elevation: 0.0,
    };
    let st_id = st.id;
    m.storeys.insert(st.id, st);
    let mat = bimble_core::Material {
        id: bimble_core::GlobalId::derived(b"material/test"),
        name: "C30/37".into(),
        category: bimble_core::MaterialCategory::Concrete,
        props: Default::default(),
    };
    let mat_id = mat.id;
    m.materials.insert(mat.id, mat);

    let mut e = Element {
        id: bimble_core::GlobalId::derived(b"element/wall"),
        class: IfcClass::IfcWall,
        name: "Mur".into(),
        storey: st_id,
        material: mat_id,
        placement: Placement {
            origin: [0.0, 100.0, 0.0],
            z_axis: [0.0, 0.0, 1.0],
            x_axis: [1.0, 0.0, 0.0],
        },
        generator: Generator::Wall {
            length: 4000.0,
            thickness: 200.0,
            height: 2700.0,
        },
        openings: vec![bimble_core::Opening {
            placement: Placement::at(1000.0, -100.0, 900.0),
            generator: Generator::ExtrudedPolygon {
                outline: vec![[0.0, 0.0], [1200.0, 0.0], [1200.0, 200.0], [0.0, 200.0]],
                voids: vec![],
                height: 1200.0,
            },
        }],
        properties: [
            ("Pset_WallCommon/FireRating".to_owned(), "REI 60".to_owned()),
            ("Pset_WallCommon/IsExternal".to_owned(), "T".to_owned()),
        ]
        .into_iter()
        .collect(),
    };
    m.elements.insert(e.id, e.clone());
    m.validate().expect("model validates");

    let (back, rep) = import(&export(&m));
    assert_eq!(rep.elements, 1, "report: {rep:?}");
    assert_eq!(rep.openings, 1, "report: {rep:?}");
    assert_eq!(rep.properties, 2, "report: {rep:?}");
    assert!(rep.skipped.is_empty(), "report: {rep:?}");

    let b: &Element = &back.elements[&e.id];
    assert_eq!(b.class, e.class);
    assert_eq!(b.name, e.name);
    compare_placement(&b.placement, &e.placement, &e);
    assert_eq!(b.openings.len(), 1);
    // The opening comes back as a mesh in element-local coordinates (its placement is baked
    // in), so compare the region it covers: placement ∘ outline, extruded along z.
    let region = |op: &bimble_core::Opening| {
        let x = op.generator.extrusion().expect("extrudes");
        let pts: Vec<[f64; 2]> = match &x.profile {
            bimble_core::Profile::Rectangle { x_dim, y_dim } => {
                let (hx, hy) = (x_dim / 2.0, y_dim / 2.0);
                vec![[-hx, -hy], [hx, -hy], [hx, hy], [-hx, hy]]
            }
            bimble_core::Profile::Polygon(pts) => pts.clone(),
        };
        let pl = &op.placement;
        let at = |p: [f64; 2], z: f64| {
            [
                pl.origin[0] + pl.x_axis[0] * p[0] + pl.y_axis()[0] * p[1],
                pl.origin[1] + pl.x_axis[1] * p[0] + pl.y_axis()[1] * p[1],
                pl.origin[2] + pl.x_axis[2] * p[0] + pl.y_axis()[2] * p[1] + z,
            ]
        };
        let mut pts: Vec<[f64; 3]> = pts
            .iter()
            .flat_map(|p| [at(*p, 0.0), at(*p, x.depth)])
            .collect();
        sort3(&mut pts);
        pts
    };
    let eb = region(&e.openings[0]);
    let Generator::Mesh { positions, .. } = &b.openings[0].generator else {
        panic!("expected a mesh opening");
    };
    assert!(!positions.is_empty());
    let mut bb: Vec<[f64; 3]> = positions.clone();
    sort3(&mut bb);
    // Same corners: the mesh welds the solid's 8 unique vertices.
    let mut eb8 = eb.clone();
    eb8.dedup();
    let mut bb8 = bb.clone();
    bb8.dedup();
    assert_eq!(eb8, bb8, "opening region");
    assert_eq!(b.properties, e.properties);
    back.validate().expect("imported model validates");

    // The opening is cut both before and after the trip, with the same volume lost.
    let lost = |el: &Element| {
        let solid = bimble_geom::extrude_tris(
            &bimble_core::Profile::Polygon(vec![
                [0.0, -100.0],
                [4000.0, -100.0],
                [4000.0, 100.0],
                [0.0, 100.0],
            ]),
            &[],
            2700.0,
        )
        .expect("extrudes");
        let full: f64 = solid
            .iter()
            .map(|t| {
                let [[ax, ay, az], [bx, by, bz], [cx, cy, cz]] = *t;
                (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx))
                    / 6.0
            })
            .sum::<f64>()
            .abs();
        let cut: f64 = bimble_geom::element_tris(el)
            .expect("cut")
            .iter()
            .map(|t| {
                let [[ax, ay, az], [bx, by, bz], [cx, cy, cz]] = *t;
                (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx))
                    / 6.0
            })
            .sum::<f64>()
            .abs();
        full - cut
    };
    let window = 1200.0 * 200.0 * 1200.0;
    assert!((lost(&e) - window).abs() < window * 1e-3, "{}", lost(&e));
    assert!((lost(b) - window).abs() < window * 1e-3, "{}", lost(b));

    let _ = &mut e; // e is only the reference model here
}

/// Sort 3D points lexicographically, for comparing vertex sets.
fn sort3(pts: &mut [[f64; 3]]) {
    pts.sort_by(|a, b| {
        a[0].total_cmp(&b[0])
            .then(a[1].total_cmp(&b[1]))
            .then(a[2].total_cmp(&b[2]))
    });
}

fn compare_placement(b: &Placement, e: &Placement, el: &Element) {
    for k in 0..3 {
        assert!(
            (b.origin[k] - e.origin[k]).abs() < 1e-6,
            "{} origin {k}: {} vs {}",
            el.name,
            b.origin[k],
            e.origin[k]
        );
        assert!(
            (b.z_axis[k] - e.z_axis[k]).abs() < 1e-6,
            "{} z_axis {k}",
            el.name
        );
        assert!(
            (b.x_axis[k] - e.x_axis[k]).abs() < 1e-6,
            "{} x_axis {k}",
            el.name
        );
    }
}

fn compare_generator(b: &Generator, e: &Generator, el: &Element) {
    match (b, e) {
        (
            Generator::RectColumn {
                width,
                depth,
                height,
            },
            Generator::RectColumn { .. },
        )
        | (
            Generator::PadFooting {
                width,
                depth,
                height,
            },
            Generator::PadFooting { .. },
        ) => {
            let (Generator::RectColumn {
                width: w2,
                depth: d2,
                height: h2,
            }
            | Generator::PadFooting {
                width: w2,
                depth: d2,
                height: h2,
            }) = e
            else {
                unreachable!()
            };
            for (a, x, n) in [
                (width, w2, "width"),
                (depth, d2, "depth"),
                (height, h2, "height"),
            ] {
                assert!((a - x).abs() < 1e-6, "{} {n}: {a} vs {x}", el.name);
            }
        }
        (
            Generator::RectBeam {
                width,
                height,
                length,
            },
            Generator::RectBeam { .. },
        ) => {
            let Generator::RectBeam {
                width: w2,
                height: h2,
                length: l2,
            } = e
            else {
                unreachable!()
            };
            for (a, x, n) in [
                (width, w2, "width"),
                (height, h2, "height"),
                (length, l2, "length"),
            ] {
                assert!((a - x).abs() < 1e-6, "{} {n}: {a} vs {x}", el.name);
            }
        }
        (Generator::Slab { outline, thickness }, Generator::Slab { .. }) => {
            let Generator::Slab {
                outline: o2,
                thickness: t2,
            } = e
            else {
                unreachable!()
            };
            assert!((thickness - t2).abs() < 1e-6, "{} thickness", el.name);
            assert_eq!(outline.len(), o2.len(), "{} outline", el.name);
        }
        (a, b2) => panic!("{}: generator changed: {a:?} from {b2:?}", el.name),
    }
}
