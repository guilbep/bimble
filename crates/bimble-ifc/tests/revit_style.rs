//! A Revit-style IFC4 fixture, written the way Revit writes walls: extruded solids from
//! polyline profiles (no typed rectangle profile), openings as `IfcOpeningElement` voids,
//! and property sets as `IfcPropertySingleValue` labels. The import must detect the rectangle
//! back into the parametric wall, keep the opening as a mesh cut into the element, and read
//! the properties as text (ADR 0032). It then round trips: export → import again.

use bimble_core::{Element, Generator, Placement};

/// The fixture: one storey, one Revit-style wall with a window opening and two property sets
/// (one of them Bimble's own generator pset, which must be skipped). Units are millimetres.
const REVIT_STYLE: &str = r#"ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [CoordinationView]'),'2;1');
FILE_NAME('revit-style.ifc','2026-11-08T10:00:00',(''),(''),'Revit','','');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
#1=IFCPROJECT('2MrcuFTa90SfJXy$ZbCd1A',$,'Projet',$,$,$,$,(#2),#3);
#2=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#4,$);
#3=IFCUNITASSIGNMENT((#5,#6,#7));
#4=IFCAXIS2PLACEMENT3D(#8,$,$);
#5=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#6=IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.);
#7=IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.);
#8=IFCCARTESIANPOINT((0.,0.,0.));
#9=IFCSITE('2MrcuFTa90SfJXy$ZbCd1B',$,'Site',$,$,#10,$,$,.ELEMENT.,$,$,$,$,$);
#10=IFCLOCALPLACEMENT($,#11);
#11=IFCAXIS2PLACEMENT3D(#8,$,$);
#12=IFCBUILDING('2MrcuFTa90SfJXy$ZbCd1C',$,'Batiment',$,$,#13,$,$,.ELEMENT.,$,$,$);
#13=IFCLOCALPLACEMENT(#10,#14);
#14=IFCAXIS2PLACEMENT3D(#8,$,$);
#15=IFCBUILDINGSTOREY('2MrcuFTa90SfJXy$ZbCd1D',$,'Niveau 1',$,$,#16,$,$,.ELEMENT.,0.);
#16=IFCLOCALPLACEMENT(#13,#17);
#17=IFCAXIS2PLACEMENT3D(#8,$,$);
#18=IFCRELAGGREGATES('2MrcuFTa90SfJXy$ZbCd1E',$,$,$,#1,(#9));
#19=IFCRELAGGREGATES('2MrcuFTa90SfJXy$ZbCd1F',$,$,$,#9,(#12));
#20=IFCRELAGGREGATES('2MrcuFTa90SfJXy$ZbCd1G',$,$,$,#12,(#15));
#21=IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*,#2,$,.MODEL_VIEW.,$);
#22=IFCWALLSTANDARDCASE('2MrcuFTa90SfJXy$ZbCd1H',$,'Basic Wall',$,$,#23,#31,$);
#23=IFCLOCALPLACEMENT(#16,#24);
#24=IFCAXIS2PLACEMENT3D(#8,$,$);
#25=IFCCARTESIANPOINT((0.,0.));
#26=IFCCARTESIANPOINT((4000.,0.));
#27=IFCCARTESIANPOINT((4000.,200.));
#28=IFCCARTESIANPOINT((0.,200.));
#29=IFCPOLYLINE((#25,#26,#27,#28,#25));
#30=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#29);
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#32));
#32=IFCSHAPEREPRESENTATION(#21,'Body','SweptSolid',(#33));
#33=IFCEXTRUDEDAREASOLID(#30,#34,#35,2700.);
#34=IFCAXIS2PLACEMENT3D(#8,$,$);
#35=IFCDIRECTION((0.,0.,1.));
#36=IFCRELCONTAINEDINSPATIALSTRUCTURE('2MrcuFTa90SfJXy$ZbCd1I',$,$,$,(#22),#15);
#37=IFCOPENINGELEMENT('2MrcuFTa90SfJXy$ZbCd1J',$,'Window',$,$,#38,#47,$,.OPENING.);
#38=IFCLOCALPLACEMENT(#23,#39);
#39=IFCAXIS2PLACEMENT3D(#40,$,$);
#40=IFCCARTESIANPOINT((1000.,0.,900.));
#41=IFCCARTESIANPOINT((0.,0.));
#42=IFCCARTESIANPOINT((1200.,0.));
#43=IFCCARTESIANPOINT((1200.,200.));
#44=IFCCARTESIANPOINT((0.,200.));
#45=IFCPOLYLINE((#41,#42,#43,#44,#41));
#46=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#45);
#47=IFCPRODUCTDEFINITIONSHAPE($,$,(#48));
#48=IFCSHAPEREPRESENTATION(#21,'Body','SweptSolid',(#49));
#49=IFCEXTRUDEDAREASOLID(#46,#50,#51,1200.);
#50=IFCAXIS2PLACEMENT3D(#8,$,$);
#51=IFCDIRECTION((0.,0.,1.));
#52=IFCRELVOIDSELEMENT('2MrcuFTa90SfJXy$ZbCd1K',$,$,$,#22,#37);
#53=IFCPROPERTYSINGLEVALUE('FireRating',$,IFCLABEL('REI 60'),$);
#54=IFCPROPERTYSINGLEVALUE('IsExternal',$,IFCBOOLEAN(.T.),$);
#55=IFCPROPERTYSINGLEVALUE('ThermalTransmittance',$,IFCREAL(0.35),$);
#56=IFCPROPERTYSET('2MrcuFTa90SfJXy$ZbCd1L',$,'Pset_WallCommon',$,(#53,#54,#55));
#57=IFCRELDEFINESBYPROPERTIES('2MrcuFTa90SfJXy$ZbCd1M',$,$,$,(#22),#56);
#58=IFCPROPERTYSINGLEVALUE('SomeParam',$,IFCINTEGER(3),$);
#59=IFCPROPERTYSET('2MrcuFTa90SfJXy$ZbCd1N',$,'Bimble_Generator',$,(#58));
#60=IFCRELDEFINESBYPROPERTIES('2MrcuFTa90SfJXy$ZbCd1O',$,$,$,(#22),#59);
#61=IFCPROPERTYSINGLEVALUE('Mark',$,IFCLABEL('W1'),$);
#62=IFCPROPERTYSET('2MrcuFTa90SfJXy$ZbCd1P',$,'Bimble_Properties',$,(#61));
#63=IFCRELDEFINESBYPROPERTIES('2MrcuFTa90SfJXy$ZbCd1Q',$,$,$,(#22),#62);
ENDSEC;
END-ISO-10303-21;
"#;

