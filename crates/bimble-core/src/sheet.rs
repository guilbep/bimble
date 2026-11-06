//! Sheets (issue #33): pages of paper with views placed on them at their own scales, one title
//! block, and texts of their own.
//!
//! Like views (ADR 0021), sheets are part of a `.bimble` model (changed by Commands, versioned)
//! and live in the side file of an IFC model. A view can be placed on several sheets; what the
//! view holds (its range, visibility, references, dimensions, texts) shows wherever it is placed.

use crate::GlobalId;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// Paper sizes a sheet can have (ISO A series).
pub const PAPERS: [&str; 5] = ["A4", "A3", "A2", "A1", "A0"];
/// Most views on one sheet, and most texts on one sheet.
pub const MAX_PLACED: usize = 200;
pub const MAX_TEXTS: usize = 2_000;
/// Longest text, in characters.
pub const MAX_TEXT_CHARS: usize = 2_000;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Sheet {
    pub id: GlobalId,
    /// Drawing number, e.g. "S-101". Unique among the model's sheets.
    pub number: String,
    pub name: String,
    /// "A4" … "A0".
    pub paper: String,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub portrait: bool,
    /// Title block values for this sheet ({phase}, {revision}, …), over the template's fields.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub fields: BTreeMap<String, String>,
    /// Views placed on the sheet, drawn in this order.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub placed: Vec<Placed>,
    /// Texts on the sheet itself (notes, titles), not belonging to a view.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub texts: Vec<SheetText>,
}

/// A view placed on a sheet.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Placed {
    pub id: GlobalId,
    pub view: GlobalId,
    /// 1:N on this sheet.
    pub scale: u32,
    /// Centre of the view's drawing on the paper, mm from the sheet's top-left corner.
    pub at_mm: [f64; 2],
}

/// A text on a sheet, mm from the top-left corner.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SheetText {
    pub id: GlobalId,
    pub text: String,
    pub at_mm: [f64; 2],
    pub size_mm: f64,
    #[serde(default, skip_serializing_if = "is_zero")]
    pub angle_deg: f64,
    /// Its text type (#43).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kind: Option<String>,
}

fn is_zero(x: &f64) -> bool {
    *x == 0.0
}

/// Checks a text's size, angle and length (shared with view texts).
pub fn check_text(text: &str, size_mm: f64, angle_deg: f64) -> Result<(), String> {
    if text.chars().count() > MAX_TEXT_CHARS {
        return Err(format!("text longer than {MAX_TEXT_CHARS} characters"));
    }
    if !(size_mm.is_finite() && (0.5..=50.0).contains(&size_mm)) {
        return Err("text size must be 0.5 to 50 mm".into());
    }
    if !(angle_deg.is_finite() && (-360.0..=360.0).contains(&angle_deg)) {
        return Err("text angle must be within ±360°".into());
    }
    Ok(())
}

impl Sheet {
    /// A new, empty sheet (A1 landscape).
    pub fn new(number: impl Into<String>, name: impl Into<String>) -> Sheet {
        Sheet {
            id: GlobalId::new(),
            number: number.into(),
            name: name.into(),
            paper: "A1".into(),
            portrait: false,
            fields: BTreeMap::new(),
            placed: Vec::new(),
            texts: Vec::new(),
        }
    }

    /// Paper width and height, mm, in the sheet's orientation.
    pub fn size_mm(&self) -> Option<(f64, f64)> {
        let (w, h) = match self.paper.as_str() {
            "A4" => (297.0, 210.0),
            "A3" => (420.0, 297.0),
            "A2" => (594.0, 420.0),
            "A1" => (841.0, 594.0),
            "A0" => (1189.0, 841.0),
            _ => return None,
        };
        Some(if self.portrait { (h, w) } else { (w, h) })
    }

