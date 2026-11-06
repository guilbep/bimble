//! IFC4 (ADD2 TC1) STEP export.
//!
//! Output is deterministic: entity ids follow model order, and relationship GlobalIds are derived
//! from the model's own ids. Exporting the same model twice gives byte-identical files (minus the
//! timestamp, which the caller controls), so exported IFC is diffable too.
//!
//! Each element gets explicit SweptSolid geometry (readable by any IFC tool) plus a
//! `Bimble_Generator` property set holding the generator name, version and parameters.

pub mod read;
pub mod step;

use bimble_core::{Element, Generator, GlobalId, Model, Profile};
use std::fmt::Write;

pub const GENERATOR_PSET: &str = "Bimble_Generator";
/// The project information's property set on IfcProject (ADR 0030).
pub const PROJECT_PSET: &str = "Bimble_ProjectInformation";

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

fn predefined(e: &Element) -> &'static str {
    match e.generator {
        Generator::RectColumn { .. } => ".COLUMN.",
        Generator::RectBeam { .. } => ".BEAM.",
        Generator::Slab { .. } => ".FLOOR.",
        Generator::Wall { .. } => ".SOLIDWALL.",
        Generator::PadFooting { .. } => ".PAD_FOOTING.",
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

        let profile = match e.generator.profile() {
            Profile::Rectangle { x_dim, y_dim } => w.add(format!(
                "IFCRECTANGLEPROFILEDEF(.AREA.,$,$,{},{})",
                real(x_dim),
                real(y_dim)
            )),
            Profile::Polygon(pts) => {
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
                let poly = w.add(format!("IFCPOLYLINE(({}))", refs(&ids)));
                w.add(format!("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#{poly})"))
            }
        };
        let solid = w.add(format!(
            "IFCEXTRUDEDAREASOLID(#{profile},#{world},#{z},{})",
            real(e.generator.depth())
        ));
        let shape = w.add(format!(
            "IFCSHAPEREPRESENTATION(#{body},'Body','SweptSolid',(#{solid}))"
        ));
        let pds = w.add(format!("IFCPRODUCTDEFINITIONSHAPE($,$,(#{shape}))"));

        let el = w.add(format!(
            "{}({},$,{},$,$,#{pl},#{pds},$,{})",
            e.class.name().to_uppercase(),
            string(e.id.as_str()),
            string(&e.name),
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
