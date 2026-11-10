//! IFC4 (ADD2 TC1) STEP export.
//!
//! Output is deterministic: entity ids follow model order, and relationship GlobalIds are derived
//! from the model's own ids. Exporting the same model twice gives byte-identical files (minus the
//! timestamp, which the caller controls), so exported IFC is diffable too.
//!
//! Each element gets explicit SweptSolid geometry (readable by any IFC tool) plus a
//! `Bimble_Generator` property set holding the generator name, version and parameters.

pub mod import;
pub mod read;
pub mod step;

use bimble_core::{Element, Generator, GlobalId, IfcClass, Model, Profile};
use std::collections::HashMap;
use std::fmt::Write;

pub const GENERATOR_PSET: &str = "Bimble_Generator";
/// The project information's property set on IfcProject (ADR 0030).
pub const PROJECT_PSET: &str = "Bimble_ProjectInformation";
/// Element properties written without a "Pset/" prefix (ADR 0032).
pub const PROPERTIES_PSET: &str = "Bimble_Properties";

/// Format a STEP REAL: always contains a '.', exponent as 'E'.
fn real(x: f64) -> String {
    let s = format!("{x:?}");
    match s.split_once('e') {
        Some((m, e)) => {
            let m = if m.contains('.') {
                m.to_owned()
            } else {
                format!("{m}.")
            };
            format!("{m}E{e}")
        }
        None if s.contains('.') => s,
        None => format!("{s}."),
    }
}

/// Encode a STEP string literal (ISO 10303-21): quotes doubled, backslash escaped,
/// non-ASCII as \X2\…\X0\ UTF-16 hex.
fn string(s: &str) -> String {
    let mut out = String::from("'");
    let mut wide: Vec<u16> = Vec::new();
    let flush = |wide: &mut Vec<u16>, out: &mut String| {
        if !wide.is_empty() {
            out.push_str("\\X2\\");
            for u in wide.drain(..) {
                write!(out, "{u:04X}").unwrap();
            }
            out.push_str("\\X0\\");
        }
    };
    for c in s.chars() {
        if c.is_ascii() && !c.is_ascii_control() {
            flush(&mut wide, &mut out);
            match c {
                '\'' => out.push_str("''"),
                '\\' => out.push_str("\\\\"),
                _ => out.push(c),
            }
        } else {
            let mut buf = [0u16; 2];
            wide.extend_from_slice(c.encode_utf16(&mut buf));
        }
    }
    flush(&mut wide, &mut out);
    out.push('\'');
    out
}

fn point3(p: [f64; 3]) -> String {
    format!(
        "IFCCARTESIANPOINT(({},{},{}))",
        real(p[0]),
        real(p[1]),
        real(p[2])
    )
}

fn dir3(d: [f64; 3]) -> String {
    format!(
        "IFCDIRECTION(({},{},{}))",
        real(d[0]),
        real(d[1]),
        real(d[2])
    )
}

struct Writer {
    data: String,
    next: usize,
}

impl Writer {
    fn add(&mut self, entity: impl AsRef<str>) -> usize {
        let id = self.next;
        self.next += 1;
        writeln!(self.data, "#{id}={};", entity.as_ref()).unwrap();
        id
    }
}

fn refs(ids: &[usize]) -> String {
    ids.iter()
        .map(|i| format!("#{i}"))
        .collect::<Vec<_>>()
        .join(",")
}

/// An IfcFacetedBrep of an explicit triangle mesh, in the element's local space. Point entities
/// are shared and created in first-use order, so the output stays deterministic.
fn faceted_brep(w: &mut Writer, positions: &[[f64; 3]], indices: &[u32]) -> usize {
    let mut point_id: HashMap<[u64; 3], usize> = HashMap::new();
    let mut pt = |w: &mut Writer, p: [f64; 3]| {
        *point_id
            .entry(p.map(|c| c.to_bits()))
            .or_insert_with(|| w.add(point3(p)))
    };
    let mut faces: Vec<usize> = Vec::with_capacity(indices.len() / 3);
    for t in indices.as_chunks::<3>().0 {
        let loop_pts: Vec<usize> = t.iter().map(|i| pt(w, positions[*i as usize])).collect();
        let poly = w.add(format!("IFCPOLYLOOP(({}))", refs(&loop_pts)));
        let bound = w.add(format!("IFCFACEOUTERBOUND(#{poly},.T.)"));
        faces.push(w.add(format!("IFCFACE((#{bound}))")));
    }
    let shell = w.add(format!("IFCCLOSEDSHELL(({}))", refs(&faces)));
    w.add(format!("IFCFACETEDBREP(#{shell})"))
}