    /// Checks the sheet on its own; `view_exists` says whether a placed view is in the model.
    pub fn validate(&self, view_exists: impl Fn(GlobalId) -> bool) -> Result<(), String> {
        let ctx = |m: String| format!("sheet {} {}: {m}", self.number, self.name);
        if self.number.trim().is_empty() {
            return Err(ctx("needs a number".into()));
        }
        if self.number.chars().count() > 40 || self.name.chars().count() > 200 {
            return Err(ctx("number or name too long".into()));
        }
        let (w, h) = self.size_mm().ok_or_else(|| {
            ctx(format!(
                "paper {:?}: one of {}",
                self.paper,
                PAPERS.join(", ")
            ))
        })?;
        if self.placed.len() > MAX_PLACED || self.texts.len() > MAX_TEXTS {
            return Err(ctx("too many views or texts".into()));
        }
        let on_paper = |p: [f64; 2]| {
            p.iter().all(|v| v.is_finite())
                && (0.0..=w).contains(&p[0])
                && (0.0..=h).contains(&p[1])
        };
        for p in &self.placed {
            if !(1..=5000).contains(&p.scale) {
                return Err(ctx(format!("scale 1:{}: must be 1:1 to 1:5000", p.scale)));
            }
            if !on_paper(p.at_mm) {
                return Err(ctx("a view is placed off the paper".into()));
            }
            if !view_exists(p.view) {
                return Err(ctx(format!("no view {}", p.view)));
            }
        }
        for t in &self.texts {
            if !on_paper(t.at_mm) {
                return Err(ctx("a text is off the paper".into()));
            }
            check_text(&t.text, t.size_mm, t.angle_deg).map_err(ctx)?;
        }
        for (k, v) in &self.fields {
            if k.chars().count() > 100 || v.chars().count() > MAX_TEXT_CHARS {
                return Err(ctx(format!("field {k}: too long")));
            }
        }
        Ok(())
    }
}

