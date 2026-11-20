//! Read-only queries over a [`Model`]. Results are small and JSON-ready: callers include LLM
//! agents, so element lists are capped and meshes are reduced to counts.

use bimble_core::schedule::ScheduleDef;
use bimble_core::sheet::Placed;
use bimble_core::view::{Crop, ViewKind};
use bimble_core::{Generator, GlobalId, IfcClass, MaterialCategory, Model, Placement};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// Elements returned when the filter sets no limit.
pub const DEFAULT_LIMIT: usize = 1_000;
/// Hard cap on elements returned, whatever the filter asks: callers are untrusted.
pub const MAX_ELEMENTS: usize = 10_000;

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct StoreySummary {
    pub id: GlobalId,
    pub name: String,
    /// mm
    pub elevation: f64,
    pub element_count: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct MaterialSummary {
    pub id: GlobalId,
    pub name: String,
    pub category: MaterialCategory,
    pub element_count: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ElementFilter {
    /// IFC class name, e.g. "IfcColumn".
    pub class: Option<IfcClass>,
    pub storey: Option<GlobalId>,
    /// Defaults to [`DEFAULT_LIMIT`], capped at [`MAX_ELEMENTS`].
    pub limit: Option<usize>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ElementSummary {
    pub id: GlobalId,
    pub class: IfcClass,
    pub name: String,
    pub storey: GlobalId,
    pub material: GlobalId,
    /// Generator kind, e.g. "rect_column", "mesh".
    pub generator: &'static str,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ElementDetail {
    #[serde(flatten)]
    pub summary: ElementSummary,
    pub storey_name: Option<String>,
    pub material_name: Option<String>,
    pub placement: Placement,
    /// The generator's parameters; for a mesh only `kind`, `triangles` and `vertices`.
    pub generator_params: serde_json::Value,
    pub openings: usize,
    pub properties: BTreeMap<String, String>,
}

fn counts(ids: impl Iterator<Item = GlobalId>) -> BTreeMap<GlobalId, usize> {
    let mut n = BTreeMap::new();
    for id in ids {
        *n.entry(id).or_insert(0) += 1;
    }
    n
}

/// Storeys by elevation, then id.
pub fn storeys(m: &Model) -> Vec<StoreySummary> {
    let n = counts(m.elements.values().map(|e| e.storey));
    let mut v: Vec<_> = m
        .storeys
        .values()
        .map(|s| StoreySummary {
            id: s.id,
            name: s.name.clone(),
            elevation: s.elevation,
            element_count: n.get(&s.id).copied().unwrap_or(0),
        })
        .collect();
    v.sort_by(|a, b| a.elevation.total_cmp(&b.elevation).then(a.id.cmp(&b.id)));
    v
}

/// Materials by id.
pub fn materials(m: &Model) -> Vec<MaterialSummary> {
    let n = counts(m.elements.values().map(|e| e.material));
    m.materials
        .values()
        .map(|x| MaterialSummary {
            id: x.id,
            name: x.name.clone(),
            category: x.category,
            element_count: n.get(&x.id).copied().unwrap_or(0),
        })
        .collect()
}

fn summary(e: &bimble_core::Element) -> ElementSummary {
    ElementSummary {
        id: e.id,
        class: e.class,
        name: e.name.clone(),
        storey: e.storey,
        material: e.material,
        generator: e.generator.name(),
    }
}

/// Elements matching the filter, by GlobalId, at most [`MAX_ELEMENTS`].
pub fn elements(m: &Model, filter: &ElementFilter) -> Vec<ElementSummary> {
    let limit = filter.limit.unwrap_or(DEFAULT_LIMIT).min(MAX_ELEMENTS);
    m.elements
        .values()
        .filter(|e| filter.class.is_none_or(|c| e.class == c))
        .filter(|e| filter.storey.is_none_or(|s| e.storey == s))
        .take(limit)
        .map(summary)
        .collect()
}

/// One element in full, except mesh data, which is reduced to counts.
pub fn element(m: &Model, id: GlobalId) -> Option<ElementDetail> {
    let e = m.elements.get(&id)?;
    let generator_params = match &e.generator {
        Generator::Mesh { positions, indices } => serde_json::json!({
            "kind": "mesh",
            "triangles": indices.len() / 3,
            "vertices": positions.len(),
        }),
        g => serde_json::to_value(g).unwrap_or(serde_json::Value::Null),
    };
    Some(ElementDetail {
        summary: summary(e),
        storey_name: m.storeys.get(&e.storey).map(|s| s.name.clone()),
        material_name: m.materials.get(&e.material).map(|x| x.name.clone()),
        placement: e.placement.clone(),
        generator_params,
        openings: e.openings.len(),
        properties: e.properties.clone(),
    })
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ProjectSummary {
    pub id: GlobalId,
    pub name: String,
    /// Project information (ADR 0030): client, number, phase…
    pub info: BTreeMap<String, String>,
}

/// A view without its drawing content: references and annotations are counted.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ViewSummary {
    pub id: GlobalId,
    pub name: String,
    /// `{"type": "three_d"}`, `{"type": "plan", storey, look, range}` or
    /// `{"type": "section", origin, facing, depth_mm}`, as stored.
    #[serde(flatten)]
    pub kind: ViewKind,
    /// 1:N
    pub scale: u32,
    /// World mm in the view's plane; null: all of it.
    pub crop: Option<Crop>,
    pub true_north: bool,
    pub references: usize,
    pub annotations: usize,
}

/// A sheet without its drawings: placed views by id.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct SheetSummary {
    pub id: GlobalId,
    pub number: String,
    pub name: String,
    /// "A4" … "A0".
    pub paper: String,
    pub portrait: bool,
    /// Title block values of this sheet.
    pub fields: BTreeMap<String, String>,
    pub placed: Vec<Placed>,
    pub texts: usize,
}

pub fn project(m: &Model) -> ProjectSummary {
    ProjectSummary {
        id: m.project_id,
        name: m.project_name.clone(),
        info: m.project.clone(),
    }
}

/// Views by GlobalId.
pub fn views(m: &Model) -> Vec<ViewSummary> {
    m.views
        .values()
        .map(|v| ViewSummary {
            id: v.id,
            name: v.name.clone(),
            kind: v.kind.clone(),
            scale: v.scale,
            crop: v.crop,
            true_north: v.true_north,
            references: v.references.len(),
            annotations: v.annotations.len(),
        })
        .collect()
}

/// Sheets by GlobalId; placed views in drawing order.
pub fn sheets(m: &Model) -> Vec<SheetSummary> {
    m.sheets
        .values()
        .map(|s| SheetSummary {
            id: s.id,
            number: s.number.clone(),
            name: s.name.clone(),
            paper: s.paper.clone(),
            portrait: s.portrait,
            fields: s.fields.clone(),
            placed: s.placed.clone(),
            texts: s.texts.len(),
        })
        .collect()
}

/// Schedule definitions as stored (not their rows).
pub fn schedules(m: &Model) -> &[ScheduleDef] {
    &m.schedules
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::{Element, Material, Storey};

    struct Fx {
        m: Model,
        ground: GlobalId,
        level1: GlobalId,
        concrete: GlobalId,
        steel: GlobalId,
        beam: GlobalId,
        proxy: GlobalId,
    }

    /// 2 storeys (inserted high one first), 2 materials, 3 elements: a column on the ground
    /// floor, a beam and a 4-vertex 2-triangle mesh proxy on level 1.
    fn fixture() -> Fx {
        let id = |s: &str| GlobalId::derived(s.as_bytes());
        let (ground, level1) = (id("storey/ground"), id("storey/level1"));
        let (concrete, steel) = (id("mat/concrete"), id("mat/steel"));
        let (col, beam, proxy) = (id("el/col"), id("el/beam"), id("el/proxy"));
        let mut m = Model::new("fixture");
        for (sid, name, elevation) in [(level1, "Level 1", 3000.0), (ground, "Ground", 0.0)] {
            m.storeys.insert(
                sid,
                Storey {
                    id: sid,
                    name: name.into(),
                    elevation,
                },
            );
        }
        for (mid, name, category) in [
            (concrete, "C30/37", MaterialCategory::Concrete),
            (steel, "S355", MaterialCategory::Steel),
        ] {
            m.materials.insert(
                mid,
                Material {
                    id: mid,
                    name: name.into(),
                    category,
                    props: Default::default(),
                },
            );
        }
        let el = |eid, class, storey, material, generator| Element {
            id: eid,
            class,
            name: format!("{eid}"),
            storey,
            material,
            placement: Placement::at(0.0, 0.0, 0.0),
            generator,
            openings: Vec::new(),
            properties: BTreeMap::new(),
            ifc_entity: None,
        };
        let mut p = el(
            proxy,
            IfcClass::IfcBuildingElementProxy,
            level1,
            steel,
            Generator::Mesh {
                positions: vec![
                    [0.0, 0.0, 0.0],
                    [1000.0, 0.0, 0.0],
                    [1000.0, 1000.0, 0.0],
                    [0.0, 1000.0, 0.0],
                ]
                .into(),
                indices: vec![0, 1, 2, 0, 2, 3].into(),
            },
        );
        p.properties.insert("Pset_Test/Ref".into(), "A1".into());
        for e in [
            el(
                col,
                IfcClass::IfcColumn,
                ground,
                concrete,
                Generator::RectColumn {
                    width: 300.0,
                    depth: 300.0,
                    height: 3000.0,
                },
            ),
            el(
                beam,
                IfcClass::IfcBeam,
                level1,
                concrete,
                Generator::RectBeam {
                    width: 250.0,
                    height: 500.0,
                    length: 6000.0,
                },
            ),
            p,
        ] {
            m.elements.insert(e.id, e);
        }
        m.validate().unwrap();
        Fx {
            m,
            ground,
            level1,
            concrete,
            steel,
            beam,
            proxy,
        }
    }

    /// The element fixture plus 2 views (a 1:50 plan on Ground, cropped; a 3D view), 1 A3
    /// sheet placing the plan, 2 project keys, 1 schedule.
    fn drawings() -> (Fx, GlobalId, GlobalId, GlobalId) {
        use bimble_core::sheet::Sheet;
        use bimble_core::view::{Look, View, ViewRange};
        let mut f = fixture();
        let (plan, three_d, sheet) = (
            GlobalId::derived(b"view/plan"),
            GlobalId::derived(b"view/3d"),
            GlobalId::derived(b"sheet/1"),
        );
        let view = |id, name: &str, kind| View {
            id,
            name: name.into(),
            kind,
            visibility: Default::default(),
            references: Vec::new(),
            annotations: Vec::new(),
            true_north: false,
            scale: 100,
            crop: None,
        };
        let mut p = view(
            plan,
            "Ground plan",
            ViewKind::Plan {
                storey: f.ground.to_string(),
                look: Look::Down,
                range: ViewRange::default_for(Look::Down),
            },
        );
        p.scale = 50;
        p.true_north = true;
        p.crop = Some(Crop {
            min: [0.0, 0.0],
            max: [6000.0, 4000.0],
        });
        for v in [p, view(three_d, "3D", ViewKind::ThreeD)] {
            f.m.views.insert(v.id, v);
        }
        f.m.sheets.insert(
            sheet,
            Sheet {
                id: sheet,
                number: "S-101".into(),
                name: "Plans".into(),
                paper: "A3".into(),
                portrait: false,
                fields: BTreeMap::new(),
                placed: vec![Placed {
                    id: GlobalId::derived(b"placed/1"),
                    view: plan,
                    scale: 50,
                    at_mm: [210.0, 148.5],
                }],
                texts: Vec::new(),
            },
        );
        f.m.project = BTreeMap::from([
            ("client".to_string(), "Acme".to_string()),
            ("number".to_string(), "P-42".to_string()),
        ]);
        f.m.schedules = vec![ScheduleDef {
            name: "Columns".into(),
            classes: vec!["IfcColumn".into()],
            family: Some(MaterialCategory::Concrete),
            fields: vec![
                bimble_core::schedule::Field::Level,
                bimble_core::schedule::Field::Name,
            ],
            group_by_level: true,
            totals: false,
        }];
        f.m.validate().unwrap();
        (f, plan, three_d, sheet)
    }

    #[test]
    fn project_views_sheets_schedules() {
        let (f, plan, three_d, sheet) = drawings();
        let p = project(&f.m);
        assert_eq!((p.id, p.name.as_str()), (f.m.project_id, "fixture"));
        assert_eq!(p.info.keys().collect::<Vec<_>>(), ["client", "number"]);

        let v = views(&f.m);
        let mut want = vec![plan, three_d];
        want.sort();
        assert_eq!(v.iter().map(|v| v.id).collect::<Vec<_>>(), want);
        let p = v.iter().find(|v| v.id == plan).unwrap();
        assert_eq!((p.scale, p.true_north), (50, true));
        let j = serde_json::to_value(p).unwrap();
        assert_eq!(j["type"], "plan");
        assert_eq!(j["storey"], f.ground.to_string());
        assert_eq!(j["look"], "down");
        assert_eq!(j["crop"]["max"], serde_json::json!([6000.0, 4000.0]));
        assert!(j["range"]["cut"].is_object(), "{j}");
        let t = serde_json::to_value(v.iter().find(|v| v.id == three_d).unwrap()).unwrap();
        assert_eq!(
            (t["type"].as_str(), t["scale"].as_u64()),
            (Some("three_d"), Some(100))
        );
        assert!(t["crop"].is_null() && t.get("storey").is_none(), "{t}");

        let s = sheets(&f.m);
        assert_eq!(s.len(), 1);
        assert_eq!(
            (s[0].id, s[0].number.as_str(), s[0].paper.as_str()),
            (sheet, "S-101", "A3")
        );
        assert_eq!(
            s[0].placed.iter().map(|p| p.view).collect::<Vec<_>>(),
            [plan]
        );

        assert_eq!(schedules(&f.m), &f.m.schedules[..]);
        assert_eq!(schedules(&f.m)[0].name, "Columns");
    }

    #[test]
    fn empty_model_reads_empty() {
        let m = Model::new("empty");
        assert!(project(&m).info.is_empty());
        assert!(views(&m).is_empty() && sheets(&m).is_empty() && schedules(&m).is_empty());
    }

    #[test]
    fn storeys_sorted_by_elevation_with_counts() {
        let f = fixture();
        let s = storeys(&f.m);
        let got: Vec<_> = s.iter().map(|s| (s.id, s.element_count)).collect();
        assert_eq!(got, vec![(f.ground, 1), (f.level1, 2)]);
        assert_eq!(s[1].elevation, 3000.0);
    }

    #[test]
    fn material_counts() {
        let f = fixture();
        let n: BTreeMap<_, _> = materials(&f.m)
            .into_iter()
            .map(|x| (x.id, x.element_count))
            .collect();
        assert_eq!(n, BTreeMap::from([(f.concrete, 2), (f.steel, 1)]));
    }

    #[test]
    fn filter_by_class_storey_and_limit() {
        let f = fixture();
        let ids = |flt: ElementFilter| -> Vec<GlobalId> {
            elements(&f.m, &flt).into_iter().map(|e| e.id).collect()
        };
        assert_eq!(
            ids(ElementFilter {
                class: Some(IfcClass::IfcBeam),
                ..Default::default()
            }),
            vec![f.beam]
        );
        let mut on_level1 = vec![f.beam, f.proxy];
        on_level1.sort();
        assert_eq!(
            ids(ElementFilter {
                storey: Some(f.level1),
                ..Default::default()
            }),
            on_level1
        );
        assert_eq!(ids(ElementFilter::default()).len(), 3);
        let one = ElementFilter {
            limit: Some(1),
            ..Default::default()
        };
        assert_eq!(ids(one).len(), 1);
        let huge = ElementFilter {
            limit: Some(usize::MAX),
            ..Default::default()
        };
        assert_eq!(ids(huge).len(), 3);
    }

    #[test]
    fn filter_reads_ifc_class_names() {
        let flt: ElementFilter =
            serde_json::from_str(r#"{"class":"IfcBuildingElementProxy","limit":20000}"#).unwrap();
        assert_eq!(flt.class, Some(IfcClass::IfcBuildingElementProxy));
        assert!(serde_json::from_str::<ElementFilter>(r#"{"class":"Column"}"#).is_err());
    }

    #[test]
    fn mesh_detail_has_counts_not_vertices() {
        let f = fixture();
        let d = element(&f.m, f.proxy).unwrap();
        assert_eq!(
            d.generator_params,
            serde_json::json!({"kind": "mesh", "triangles": 2, "vertices": 4})
        );
        assert_eq!(d.storey_name.as_deref(), Some("Level 1"));
        assert_eq!(d.material_name.as_deref(), Some("S355"));
        assert_eq!(d.properties["Pset_Test/Ref"], "A1");
        let json = serde_json::to_string(&d).unwrap();
        assert!(
            !json.contains("positions") && !json.contains("indices"),
            "{json}"
        );
        assert!(
            json.contains("\"class\":\"IfcBuildingElementProxy\""),
            "{json}"
        );
    }

    #[test]
    fn extrusion_detail_keeps_parameters() {
        let f = fixture();
        let d = element(&f.m, f.beam).unwrap();
        assert_eq!(d.generator_params["kind"], "rect_beam");
        assert_eq!(d.generator_params["length"], 6000.0);
        assert_eq!(d.summary.generator, "rect_beam");
    }

    #[test]
    fn unknown_element_is_none() {
        let f = fixture();
        assert!(element(&f.m, GlobalId::derived(b"nope")).is_none());
    }
}
