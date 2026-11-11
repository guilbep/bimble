//! IFC2x3 export (TC1, Coordination View 2.0): Bimble's IFC4 output written down to the older
//! schema many structural tools still read. Bimble writes few entity kinds, so the differences
//! are a short list, checked against IfcOpenShell's schema validator:
//!
//! - every rooted object needs an owner history (optional in IFC4);
//! - a rectangle profile needs its position (the origin);
//! - columns, beams, walls and openings have no PredefinedType; a proxy's last attribute is its
//!   CompositionType; a material is its name only;
//! - no map conversion or projected CRS (the building's placement still carries the place and
//!   project north, ADR 0045), no IFC4 material property sets (density, strength: left out).

use std::fmt::Write;

/// Rooted entities Bimble writes: their second attribute is the owner history.
const ROOTED: &[&str] = &[
    "IFCPROJECT",
    "IFCSITE",
    "IFCBUILDING",
    "IFCBUILDINGSTOREY",
    "IFCCOLUMN",
    "IFCBEAM",
    "IFCSLAB",
    "IFCWALL",
    "IFCFOOTING",
    "IFCBUILDINGELEMENTPROXY",
    "IFCOPENINGELEMENT",
    "IFCPROPERTYSET",
    "IFCRELAGGREGATES",
    "IFCRELCONTAINEDINSPATIALSTRUCTURE",
    "IFCRELDEFINESBYPROPERTIES",
    "IFCRELVOIDSELEMENT",
    "IFCRELASSOCIATESMATERIAL",
];

/// IFC4 entities with no IFC2x3 counterpart in Bimble's output: left out.
const DROPPED: &[&str] = &[
    "IFCMATERIALPROPERTIES",
    "IFCPROJECTEDCRS",
    "IFCMAPCONVERSION",
];

/// Bimble's IFC4 STEP text as IFC2x3. `version`: Bimble's, for the owning application.
pub fn from_ifc4(ifc4: &str, version: &str) -> String {
    let mut out = String::with_capacity(ifc4.len() + 512);
    let mut max_id = 0u64;
    let mut data: Vec<(u64, String, Vec<String>)> = Vec::new();
    let mut in_data = false;
    for line in ifc4.lines() {
        if line == "DATA;" {
            in_data = true;
            continue;
        }
        if in_data && line == "ENDSEC;" {
            in_data = false;
            continue;
        }
        if in_data {
            if let Some((id, name, args)) = entity(line) {
                max_id = max_id.max(id);
                data.push((id, name, args));
            }
            continue;
        }
        if line == "END-ISO-10303-21;" {
            continue;
        }
        let line = line
            .replace("FILE_SCHEMA(('IFC4'));", "FILE_SCHEMA(('IFC2X3'));")
            .replace(
                "'ViewDefinition [DesignTransferView]'",
                "'ViewDefinition [CoordinationView_V2.0]'",
            );
        out.push_str(&line);
        out.push('\n');
    }
    // The owner history every rooted object points to: Bimble, no change tracking.
    let oh = max_id + 5;
    let history = [
        (max_id + 1, "IFCPERSON($,$,'',$,$,$,$,$)".to_string()),
        (max_id + 2, "IFCORGANIZATION($,'Bimble',$,$,$)".to_string()),
        (
            max_id + 3,
            format!(
                "IFCPERSONANDORGANIZATION(#{},#{},$)",
                max_id + 1,
                max_id + 2
            ),
        ),
        (
            max_id + 4,
            format!(
                "IFCAPPLICATION(#{},'{version}','Bimble','bimble')",
                max_id + 2
            ),
        ),
        (
            oh,
            format!(
                "IFCOWNERHISTORY(#{},#{},$,.NOCHANGE.,$,$,$,0)",
                max_id + 3,
                max_id + 4
            ),
        ),
    ];
    // The position a rectangle profile must have: the origin, shared.
    let (origin2, at2) = (max_id + 6, max_id + 7);
    out.push_str("DATA;\n");
    for (id, name, mut args) in data {
        if DROPPED.contains(&name.as_str()) {
            continue;
        }
        if ROOTED.contains(&name.as_str()) && args.len() > 1 && args[1] == "$" {
            args[1] = format!("#{oh}");
        }
        match name.as_str() {
            "IFCCOLUMN" | "IFCBEAM" | "IFCWALL" | "IFCOPENINGELEMENT" => {
                args.pop();
            }
            "IFCBUILDINGELEMENTPROXY" => {
                if let Some(last) = args.last_mut() {
                    *last = "$".into();
                }
            }
            "IFCMATERIAL" => args.truncate(1),
            "IFCRECTANGLEPROFILEDEF" if args.get(2).is_some_and(|a| a == "$") => {
                args[2] = format!("#{at2}");
            }
            _ => {}
        }
        let _ = writeln!(out, "#{id}={name}({});", args.join(","));
    }
    for (id, e) in &history {
        let _ = writeln!(out, "#{id}={e};");
    }
    let _ = writeln!(out, "#{origin2}=IFCCARTESIANPOINT((0.,0.));");
    let _ = writeln!(out, "#{at2}=IFCAXIS2PLACEMENT2D(#{origin2},$);");
    out.push_str("ENDSEC;\nEND-ISO-10303-21;\n");
    out
}