/// A body solid in the element's local space: `(representation type, solid id)`. A `Mesh`
/// generator becomes an IfcFacetedBrep; an extrusion becomes a profile + IfcExtrudedAreaSolid
/// (a rectangle without voids keeps its typed profile, for round-trip-friendly output).
fn write_geometry(w: &mut Writer, g: &Generator, world: usize, z: usize) -> (&'static str, usize) {
    match g {
        Generator::Mesh { positions, indices } => {
            ("FacetedBrep", faceted_brep(w, positions, indices))
        }
        g => {
            let x = g.extrusion().expect("non-mesh generators extrude");
            let profile = match (&x.profile, x.voids.is_empty()) {
                (Profile::Rectangle { x_dim, y_dim }, true) => w.add(format!(
                    "IFCRECTANGLEPROFILEDEF(.AREA.,$,$,{},{})",
                    real(*x_dim),
                    real(*y_dim)
                )),
                (profile, with_voids) => {
                    let mut polyline = |pts: &[[f64; 2]]| {
                        let mut ids: Vec<usize> = pts
                            .iter()
                            .map(|q| {
                                w.add(format!(
                                    "IFCCARTESIANPOINT(({},{}))",
                                    real(q[0]),
                                    real(q[1])
                                ))
                            })
                            .collect();
                        ids.push(ids[0]); // closed polyline repeats the first point
                        w.add(format!("IFCPOLYLINE(({}))", refs(&ids)))
                    };
                    let rect_pts = |x_dim: &f64, y_dim: &f64| {
                        let (x, y) = (x_dim / 2.0, y_dim / 2.0);
                        vec![[-x, -y], [x, -y], [x, y], [-x, y]]
                    };
                    let outer_pts: Vec<[f64; 2]> = match profile {
                        Profile::Rectangle { x_dim, y_dim } => rect_pts(x_dim, y_dim),
                        Profile::Polygon(pts) => pts.clone(),
                    };
                    let outer = polyline(&outer_pts);
                    if with_voids {
                        let inners = x
                            .voids
                            .iter()
                            .map(|v| format!("#{}", polyline(v)))
                            .collect::<Vec<_>>()
                            .join(",");
                        w.add(format!(
                            "IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#{outer},({inners}))"
                        ))
                    } else {
                        w.add(format!("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#{outer})"))
                    }
                }
            };
            let solid = w.add(format!(
                "IFCEXTRUDEDAREASOLID(#{profile},#{world},#{z},{})",
                real(x.depth)
            ));
            ("SweptSolid", solid)
        }
    }
}

fn predefined(e: &Element) -> &'static str {
    match e.class {
        IfcClass::IfcColumn => ".COLUMN.",
        IfcClass::IfcBeam => ".BEAM.",
        IfcClass::IfcSlab => ".FLOOR.",
        IfcClass::IfcWall => ".SOLIDWALL.",
        IfcClass::IfcFooting => ".PAD_FOOTING.",
        // Its own class goes in ObjectType (ADR 0042), which USERDEFINED asks for.
        IfcClass::IfcBuildingElementProxy if e.ifc_entity.is_some() => ".USERDEFINED.",
        IfcClass::IfcBuildingElementProxy => ".NOTDEFINED.",
    }
}

