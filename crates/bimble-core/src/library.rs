//! The material library (ADR 0031): standard structural materials, with the values an engineer
//! uses — density, modulus of elasticity, characteristic strength — and the table they come from.
//!
//! Values are the characteristic or mean values of the EN tables (the same grades as SIA 262,
//! 263 and 265): concrete EN 1992-1-1 Table 3.1 (fck, Ecm), structural steel EN 1993-1-1
//! Table 3.1 (fy for t ≤ 40 mm, E = 210 GPa), reinforcement B500B (fyk, Es = 205 GPa as SIA 262),
//! timber EN 338 (C24) and glulam EN 14080 (fm,g,k, E0,g,mean), unit weights EN 1991-1-1 Annex A.
//! They are a starting point: check them against the standard your project uses.

use crate::{Material, MaterialCategory};
use serde::{Deserialize, Serialize};

/// What a structural engineer needs to know of a material. Every value is optional.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MaterialProps {
    /// kg/m³ (for masses: steel tonnage, self-weight).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub density_kg_m3: Option<f64>,
    /// Modulus of elasticity, MPa (mean value).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub e_mpa: Option<f64>,
    /// Characteristic strength, MPa: fck (concrete), fy (steel), fyk (rebar), fm,k (timber).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub strength_mpa: Option<f64>,
    /// Where the values come from, e.g. "EN 1992-1-1 Table 3.1".
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub source: String,
}

impl MaterialProps {
    pub fn is_empty(&self) -> bool {
        *self == MaterialProps::default()
    }

    pub fn validate(&self) -> Result<(), String> {
        for (what, v, max) in [
            ("density", self.density_kg_m3, 30_000.0),
            ("modulus", self.e_mpa, 1_000_000.0),
            ("strength", self.strength_mpa, 10_000.0),
        ] {
            if let Some(v) = v
                && !(v.is_finite() && v > 0.0 && v <= max)
            {
                return Err(format!("material {what}: a positive number up to {max}"));
            }
        }
        if self.source.chars().count() > 200 {
            return Err("material source: at most 200 characters".into());
        }
        Ok(())
    }
}

/// One entry of the library.
#[derive(Clone, Debug, PartialEq)]
pub struct LibraryMaterial {
    pub name: &'static str,
    pub category: MaterialCategory,
    pub props: MaterialProps,
    /// What the strength is, for its label: "fck", "fy", "fyk", "fm,k".
    pub strength_symbol: &'static str,
}

fn entry(
    name: &'static str,
    category: MaterialCategory,
    density: f64,
    e: f64,
    strength: f64,
    symbol: &'static str,
    source: &str,
) -> LibraryMaterial {
    LibraryMaterial {
        name,
        category,
        props: MaterialProps {
            density_kg_m3: Some(density),
            e_mpa: Some(e),
            strength_mpa: Some(strength),
            source: source.into(),
        },
        strength_symbol: symbol,
    }
}

/// The library: concrete, structural steel, reinforcement, timber, glulam.
pub fn library() -> Vec<LibraryMaterial> {
    use MaterialCategory::*;
    let c = "EN 1992-1-1 Table 3.1; 25 kN/m³ reinforced (EN 1991-1-1)";
    let s = "EN 1993-1-1 Table 3.1 (t ≤ 40 mm)";
    let mut v = vec![
        entry("C20/25", Concrete, 2500.0, 30_000.0, 20.0, "fck", c),
        entry("C25/30", Concrete, 2500.0, 31_000.0, 25.0, "fck", c),
        entry("C30/37", Concrete, 2500.0, 33_000.0, 30.0, "fck", c),
        entry("C35/45", Concrete, 2500.0, 34_000.0, 35.0, "fck", c),
        entry("C40/50", Concrete, 2500.0, 35_000.0, 40.0, "fck", c),
        entry("C45/55", Concrete, 2500.0, 36_000.0, 45.0, "fck", c),
        entry("C50/60", Concrete, 2500.0, 37_000.0, 50.0, "fck", c),
        entry("S235", Steel, 7850.0, 210_000.0, 235.0, "fy", s),
        entry("S275", Steel, 7850.0, 210_000.0, 275.0, "fy", s),
        entry("S355", Steel, 7850.0, 210_000.0, 355.0, "fy", s),
        entry("S460", Steel, 7850.0, 210_000.0, 460.0, "fy", s),
        entry(
            "B500B",
            Steel,
            7850.0,
            205_000.0,
            500.0,
            "fyk",
            "SIA 262 (Es = 205 GPa)",
        ),
        entry(
            "C24",
            Timber,
            420.0,
            11_000.0,
            24.0,
            "fm,k",
            "EN 338 (ρmean)",
        ),
    ];
    for (name, e, f, rho) in [
        ("GL24h", 11_500.0, 24.0, 420.0),
        ("GL28h", 12_600.0, 28.0, 460.0),
        ("GL32h", 14_200.0, 32.0, 490.0),
    ] {
        v.push(entry(
            name,
            Timber,
            rho,
            e,
            f,
            "fm,g,k",
            "EN 14080 (E0,g,mean, ρg,mean)",
        ));
    }
    v
}

/// The library entry a material name refers to: "C30/37", "S235(R)", "Steel S355 J2",
/// "GL24h 24x46" all find their grade. The longest grade name found wins.
pub fn lookup(name: &str) -> Option<LibraryMaterial> {
    let n = name.to_ascii_uppercase();
    let words: Vec<&str> = n
        .split(|c: char| !(c.is_ascii_alphanumeric() || c == '/'))
        .filter(|w| !w.is_empty())
        .collect();
    library()
        .into_iter()
        .filter(|m| {
            let g = m.name.to_ascii_uppercase();
            // A whole word, or the start of one ("S235J2", "C30/37XC4" stay found).
            words.iter().any(|w| *w == g || w.starts_with(&g))
        })
        .max_by_key(|m| m.name.len())
}

/// A model material made from a library entry (a new GlobalId).
pub fn to_material(m: &LibraryMaterial) -> Material {
    Material {
        id: crate::GlobalId::new(),
        name: m.name.to_string(),
        category: m.category,
        props: m.props.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_library_is_valid_and_named_once() {
        let v = library();
        let mut names: Vec<&str> = v.iter().map(|m| m.name).collect();
        for m in &v {
            m.props.validate().unwrap();
        }
        names.sort();
        names.dedup();
        assert_eq!(names.len(), v.len());
    }

    #[test]
    fn names_find_their_grade() {
        let g = |n: &str| lookup(n).map(|m| m.name);
        assert_eq!(g("C30/37"), Some("C30/37"));
        assert_eq!(g("S235(R)"), Some("S235"));
        assert_eq!(g("Steel S355 J2"), Some("S355"));
        assert_eq!(g("S355J2"), Some("S355"));
        assert_eq!(g("GL24h 24x46"), Some("GL24h"));
        assert_eq!(g("gl28h"), Some("GL28h"));
        assert_eq!(g("Béton"), None);
        // "C24" is timber; "C25/30" is concrete, not C24's prefix.
        assert_eq!(g("C25/30"), Some("C25/30"));
        assert_eq!(g("Timber C24"), Some("C24"));
    }

    #[test]
    fn bad_values_are_refused() {
        let p = MaterialProps {
            density_kg_m3: Some(-1.0),
            ..Default::default()
        };
        assert!(p.validate().is_err());
    }
}