/// `#12=NAME(a,b,…);` → (12, NAME, top-level arguments as written).
fn entity(line: &str) -> Option<(u64, String, Vec<String>)> {
    let rest = line.strip_prefix('#')?;
    let (id, rest) = rest.split_once('=')?;
    let open = rest.find('(')?;
    let name = rest[..open].to_string();
    let body = rest[open + 1..].strip_suffix(");")?;
    let (mut args, mut cur, mut depth, mut quoted) = (Vec::new(), String::new(), 0i32, false);
    let mut chars = body.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\'' if quoted && chars.peek() == Some(&'\'') => {
                cur.push_str("''");
                chars.next();
                continue;
            }
            '\'' => quoted = !quoted,
            '(' if !quoted => depth += 1,
            ')' if !quoted => depth -= 1,
            ',' if !quoted && depth == 0 => {
                args.push(std::mem::take(&mut cur));
                continue;
            }
            _ => {}
        }
        cur.push(c);
    }
    args.push(cur);
    Some((id.parse().ok()?, name, args))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arguments_split_at_the_top_level_only() {
        let (id, name, args) =
            entity("#7=IFCWALL('a,b',$,'it''s',(#1,#2),IFCLABEL('x'),.SOLIDWALL.);").unwrap();
        assert_eq!((id, name.as_str()), (7, "IFCWALL"));
        assert_eq!(
            args,
            [
                "'a,b'",
                "$",
                "'it''s'",
                "(#1,#2)",
                "IFCLABEL('x')",
                ".SOLIDWALL."
            ]
        );
    }

    #[test]
    fn the_frame_written_down_to_ifc2x3() {
        let m = bimble_core::demo::concrete_frame(&Default::default());
        let t = from_ifc4(&crate::export(&m, "t.ifc", "2026-11-11T10:00:00"), "0.14.0");
        assert!(t.contains("FILE_SCHEMA(('IFC2X3'));"));
        assert!(t.contains("IFCOWNERHISTORY("));
        assert!(!t.contains("IFCMATERIALPROPERTIES("));
        assert!(!t.contains(",.COLUMN.)"), "no PredefinedType on columns");
        // Bimble reads it back, element by element.
        let (back, rep) = crate::import::import_str(&t).unwrap();
        assert_eq!(rep.elements, m.elements.len(), "{rep:?}");
        assert_eq!(
            back.elements.keys().collect::<Vec<_>>(),
            m.elements.keys().collect::<Vec<_>>()
        );
        // And says which schema it is.
        assert_eq!(crate::read::read_str(&t).unwrap().schema, "IFC2X3");
    }
}