fn import(text: &str) -> (bimble_core::Model, bimble_ifc::import::ImportReport) {
    bimble_ifc::import::import_str(text).expect("import succeeds")
}

/// Signed volume of a closed triangle soup (divergence theorem, origin tetrahedra).
fn volume(tris: &[[[f64; 3]; 3]]) -> f64 {
    let mut v = 0.0;
    for t in tris {
        let [[ax, ay, az], [bx, by, bz], [cx, cy, cz]] = *t;
        v += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6.0;
    }
    v.abs()
}

#[test]
fn revit_wall_imports_parametric_with_cut_opening_and_psets() {
    let (m, rep) = import(REVIT_STYLE);
    assert_eq!(rep.elements, 1, "report: {rep:?}");
    assert_eq!(rep.parametric, 1, "report: {rep:?}");
    assert_eq!(rep.openings, 1, "report: {rep:?}");
    assert_eq!(rep.properties, 4, "report: {rep:?}");

    let e: &Element = m.elements.values().next().expect("the wall");
    assert_eq!(e.class, bimble_core::IfcClass::IfcWall);
    // Rectangle detected out of Revit's polyline profile.
    let Generator::Wall {
        length,
        thickness,
        height,
    } = &e.generator
    else {
        panic!("expected a parametric wall, got {:?}", e.generator);
    };
    assert!((*length - 4000.0).abs() < 1e-6, "{length}");
    assert!((*thickness - 200.0).abs() < 1e-6, "{thickness}");
    assert!((*height - 2700.0).abs() < 1e-6, "{height}");

    // The opening, in element-local coordinates: the window hole.
    assert_eq!(e.openings.len(), 1);
    let Generator::Mesh { positions, .. } = &e.openings[0].generator else {
        panic!("expected a mesh opening, got {:?}", e.openings[0].generator);
    };
    assert!(!positions.is_empty());
    let mut lo = positions[0];
    let mut hi = positions[0];
    for p in positions {
        for k in 0..3 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    // The opening is placed at (1000, 0, 900) on the wall, 1200 wide, 200 deep, 1200 high.
    for (k, (a, b)) in [
        (0usize, (1000.0, 2200.0)),
        (1, (-100.0, 100.0)),
        (2, (900.0, 2100.0)),
    ] {
        assert!((lo[k] - a).abs() < 1e-6, "min[{k}] = {} vs {a}", lo[k]);
        assert!((hi[k] - b).abs() < 1e-6, "max[{k}] = {} vs {b}", hi[k]);
    }

    // Property sets as text, under "Pset/Prop" keys; what Bimble wrote without a prefix
    // (Bimble_Properties) comes back under its bare name.
    assert_eq!(
        e.properties,
        [
            ("Pset_WallCommon/FireRating".to_owned(), "REI 60".to_owned()),
            ("Pset_WallCommon/IsExternal".to_owned(), "T".to_owned()),
            (
                "Pset_WallCommon/ThermalTransmittance".to_owned(),
                "0.35".to_owned()
            ),
            ("Mark".to_owned(), "W1".to_owned()),
        ]
        .into_iter()
        .collect()
    );

    // The opening is really cut: the wall's triangles lose the window's volume.
    let full = volume(
        &bimble_geom::extrude_tris(
            &bimble_core::Profile::Polygon(vec![
                [0.0, -100.0],
                [4000.0, -100.0],
                [4000.0, 100.0],
                [0.0, 100.0],
            ]),
            &[],
            2700.0,
        )
        .expect("wall extrudes"),
    );
    let cut = volume(&bimble_geom::element_tris(e).expect("element triangles"));
    let window = 1200.0 * 200.0 * 1200.0;
    assert!((full - cut - window).abs() < full * 1e-4, "{full} vs {cut}");
}

/// Double hop: import the fixture, export it, import again — properties and the opening
/// survive a second pass, and the model validates each time.
#[test]
fn revit_fixture_round_trips_twice() {
    let (first, rep1) = import(REVIT_STYLE);
    first.validate().expect("first import validates");
    let text = bimble_ifc::export(&first, "again.ifc", "2026-11-08T10:00:00");
    let (second, rep2) = import(&text);
    second.validate().expect("second import validates");

    assert_eq!(rep2.openings, rep1.openings, "report: {rep2:?}");
    let a: &Element = first.elements.values().next().unwrap();
    let b: &Element = second.elements.values().next().unwrap();
    assert_eq!(a.properties, b.properties);
    assert_eq!(a.openings.len(), b.openings.len());
    let va = volume(&bimble_geom::element_tris(a).unwrap());
    let vb = volume(&bimble_geom::element_tris(b).unwrap());
    assert!((va - vb).abs() < va * 1e-6, "{va} vs {vb}");

    // Saving and loading the imported model keeps everything too.
    let path = std::env::temp_dir().join("bimble-revit-style-test.bimble");
    bimble_core::file::save(&second, &path).expect("save");
    let loaded = bimble_core::file::load(&path).expect("load");
    std::fs::remove_file(&path).ok();
    let c: &Element = loaded.elements.values().next().unwrap();
    assert_eq!(c.properties, b.properties);
    assert_eq!(c.openings.len(), b.openings.len());
}

/// A mesh host (explicit geometry, no placement) gets its opening in the same storey-relative
/// space as its own triangles.
#[test]
fn opening_on_mesh_host_lands_in_element_space() {
    use bimble_core::{GlobalId, Model, Storey};
    let mut m = Model::new("mesh host");
    let st = Storey {
        id: GlobalId::derived(b"storey/test"),
        name: "Niveau 1".into(),
        elevation: 3000.0,
    };
    let st_id = st.id;
    m.storeys.insert(st.id, st);
    let mat = bimble_core::Material {
        id: GlobalId::derived(b"material/test"),
        name: "C30/37".into(),
        category: bimble_core::MaterialCategory::Concrete,
        props: Default::default(),
    };
    let mat_id = mat.id;
    m.materials.insert(mat.id, mat);
    let box_from_to = |a: [f64; 3], b: [f64; 3]| {
        let positions: Vec<[f64; 3]> = vec![
            [a[0], a[1], a[2]],
            [b[0], a[1], a[2]],
            [b[0], b[1], a[2]],
            [a[0], b[1], a[2]],
            [a[0], a[1], b[2]],
            [b[0], a[1], b[2]],
            [b[0], b[1], b[2]],
            [a[0], b[1], b[2]],
        ];
        let mut indices = Vec::new();
        for t in [
            [0, 2, 1],
            [0, 3, 2],
            [4, 5, 6],
            [4, 6, 7],
            [0, 1, 5],
            [0, 5, 4],
            [1, 2, 6],
            [1, 6, 5],
            [2, 3, 7],
            [2, 7, 6],
            [3, 0, 4],
            [3, 4, 7],
        ] {
            indices.extend(t.map(|i| i as u32));
        }
        Generator::Mesh { positions, indices }
    };
    // A slab-ish block: positions are element-local (storey-relative), storey at 3000.
    let e = Element {
        id: GlobalId::derived(b"element/slab"),
        class: bimble_core::IfcClass::IfcSlab,
        name: "Dalle".into(),
        storey: st_id,
        material: mat_id,
        placement: Placement::at(0.0, 0.0, 0.0),
        generator: box_from_to([0.0, 0.0, 0.0], [2000.0, 3000.0, 300.0]),
        openings: vec![bimble_core::Opening {
            id: None,
            placement: Placement::at(0.0, 0.0, 0.0),
            generator: box_from_to([500.0, 500.0, 0.0], [1500.0, 2500.0, 300.0]),
        }],
        properties: Default::default(),
    };
    m.elements.insert(e.id, e.clone());
    m.validate().expect("model validates");

    let (back, rep) = import(&bimble_ifc::export(&m, "mesh.ifc", "2026-11-08T10:00:00"));
    assert_eq!(rep.openings, 1, "report: {rep:?}");
    let b: &Element = back.elements.values().next().unwrap();
    assert_eq!(b.openings.len(), 1);
    let Generator::Mesh { positions, .. } = &b.openings[0].generator else {
        panic!("expected a mesh opening");
    };
    // Element space is storey-relative: everything shifted down by the 3000 elevation.
    let mut lo = positions[0];
    let mut hi = positions[0];
    for p in positions {
        for k in 0..3 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    for (k, (a, x)) in [
        (0usize, (500.0, 1500.0)),
        (1, (500.0, 2500.0)),
        (2, (0.0, 300.0)),
    ] {
        assert!((lo[k] - a).abs() < 1e-6, "min[{k}] = {} vs {a}", lo[k]);
        assert!((hi[k] - x).abs() < 1e-6, "max[{k}] = {} vs {x}", hi[k]);
    }
    // And the cut removes the hole from the host's triangles.
    let full = 2000.0 * 3000.0 * 300.0;
    let cut = volume(&bimble_geom::element_tris(b).unwrap());
    let hole = 1000.0 * 2000.0 * 300.0;
    assert!((full - cut - hole).abs() < full * 1e-4, "{full} vs {cut}");
}

/// World bounding box of each element, by GlobalId: (min, max) in mm.
fn world_boxes(scene: &bimble_geom::Scene) -> std::collections::BTreeMap<String, [[f64; 3]; 2]> {
    let mut out = std::collections::BTreeMap::new();
    for e in &scene.elements {
        let mut b = [[f64::INFINITY; 3], [f64::NEG_INFINITY; 3]];
        for p in &e.mesh.positions {
            for k in 0..3 {
                let v = p[k] as f64 + scene.origin_mm[k];
                b[0][k] = b[0][k].min(v);
                b[1][k] = b[1][k].max(v);
            }
        }
        out.insert(e.global_id.clone(), b);
    }
    out
}

/// The imported model, meshed, sits where the display reader (the reference) puts the IFC.
fn assert_same_place_as_reader(text: &str) -> bimble_core::Model {
    let (m, rep) = import(text);
    let got = world_boxes(&bimble_geom::Scene::from_model(&m));
    let want = world_boxes(&bimble_ifc::read::read_str(text).expect("reader"));
    assert_eq!(got.len(), want.len(), "report: {rep:?}");
    for (id, w) in &want {
        let g = got.get(id).unwrap_or_else(|| panic!("{id} not imported"));
        for (a, b) in g.iter().flatten().zip(w.iter().flatten()) {
            assert!((a - b).abs() < 0.5, "{id}: imported {g:?}, reader {w:?}");
        }
    }
    m
}

/// A rectangle whose polyline starts on a short edge, or runs the other way round, is still
/// the same box: the wall is not turned 90°, the column keeps its width along its long side.
#[test]
fn rectangle_detection_ignores_start_vertex_and_direction() {
    let reversed = REVIT_STYLE.replace(
        "#29=IFCPOLYLINE((#25,#26,#27,#28,#25));",
        "#29=IFCPOLYLINE((#25,#28,#27,#26,#25));",
    );
    let m = assert_same_place_as_reader(&reversed);
    let e = m.elements.values().next().unwrap();
    let Generator::Wall {
        length, thickness, ..
    } = e.generator
    else {
        panic!("expected a wall, got {:?}", e.generator);
    };
    assert_eq!((length, thickness), (4000.0, 200.0));

    let column = reversed.replace("IFCWALLSTANDARDCASE(", "IFCCOLUMN(");
    let m = assert_same_place_as_reader(&column);
    let e = m.elements.values().next().unwrap();
    let Generator::RectColumn { width, depth, .. } = e.generator else {
        panic!("expected a rect column, got {:?}", e.generator);
    };
    assert_eq!((width, depth), (4000.0, 200.0));
}

/// A rectangle turned 30° in its profile is detected and placed turned, as wall and column.
#[test]
fn rotated_rectangle_profile_keeps_its_rotation() {
    let rotated = REVIT_STYLE
        .replace(
            "#26=IFCCARTESIANPOINT((4000.,0.));",
            "#26=IFCCARTESIANPOINT((3464.1016151377546,2000.));",
        )
        .replace(
            "#27=IFCCARTESIANPOINT((4000.,200.));",
            "#27=IFCCARTESIANPOINT((3364.1016151377546,2173.2050807568877));",
        )
        .replace(
            "#28=IFCCARTESIANPOINT((0.,200.));",
            "#28=IFCCARTESIANPOINT((-100.,173.20508075688772));",
        )
        .replace("#52=IFCRELVOIDSELEMENT", "#52=IFCRELNOTHING");
    for text in [
        rotated.clone(),
        rotated.replace("IFCWALLSTANDARDCASE(", "IFCCOLUMN("),
    ] {
        let (_, rep) = import(&text);
        assert_eq!(rep.parametric, 1, "report: {rep:?}");
        assert_same_place_as_reader(&text);
    }
}

/// A storey placed off the origin, or turned, still puts its elements where the IFC says, and
/// the opening still cuts the window out of the wall.
#[test]
fn storey_offset_or_rotation_keeps_world_positions() {
    let offset = REVIT_STYLE.replace(
        "#17=IFCAXIS2PLACEMENT3D(#8,$,$);",
        "#17=IFCAXIS2PLACEMENT3D(#90,$,$);\n#90=IFCCARTESIANPOINT((5000.,0.,3000.));",
    );
    let turned = offset.replace(
        "#17=IFCAXIS2PLACEMENT3D(#90,$,$);",
        "#17=IFCAXIS2PLACEMENT3D(#90,$,#91);\n#91=IFCDIRECTION((0.,1.,0.));",
    );
    for text in [offset, turned] {
        let m = assert_same_place_as_reader(&text);
        let (_, rep) = import(&text);
        assert!(
            rep.warnings.iter().any(|w| w.contains("Niveau 1")),
            "the storey frame is reported: {rep:?}"
        );
        let e = m.elements.values().next().unwrap();
        let full = 4000.0 * 200.0 * 2700.0;
        let cut = volume(&bimble_geom::element_tris(e).unwrap());
        let window = 1200.0 * 200.0 * 1200.0;
        assert!((full - cut - window).abs() < full * 1e-4, "{full} vs {cut}");
    }
}

/// An element in no storey goes into the first one and keeps its world height (not raised by
/// that storey's elevation a second time).
#[test]
fn element_without_storey_keeps_world_height() {
    let text = REVIT_STYLE
        .replace(
            "#17=IFCAXIS2PLACEMENT3D(#8,$,$);",
            "#17=IFCAXIS2PLACEMENT3D(#90,$,$);\n#90=IFCCARTESIANPOINT((0.,0.,3000.));",
        )
        .replace("#36=IFCRELCONTAINEDINSPATIALSTRUCTURE", "#36=IFCRELNOTHING");
    assert_same_place_as_reader(&text);
}

/// The fixture with a second window in the wall, at x = 2500 (same body as the first).
fn two_openings() -> String {
    REVIT_STYLE.replace(
        "ENDSEC;\nEND-ISO",
        "#64=IFCOPENINGELEMENT('2MrcuFTa90SfJXy$ZbCd1R',$,'Window 2',$,$,#65,#47,$,.OPENING.);
#65=IFCLOCALPLACEMENT(#23,#66);
#66=IFCAXIS2PLACEMENT3D(#67,$,$);
#67=IFCCARTESIANPOINT((2500.,0.,900.));
#68=IFCRELVOIDSELEMENT('2MrcuFTa90SfJXy$ZbCd1S',$,$,$,#22,#64);
ENDSEC;\nEND-ISO",
    )
}

/// The file's limits hold for the whole import, not per element or opening: with room for one
/// opening's triangles, the second is reported as stopped, not imported.
#[test]
fn import_limits_cover_the_whole_file() {
    let text = two_openings();
    let (m, rep) = import(&text);
    assert_eq!(rep.openings, 2, "report: {rep:?}");
    let e = m.elements.values().next().unwrap();
    let Generator::Mesh { indices, .. } = &e.openings[0].generator else {
        panic!("mesh opening");
    };
    let limits = bimble_ifc::read::Limits {
        max_triangles: indices.len() / 3,
        ..Default::default()
    };
    let (_, rep) = bimble_ifc::import::import_str_with_limits(&text, limits).unwrap();
    assert_eq!(rep.openings, 1, "report: {rep:?}");
    assert!(
        rep.warnings.iter().any(|w| w.contains("stopped")),
        "report: {rep:?}"
    );
}

/// The fixture with more entities, sharing the wall's placement and body.
fn with_entities(extra: &str) -> String {
    REVIT_STYLE.replace("ENDSEC;\nEND-ISO", &format!("{extra}\nENDSEC;\nEND-ISO"))
}

fn skipped(rep: &bimble_ifc::import::ImportReport, what: &str) -> usize {
    rep.skipped
        .iter()
        .filter(|(k, _)| k.contains(what))
        .map(|(_, n)| n)
        .sum()
}

#[test]
fn wall_elemented_case_is_imported() {
    let (m, rep) = import(&with_entities(
        "#70=IFCWALLELEMENTEDCASE('2MrcuFTa90SfJXy$ZbCd1U',$,'Elemented',$,$,#23,#31,$,$);",
    ));
    assert_eq!(rep.elements, 2, "report: {rep:?}");
    assert_eq!(m.elements.len(), 2);
}

/// Nothing is dropped silently (ADR 0032): an unsupported product is in the report; the
/// spatial structure, openings and plain geometry entities are not.
#[test]
fn unsupported_products_are_reported() {
    let (_, rep) = import(REVIT_STYLE);
    assert!(rep.skipped.is_empty(), "report: {rep:?}");
    let (_, rep) = import(&with_entities(
        "#70=IFCROOF('2MrcuFTa90SfJXy$ZbCd1T',$,'Roof',$,$,#23,#31,$,$);",
    ));
    assert_eq!(skipped(&rep, "IFCROOF"), 1, "report: {rep:?}");
    assert_eq!(rep.skipped.len(), 1, "report: {rep:?}");
    // The report in words (CLI and viewer) says it too.
    let lines = rep.lines();
    assert!(
        lines
            .iter()
            .any(|l| l.starts_with("skipped:") && l.contains("IFCROOF")),
        "{lines:?}"
    );
}

/// A second product with the same GlobalId is skipped and reported, not written over the first;
/// the first one in the file wins.
#[test]
fn duplicate_global_ids_are_reported() {
    let (m, rep) = import(&with_entities(
        "#70=IFCWALL('2MrcuFTa90SfJXy$ZbCd1H',$,'Copy',$,$,#23,#31,$,$);",
    ));
    assert_eq!(rep.elements, 1, "report: {rep:?}");
    assert_eq!(skipped(&rep, "duplicate GlobalId"), 1, "report: {rep:?}");
    assert_eq!(m.elements.values().next().unwrap().name, "Basic Wall");
}

#[test]
fn invalid_global_ids_are_reported() {
    let (_, rep) = import(&with_entities(
        "#70=IFCWALL('not a GlobalId',$,'Bad',$,$,#23,#31,$,$);",
    ));
    assert_eq!(rep.elements, 1, "report: {rep:?}");
    assert_eq!(skipped(&rep, "invalid GlobalId"), 1, "report: {rep:?}");
}

/// An imported model is already canonical (no -0.0): its hash doesn't change when it is saved
/// and loaded again. A downward extrusion used to leave a z axis of [-0.0, -0.0, -1.0].
#[test]
fn imported_model_is_canonical() {
    use bimble_core::version::model_hash;
    let down = REVIT_STYLE.replace(
        "#35=IFCDIRECTION((0.,0.,1.));",
        "#35=IFCDIRECTION((0.,0.,-1.));",
    );
    let (m, _) = import(&down);
    let text = bimble_core::file::to_string(&m);
    let back = bimble_core::file::from_str(&text).unwrap();
    assert_eq!(model_hash(&m), model_hash(&back));
    assert!(!text.contains("-0.0"));
}

/// Openings keep their IfcOpeningElement GlobalId through import, .bimble and export; removing
/// one opening doesn't renumber the others.
#[test]
fn opening_global_ids_are_stable() {
    let (mut m, _) = import(&two_openings());
    let ids = |m: &bimble_core::Model| -> Vec<Option<String>> {
        m.elements
            .values()
            .next()
            .unwrap()
            .openings
            .iter()
            .map(|o| o.id.map(|g| g.to_string()))
            .collect()
    };
    assert_eq!(
        ids(&m),
        [
            Some("2MrcuFTa90SfJXy$ZbCd1J".to_owned()),
            Some("2MrcuFTa90SfJXy$ZbCd1R".to_owned())
        ]
    );
    let saved = bimble_core::file::from_str(&bimble_core::file::to_string(&m)).unwrap();
    assert_eq!(ids(&saved), ids(&m));

    m.elements.values_mut().next().unwrap().openings.remove(0);
    let text = bimble_ifc::export(&m, "one.ifc", "2026-11-08T10:00:00");
    assert!(text.contains("IFCOPENINGELEMENT('2MrcuFTa90SfJXy$ZbCd1R'"));
    let (back, _) = import(&text);
    assert_eq!(ids(&back), [Some("2MrcuFTa90SfJXy$ZbCd1R".to_owned())]);
}

/// A .bimble written before openings had ids still loads, and is written back the same way.
#[test]
fn openings_without_id_stay_without_id() {
    let (mut m, _) = import(REVIT_STYLE);
    m.elements.values_mut().next().unwrap().openings[0].id = None;
    let text = bimble_core::file::to_string(&m);
    assert!(!text.contains("\"id\": null"));
    let back = bimble_core::file::from_str(&text).unwrap();
    assert_eq!(back, m);
}

/// The fixture's project units are read: no "assuming millimetres" fallback.
#[test]
fn revit_fixture_length_unit_is_read() {
    let (_, rep) = import(REVIT_STYLE);
    assert!(
        !rep.warnings.iter().any(|w| w.contains("no length unit")),
        "warnings: {:?}",
        rep.warnings
    );
}

/// The same fixture in metres imports to the same model in mm, and the display reader puts
/// it in the same place.
#[test]
fn metre_units_import_like_millimetres() {
    let mut metres = REVIT_STYLE.to_string();
    for (from, to) in [
        (".MILLI.,.METRE.", "$,.METRE."),
        ("((4000.,0.))", "((4.,0.))"),
        ("((4000.,200.))", "((4.,0.2))"),
        ("((0.,200.))", "((0.,0.2))"),
        ("#35,2700.)", "#35,2.7)"),
        ("((1000.,0.,900.))", "((1.,0.,0.9))"),
        ("((1200.,0.))", "((1.2,0.))"),
        ("((1200.,200.))", "((1.2,0.2))"),
        ("#51,1200.)", "#51,1.2)"),
    ] {
        assert!(metres.contains(from), "{from}");
        metres = metres.replace(from, to);
    }
    let (mm, _) = import(REVIT_STYLE);
    let (m, rep) = import(&metres);
    assert!(
        !rep.warnings.iter().any(|w| w.contains("no length unit")),
        "warnings: {:?}",
        rep.warnings
    );
    let (a, b) = (
        mm.elements.values().next().unwrap(),
        m.elements.values().next().unwrap(),
    );
    let close = |x: &[f64], y: &[f64]| {
        x.len() == y.len() && x.iter().zip(y).all(|(p, q)| (p - q).abs() < 1e-6)
    };
    let (
        Generator::Wall {
            length,
            thickness,
            height,
        },
        Generator::Wall {
            length: l2,
            thickness: t2,
            height: h2,
        },
    ) = (&a.generator, &b.generator)
    else {
        panic!("expected walls: {:?} / {:?}", a.generator, b.generator);
    };
    assert!(
        close(&[*length, *thickness, *height], &[*l2, *t2, *h2]),
        "{:?}",
        b.generator
    );
    let p = |e: &Element| -> Vec<f64> {
        let Placement {
            origin,
            z_axis,
            x_axis,
        } = &e.placement;
        origin.iter().chain(z_axis).chain(x_axis).copied().collect()
    };
    assert!(close(&p(a), &p(b)), "{:?}", b.placement);
    let opening = |e: &Element| -> Vec<f64> {
        let Generator::Mesh { positions, .. } = &e.openings[0].generator else {
            panic!("expected a mesh opening");
        };
        positions.iter().flatten().copied().collect()
    };
    assert!(close(&opening(a), &opening(b)));
    // The display reader scales the same way.
    let boxes = |t: &str| -> Vec<f64> {
        let s = bimble_ifc::read::read_str(t).expect("reader");
        world_boxes(&s)
            .values()
            .flatten()
            .flatten()
            .copied()
            .collect()
    };
    let (ra, rb) = (boxes(REVIT_STYLE), boxes(&metres));
    assert!(!ra.is_empty());
    assert!(
        ra.iter().zip(&rb).all(|(p, q)| (p - q).abs() < 1e-3),
        "{ra:?} vs {rb:?}"
    );
}
