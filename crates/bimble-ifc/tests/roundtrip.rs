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
        },
        Element {
            id: bimble_core::GlobalId::derived(b"element/mesh"),
            class: IfcClass::IfcBeam,
            name: "M".into(),
            storey: st_id,
            material: mat_id,
            placement: Placement::at(0.0, 0.0, 0.0),
            generator: mesh,
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