/// The next free number after the sheets' highest one with the same prefix ("S-101" → "S-102");
/// "S-101" when there are none.
pub fn next_number<'a>(numbers: impl Iterator<Item = &'a str>) -> String {
    let mut best: Option<(String, u64, usize)> = None;
    let all: Vec<&str> = numbers.collect();
    for n in &all {
        let digits = n.chars().rev().take_while(|c| c.is_ascii_digit()).count();
        if digits == 0 || digits > 9 {
            continue;
        }
        let (prefix, num) = n.split_at(n.len() - digits);
        let v: u64 = num.parse().unwrap_or(0);
        if best.as_ref().is_none_or(|b| v > b.1) {
            best = Some((prefix.to_string(), v, digits));
        }
    }
    let Some((prefix, mut v, width)) = best else {
        return "S-101".into();
    };
    loop {
        v += 1;
        let n = format!("{prefix}{v:0width$}");
        if !all.contains(&n.as_str()) {
            return n;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_follow_on() {
        assert_eq!(next_number([].into_iter()), "S-101");
        assert_eq!(next_number(["S-101", "S-103"].into_iter()), "S-104");
        assert_eq!(next_number(["P-009"].into_iter()), "P-010");
        assert_eq!(next_number(["cover"].into_iter()), "S-101");
    }

    #[test]
    fn bad_sheets_are_refused() {
        let v = GlobalId::new();
        let mut s = Sheet::new("S-101", "Plans");
        s.placed.push(Placed {
            id: GlobalId::new(),
            view: v,
            scale: 100,
            at_mm: [400.0, 300.0],
        });
        s.validate(|id| id == v).unwrap();
        assert!(s.validate(|_| false).unwrap_err().contains("no view"));
        let mut off = s.clone();
        off.placed[0].at_mm = [900.0, 10.0];
        assert!(
            off.validate(|_| true)
                .unwrap_err()
                .contains("off the paper")
        );
        // Portrait A1 is 594 wide: the same point is off it.
        let mut p = s.clone();
        p.portrait = true;
        p.placed[0].at_mm = [700.0, 300.0];
        assert!(p.validate(|_| true).is_err());
        let mut bad = s.clone();
        bad.paper = "B2".into();
        assert!(bad.validate(|_| true).unwrap_err().contains("paper"));
        let mut nan = s.clone();
        nan.texts.push(SheetText {
            id: GlobalId::new(),
            text: "x".into(),
            at_mm: [f64::NAN, 1.0],
            size_mm: 3.0,
            angle_deg: 0.0,
            kind: None,
        });
        assert!(nan.validate(|_| true).is_err());
        let mut scale = s;
        scale.placed[0].scale = 0;
        assert!(scale.validate(|_| true).unwrap_err().contains("scale"));
    }

    /// Sheets go through Commands: numbers are unique, placed views must exist, a placed view
    /// can't be removed, undo works, and the model's file and history keep them.
    #[test]
    fn sheets_in_a_model() {
        use crate::Command;
        use crate::demo::{FrameSpec, concrete_frame};
        use crate::view::default_views;
        let start = concrete_frame(&FrameSpec::default());
        let mut m = start.clone();
        let before = crate::version::model_hash(&m);
        let plan = default_views(&m.levels())
            .into_iter()
            .find(|v| v.name.starts_with("Plan"))
            .unwrap();
        let mut s = Sheet::new("S-101", "Plans");
        s.placed.push(Placed {
            id: GlobalId::new(),
            view: plan.id,
            scale: 100,
            at_mm: [400.0, 300.0],
        });
        let mut h = crate::History::default();
        h.apply(&mut m, vec![Command::AddSheet(s.clone())]).unwrap();
        assert_ne!(crate::version::model_hash(&m), before);
        // Same number again: refused.
        let mut dup = Sheet::new("S-101", "Other");
        dup.paper = "A3".into();
        assert!(Command::AddSheet(dup).apply(&mut m.clone()).is_err());
        // A view that isn't in the model: refused.
        let mut ghost = Sheet::new("S-102", "Ghost");
        ghost.placed.push(Placed {
            id: GlobalId::new(),
            view: GlobalId::new(),
            scale: 50,
            at_mm: [10.0, 10.0],
        });
        assert!(Command::AddSheet(ghost).apply(&mut m.clone()).is_err());
        // A stored view placed on a sheet can't be removed.
        let mut custom = plan.clone();
        custom.id = GlobalId::new();
        custom.name = "Plan custom".into();
        Command::AddView(custom.clone()).apply(&mut m).unwrap();
        let mut s2 = s.clone();
        s2.placed[0].view = custom.id;
        Command::SetSheet(s2).apply(&mut m).unwrap();
        assert!(
            Command::RemoveView { id: custom.id }
                .apply(&mut m.clone())
                .is_err()
        );
        // File round trip.
        let back = crate::file::from_str(&crate::file::to_string(&m)).unwrap();
        assert_eq!(back.sheets, m.sheets);
        // Undo of the first step leaves the model as it was.
        let mut m2 = start.clone();
        let mut h2 = crate::History::default();
        let base = m2.clone();
        h2.apply(&mut m2, vec![Command::AddSheet(s)]).unwrap();
        assert!(h2.undo(&mut m2));
        assert_eq!(m2, base);
        let _ = h;
    }

    /// The side file of an IFC keeps sheets (version 2) and still reads version 1.
    #[test]
    fn side_files_keep_sheets() {
        use crate::view::ViewsFile;
        let s = Sheet::new("S-101", "Plans");
        let f = ViewsFile::new(Default::default(), [(s.id, s.clone())].into());
        assert_eq!(f.version, 2);
        let back = ViewsFile::from_bytes(f.to_json().as_bytes()).unwrap();
        assert_eq!(back.sheets[&s.id], s);
        let v1 = ViewsFile::new(Default::default(), Default::default());
        assert_eq!(v1.version, 1);
        assert!(!v1.to_json().contains("sheets"));
        let mut dup = f.clone();
        let mut other = s.clone();
        other.id = GlobalId::new();
        dup.sheets.insert(other.id, other);
        assert!(ViewsFile::from_bytes(dup.to_json().as_bytes()).is_err());
    }
}