/// Export the model as IFC4. `timestamp` is ISO 8601 (e.g. "2026-11-05T10:00:00").
pub fn export(m: &Model, file_name: &str, timestamp: &str) -> String {
    let mut w = Writer {
        data: String::new(),
        next: 1,
    };
    let gid = |seed: &str| {
        string(GlobalId::derived(format!("{}/{seed}", m.project_id).as_bytes()).as_str())
    };

    // Units: mm, m², m³, radian; for material values (ADR 0031): kg, Pa, kg/m³.
    let u_len = w.add("IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)");
    let u_area = w.add("IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.)");
    let u_vol = w.add("IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.)");
    let u_ang = w.add("IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.)");
    let u_mass = w.add("IFCSIUNIT(*,.MASSUNIT.,.KILO.,.GRAM.)");
    let u_pa = w.add("IFCSIUNIT(*,.PRESSUREUNIT.,$,.PASCAL.)");
    let u_m = w.add("IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.)");
    let de_kg = w.add(format!("IFCDERIVEDUNITELEMENT(#{u_mass},1)"));
    let de_m = w.add(format!("IFCDERIVEDUNITELEMENT(#{u_m},-3)"));
    let u_rho = w.add(format!(
        "IFCDERIVEDUNIT((#{de_kg},#{de_m}),.MASSDENSITYUNIT.,$)"
    ));
    let units = w.add(format!(
        "IFCUNITASSIGNMENT(({}))",
        refs(&[u_len, u_area, u_vol, u_ang, u_mass, u_pa, u_rho])
    ));

    let origin = w.add(point3([0.0; 3]));
    let world = w.add(format!("IFCAXIS2PLACEMENT3D(#{origin},$,$)"));
    let ctx = w.add(format!(
        "IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#{world},$)"
    ));
    let body = w.add(format!(
        "IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*,#{ctx},$,.MODEL_VIEW.,$)"
    ));

    // Project information (ADR 0030): the project's long name and phase in IfcProject's own
    // attributes, the building's name in IfcBuilding, everything in a property set.
    let opt = |k: &str| {
        m.project
            .get(k)
            .filter(|v| !v.trim().is_empty())
            .map_or_else(|| "$".to_string(), |v| string(v))
    };
    let project = w.add(format!(
        "IFCPROJECT({},$,{},$,$,{},{},(#{ctx}),#{units})",
        string(m.project_id.as_str()),
        string(&m.project_name),
        opt("project_title"),
        opt("phase")
    ));
    if !m.project.is_empty() {
        let props: Vec<usize> = m
            .project
            .iter()
            .map(|(k, v)| {
                w.add(format!(
                    "IFCPROPERTYSINGLEVALUE({},$,IFCTEXT({}),$)",
                    string(k),
                    string(v)
                ))
            })
            .collect();
        let pset = w.add(format!(
            "IFCPROPERTYSET({},$,'{PROJECT_PSET}',$,({}))",
            gid("project-information"),
            refs(&props)
        ));
        w.add(format!(
            "IFCRELDEFINESBYPROPERTIES({},$,$,$,(#{project}),#{pset})",
            gid("project-information/rel")
        ));
    }
    let site_pl = w.add(format!("IFCLOCALPLACEMENT($,#{world})"));
    let site = w.add(format!(
        "IFCSITE({},$,'Site',$,$,#{site_pl},$,$,.ELEMENT.,$,$,$,$,$)",
        gid("site")
    ));
    let bldg_pl = w.add(format!("IFCLOCALPLACEMENT(#{site_pl},#{world})"));
    let bldg = w.add(format!(
        "IFCBUILDING({},$,{},$,$,#{bldg_pl},$,$,.ELEMENT.,$,$,$)",
        gid("building"),
        m.project
            .get("building")
            .filter(|v| !v.trim().is_empty())
            .map_or_else(|| "'Building'".to_string(), |v| string(v))
    ));
    w.add(format!(
        "IFCRELAGGREGATES({},$,$,$,#{project},(#{site}))",
        gid("agg/project")
    ));
    w.add(format!(
        "IFCRELAGGREGATES({},$,$,$,#{site},(#{bldg}))",
        gid("agg/site")
    ));

    // Storeys, sorted by elevation for readability.
    let mut storeys: Vec<_> = m.storeys.values().collect();
    storeys.sort_by(|a, b| a.elevation.total_cmp(&b.elevation).then(a.id.cmp(&b.id)));
    let mut storey_ent = std::collections::BTreeMap::new();
    let mut storey_ids = Vec::new();
    for s in &storeys {
        let o = w.add(point3([0.0, 0.0, s.elevation]));
        let a = w.add(format!("IFCAXIS2PLACEMENT3D(#{o},$,$)"));
        let pl = w.add(format!("IFCLOCALPLACEMENT(#{bldg_pl},#{a})"));
        let id = w.add(format!(
            "IFCBUILDINGSTOREY({},$,{},$,$,#{pl},$,$,.ELEMENT.,{})",
            string(s.id.as_str()),
            string(&s.name),
            real(s.elevation)
        ));
        storey_ent.insert(s.id, (id, pl));
        storey_ids.push(id);
    }
    if !storey_ids.is_empty() {
        w.add(format!(
            "IFCRELAGGREGATES({},$,$,$,#{bldg},({}))",
            gid("agg/building"),
            refs(&storey_ids)
        ));
    }

    let mut contained: std::collections::BTreeMap<GlobalId, Vec<usize>> = Default::default();
    let mut by_material: std::collections::BTreeMap<GlobalId, Vec<usize>> = Default::default();
    let z = w.add(dir3([0.0, 0.0, 1.0]));

    for e in m.elements.values() {
        let (_, storey_pl) = storey_ent[&e.storey];
        let p = &e.placement;
        let o = w.add(point3(p.origin));
        let az = w.add(dir3(p.z_axis));
        let ax = w.add(dir3(p.x_axis));
        let a = w.add(format!("IFCAXIS2PLACEMENT3D(#{o},#{az},#{ax})"));
        let pl = w.add(format!("IFCLOCALPLACEMENT(#{storey_pl},#{a})"));

        // Geometry: an extrusion (SweptSolid) or an explicit mesh (FacetedBrep, ADR 0032).
        let (shape_repr, solid_id) = write_geometry(&mut w, &e.generator, world, z);
        let shape = w.add(format!(
            "IFCSHAPEREPRESENTATION(#{body},'Body','{shape_repr}',(#{solid_id}))"
        ));
        let pds = w.add(format!("IFCPRODUCTDEFINITIONSHAPE($,$,(#{shape}))"));

        // Another class is written as a proxy, its name in ObjectType: their attribute lists
        // differ, so writing IFCDOOR(…) with these attributes would be invalid (ADR 0042).
        let el = w.add(format!(
            "{}({},$,{},$,{},#{pl},#{pds},$,{})",
            e.class.name().to_uppercase(),
            string(e.id.as_str()),
            string(&e.name),
            e.ifc_entity
                .as_deref()
                .map_or_else(|| "$".to_owned(), string),
            predefined(e)
        ));
        contained.entry(e.storey).or_default().push(el);
        by_material.entry(e.material).or_default().push(el);

        // Generator pset: parameters as canonical JSON (data only, never executed).
        let params = serde_json::to_string(&e.generator).expect("generator serializes");
        let p1 = w.add(format!(
            "IFCPROPERTYSINGLEVALUE('Generator',$,IFCLABEL({}),$)",
            string(e.generator.name())
        ));
        let p2 = w.add(format!(
            "IFCPROPERTYSINGLEVALUE('Version',$,IFCINTEGER({}),$)",
            e.generator.version()
        ));
        let p3 = w.add(format!(
            "IFCPROPERTYSINGLEVALUE('Parameters',$,IFCTEXT({}),$)",
            string(&params)
        ));
        let pset = w.add(format!(
            "IFCPROPERTYSET({},$,'{GENERATOR_PSET}',$,({}))",
            gid(&format!("pset/{}", e.id)),
            refs(&[p1, p2, p3])
        ));
        w.add(format!(
            "IFCRELDEFINESBYPROPERTIES({},$,$,$,(#{el}),#{pset})",
            gid(&format!("rel-pset/{}", e.id))
        ));

        // Openings: IfcOpeningElement voids of this element, geometry relative to it.
        for (i, op) in e.openings.iter().enumerate() {
            let oo = w.add(point3(op.placement.origin));
            let oz = w.add(dir3(op.placement.z_axis));
            let ox = w.add(dir3(op.placement.x_axis));
            let oa = w.add(format!("IFCAXIS2PLACEMENT3D(#{oo},#{oz},#{ox})"));
            let opl = w.add(format!("IFCLOCALPLACEMENT(#{pl},#{oa})"));
            let (repr, solid) = write_geometry(&mut w, &op.generator, world, z);
            let shape = w.add(format!(
                "IFCSHAPEREPRESENTATION(#{body},'Body','{repr}',(#{solid}))"
            ));
            let opds = w.add(format!("IFCPRODUCTDEFINITIONSHAPE($,$,(#{shape}))"));
            // The opening's own GlobalId when it has one (stable when others are removed),
            // else derived from its index.
            let (ogid, rel) = match op.id {
                Some(id) => (string(id.as_str()), gid(&format!("voids/{id}"))),
                None => (
                    gid(&format!("opening/{}/{}", e.id, i)),
                    gid(&format!("voids/{}/{}", e.id, i)),
                ),
            };
            let opening = w.add(format!(
                "IFCOPENINGELEMENT({},$,$,$,$,#{opl},#{opds},$,.OPENING.)",
                ogid
            ));
            w.add(format!("IFCRELVOIDSELEMENT({rel},$,$,$,#{el},#{opening})"));
        }

        // Property sets beyond the material (ADR 0032), grouped by their "Pset/Property" prefix.
        let mut sets: std::collections::BTreeMap<&str, Vec<(String, &String)>> = Default::default();
        for (k, v) in &e.properties {
            let (pset, prop) = match k.split_once('/') {
                Some((p, r)) => (p, (r.to_owned(), v)),
                None => (PROPERTIES_PSET, (k.clone(), v)),
            };
            sets.entry(pset).or_default().push((prop.0, prop.1));
        }
        for (name, props) in sets {
            let values: Vec<usize> = props
                .iter()
                .map(|(prop, v)| {
                    w.add(format!(
                        "IFCPROPERTYSINGLEVALUE({},$,IFCLABEL({}),$)",
                        string(prop),
                        string(v)
                    ))
                })
                .collect();
            let pset = w.add(format!(
                "IFCPROPERTYSET({},$,{},$,({}))",
                gid(&format!("pset-{name}/{}", e.id)),
                string(name),
                refs(&values)
            ));
            w.add(format!(
                "IFCRELDEFINESBYPROPERTIES({},$,$,$,(#{el}),#{pset})",
                gid(&format!("rel-pset-{name}/{}", e.id))
            ));
        }
    }

    for (sid, els) in &contained {
        let (st, _) = storey_ent[sid];
        w.add(format!(
            "IFCRELCONTAINEDINSPATIALSTRUCTURE({},$,$,$,({}),#{st})",
            gid(&format!("contains/{sid}")),
            refs(els)
        ));
    }
    for (mid, els) in &by_material {
        let mat = &m.materials[mid];
        let me = w.add(format!(
            "IFCMATERIAL({},$,{})",
            string(&mat.name),
            string(mat.category.ifc_category())
        ));
        // Its values in the standard property sets (ADR 0031): density, modulus, strength.
        let p = &mat.props;
        let mut sets: Vec<(&str, Vec<usize>)> = Vec::new();
        if let Some(rho) = p.density_kg_m3 {
            let v = w.add(format!(
                "IFCPROPERTYSINGLEVALUE('MassDensity',$,IFCMASSDENSITYMEASURE({}),$)",
                real(rho)
            ));
            sets.push(("Pset_MaterialCommon", vec![v]));
        }
        if let Some(e) = p.e_mpa {
            let v = w.add(format!(
                "IFCPROPERTYSINGLEVALUE('YoungModulus',$,IFCMODULUSOFELASTICITYMEASURE({}),$)",
                real(e * 1e6)
            ));
            sets.push(("Pset_MaterialMechanical", vec![v]));
        }
        if let Some(f) = p.strength_mpa {
            let pa = real(f * 1e6);
            match mat.category {
                bimble_core::MaterialCategory::Concrete => {
                    let v = w.add(format!(
                        "IFCPROPERTYSINGLEVALUE('CompressiveStrength',$,IFCPRESSUREMEASURE({pa}),$)"
                    ));
                    sets.push(("Pset_MaterialConcrete", vec![v]));
                }
                bimble_core::MaterialCategory::Steel => {
                    let v = w.add(format!(
                        "IFCPROPERTYSINGLEVALUE('YieldStress',$,IFCPRESSUREMEASURE({pa}),$)"
                    ));
                    sets.push(("Pset_MaterialSteel", vec![v]));
                }
                _ => {}
            }
        }
        for (name, props) in sets {
            w.add(format!(
                "IFCMATERIALPROPERTIES('{name}',$,({}),#{me})",
                refs(&props)
            ));
        }
        w.add(format!(
            "IFCRELASSOCIATESMATERIAL({},$,$,$,({}),#{me})",
            gid(&format!("material/{mid}")),
            refs(els)
        ));
    }

    format!(
        "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n\
         FILE_NAME({},{},(''),(''),'bimble {}','bimble','');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n{}ENDSEC;\n\
         END-ISO-10303-21;\n",
        string(file_name),
        string(timestamp),
        env!("CARGO_PKG_VERSION"),
        w.data
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    /// A material's values go in the standard property sets, in SI units (ADR 0031).
    #[test]
    fn material_values_are_exported_in_standard_psets() {
        let m = concrete_frame(&FrameSpec::default());
        let ifc = export(&m, "frame.ifc", "2026-11-06T00:00:00");
        assert!(ifc.contains("IFCMATERIALPROPERTIES('Pset_MaterialCommon'"));
        assert!(ifc.contains("'MassDensity',$,IFCMASSDENSITYMEASURE(2500."));
        assert!(ifc.contains("'YoungModulus',$,IFCMODULUSOFELASTICITYMEASURE(33000000000."));
        assert!(ifc.contains("'CompressiveStrength',$,IFCPRESSUREMEASURE(30000000."));
        assert!(ifc.contains(".MASSDENSITYUNIT."));
    }

    /// Project information goes in IfcProject's long name and phase, IfcBuilding's name, and a
    /// property set (ADR 0030); without it, nothing changes.
    #[test]
    fn project_information_is_exported() {
        let mut m = concrete_frame(&FrameSpec::default());
        let plain = export(&m, "frame.ifc", "t");
        assert!(!plain.contains(PROJECT_PSET));
        assert!(plain.contains("'Building'"));
        m.project.insert("phase".into(), "Tender".into());
        m.project.insert("building".into(), "Hall C".into());
        let ifc = export(&m, "frame.ifc", "t");
        assert!(ifc.contains(",'Tender',(#"), "phase in IfcProject");
        assert!(ifc.contains("IFCBUILDING(") && ifc.contains(",'Hall C',"));
        assert!(ifc.contains(PROJECT_PSET));
    }

    #[test]
    fn reals() {
        assert_eq!(real(3000.0), "3000.0");
        assert_eq!(real(0.5), "0.5");
        assert_eq!(real(-800.0), "-800.0");
        assert_eq!(real(1e-5), "1.E-5");
        assert_eq!(real(1.5e20), "1.5E20");
    }

    #[test]
    fn strings() {
        assert_eq!(string("Niveau 1"), "'Niveau 1'");
        assert_eq!(string("l'étage"), "'l''\\X2\\00E9\\X0\\tage'");
        assert_eq!(string("a\\b"), "'a\\\\b'");
    }

    #[test]
    fn deterministic() {
        let m = concrete_frame(&FrameSpec::default());
        assert_eq!(export(&m, "a.ifc", "t"), export(&m, "a.ifc", "t"));
    }
}
