//! Views of a model (ADR 0021): 3D views, plans looking down or up, sections, each with what it
//! shows (view range, visibility) and the reference plans placed in it.
//!
//! In a `.bimble` model, views are part of the model (`Model::views`, changed by Commands,
//! versioned with the history). An IFC file isn't edited: its views live in a side file next to
//! it (`model.ifc.bimble-views.json`, `ViewsFile`).
//!
//! Storeys are referred to by a key: the storey's GlobalId when the model has one (`.bimble`,
//! IFC), else its name.

use crate::GlobalId;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct View {
    pub id: GlobalId,
    pub name: String,
    pub kind: ViewKind,
    /// What is hidden in this view.
    #[serde(default, skip_serializing_if = "Visibility::is_empty")]
    pub visibility: Visibility,
    /// Reference plans placed in this view (2D views only).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub references: Vec<Reference>,
    /// Dimensions and texts of this view (2D views only): they show wherever the view is placed
    /// (#33, #34).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub annotations: Vec<Annotation>,
}

/// Most annotations in one view.
pub const MAX_ANNOTATIONS: usize = 20_000;

/// A dimension or a text belonging to a view.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Annotation {
    /// The distance between two points, its line `offset_mm` (model mm, in the view's plane)
    /// to the left of a→b as seen in the view.
    Dimension {
        id: GlobalId,
        a: Anchor,
        b: Anchor,
        offset_mm: f64,
        /// Measured along a→b (aligned), or across the view (horizontal) or up it (vertical).
        #[serde(default, skip_serializing_if = "DimAxis::is_aligned")]
        axis: DimAxis,
        /// Its dimension type (graphic style, #43); none: the plan template's unit.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        kind: Option<String>,
    },
    /// A text at a point (world mm), its size on paper.
    Text {
        id: GlobalId,
        text: String,
        at: [f64; 3],
        size_mm: f64,
        #[serde(default, skip_serializing_if = "is_zero")]
        angle_deg: f64,
        /// Its text type (#43): colour, and size when set.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        kind: Option<String>,
    },
}

fn is_zero(x: &f64) -> bool {
    *x == 0.0
}

/// How a dimension measures, as seen in its view.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DimAxis {
    #[default]
    Aligned,
    Horizontal,
    Vertical,
}

impl DimAxis {
    fn is_aligned(&self) -> bool {
        *self == DimAxis::Aligned
    }
}

/// A point a dimension is attached to: world mm, and the element it was taken on (it then
/// follows that element when it moves; #34).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Anchor {
    pub at: [f64; 3],
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub element: Option<GlobalId>,
}

impl Annotation {
    pub fn id(&self) -> GlobalId {
        match self {
            Annotation::Dimension { id, .. } | Annotation::Text { id, .. } => *id,
        }
    }

    fn validate(&self) -> Result<(), String> {
        let finite = |p: &[f64; 3]| p.iter().all(|v| v.is_finite() && v.abs() < 1e9);
        match self {
            Annotation::Dimension {
                a, b, offset_mm, ..
            } => {
                if !(finite(&a.at) && finite(&b.at) && offset_mm.is_finite()) {
                    return Err("a dimension needs finite points and offset".into());
                }
            }
            Annotation::Text {
                text,
                at,
                size_mm,
                angle_deg,
                ..
            } => {
                if !finite(at) {
                    return Err("a text needs a finite position".into());
                }
                crate::sheet::check_text(text, *size_mm, *angle_deg)?;
            }
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum ViewKind {
    /// Free 3D view.
    ThreeD,
    /// A horizontal cut on a storey, looking down (floor plan) or up (ceiling plan, drawn as
    /// reflected in a mirror on the floor, so north stays up and east stays right).
    Plan {
        storey: String,
        look: Look,
        range: ViewRange,
    },
    /// A vertical cut through `origin` (world mm), perpendicular to `facing`, looking along it;
    /// shows what lies within `depth_mm` beyond the cut (0: everything).
    Section {
        origin: [f64; 2],
        facing: Facing,
        depth_mm: f64,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Look {
    Down,
    Up,
}

/// The direction a section looks in (world axes; +Y is north).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Facing {
    North,
    South,
    East,
    West,
}

impl Facing {
    pub const ALL: [Facing; 4] = [Facing::North, Facing::East, Facing::South, Facing::West];

    /// Unit vector (x, y) the section looks along.
    pub fn dir(self) -> [f64; 2] {
        match self {
            Facing::North => [0.0, 1.0],
            Facing::South => [0.0, -1.0],
            Facing::East => [1.0, 0.0],
            Facing::West => [-1.0, 0.0],
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Facing::North => "north",
            Facing::South => "south",
            Facing::East => "east",
            Facing::West => "west",
        }
    }
}

/// A plan's view range, as in Revit: the primary range from `bottom` to `top` with the cut
/// plane in it, and the view depth beyond it (below `bottom` looking down, above `top` looking
/// up). Each plane is a level and an offset.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ViewRange {
    pub top: RangePlane,
    pub cut: RangePlane,
    pub bottom: RangePlane,
    pub depth: RangePlane,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RangePlane {
    pub level: LevelRef,
    pub offset_mm: f64,
}

/// Which level a range plane is measured from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LevelRef {
    /// The view's storey.
    Associated,
    /// The next storey up (or `Unlimited` at the top storey).
    Above,
    /// The next storey down (or the view's storey at the lowest one).
    Below,
    /// No limit.
    Unlimited,
}

impl LevelRef {
    pub const ALL: [LevelRef; 4] = [
        LevelRef::Associated,
        LevelRef::Above,
        LevelRef::Below,
        LevelRef::Unlimited,
    ];

    pub fn label(self) -> &'static str {
        match self {
            LevelRef::Associated => "Associated level",
            LevelRef::Above => "Level above",
            LevelRef::Below => "Level below",
            LevelRef::Unlimited => "Unlimited",
        }
    }
}

fn plane(level: LevelRef, offset_mm: f64) -> RangePlane {
    RangePlane { level, offset_mm }
}

impl ViewRange {
    /// Revit's defaults: a floor plan cut 1.2 m above its level, seeing down to it; a ceiling
    /// plan cut 2.3 m above its level, seeing up to the level above.
    pub fn default_for(look: Look) -> ViewRange {
        match look {
            Look::Down => ViewRange {
                top: plane(LevelRef::Above, 0.0),
                cut: plane(LevelRef::Associated, 1200.0),
                bottom: plane(LevelRef::Associated, 0.0),
                depth: plane(LevelRef::Associated, 0.0),
            },
            Look::Up => ViewRange {
                top: plane(LevelRef::Above, 0.0),
                cut: plane(LevelRef::Associated, 2300.0),
                bottom: plane(LevelRef::Associated, 0.0),
                depth: plane(LevelRef::Above, 0.0),
            },
        }
    }
}

/// A view range in absolute levels (world mm); ±infinity where unlimited.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ResolvedRange {
    pub top: f64,
    pub cut: f64,
    pub bottom: f64,
    pub depth: f64,
}

impl ResolvedRange {
    /// What a plan shows, from low to high: from the view depth up to the cut plane looking
    /// down, from the cut plane up to the view depth looking up.
    pub fn visible(&self, look: Look) -> (f64, f64) {
        match look {
            Look::Down => (self.depth, self.cut),
            Look::Up => (self.cut, self.depth),
        }
    }
}

/// A storey for resolving view ranges: its key (see the module) and level, world mm.
#[derive(Clone, Debug, PartialEq)]
pub struct Level {
    pub key: String,
    pub name: String,
    pub elevation_mm: f64,
}

/// Resolve a range on the storey `storey` among `levels` (any order). Checks the planes are in
/// order: looking down depth ≤ bottom ≤ cut ≤ top, looking up bottom ≤ cut ≤ top ≤ depth.
pub fn resolve(
    range: &ViewRange,
    look: Look,
    storey: &str,
    levels: &[Level],
) -> Result<ResolvedRange, String> {
    let mut sorted: Vec<&Level> = levels.iter().collect();
    sorted.sort_by(|a, b| a.elevation_mm.total_cmp(&b.elevation_mm));
    let i = sorted
        .iter()
        .position(|l| l.key == storey)
        .ok_or_else(|| format!("unknown storey {storey}"))?;
    let here = sorted[i].elevation_mm;
    let z = |p: &RangePlane, unlimited: f64| -> f64 {
        let base = match p.level {
            LevelRef::Associated => Some(here),
            LevelRef::Above => sorted[i + 1..]
                .iter()
                .map(|l| l.elevation_mm)
                .find(|&e| e > here + 1.0),
            LevelRef::Below => Some(
                sorted[..i]
                    .iter()
                    .rev()
                    .map(|l| l.elevation_mm)
                    .find(|&e| e < here - 1.0)
                    .unwrap_or(here),
            ),
            LevelRef::Unlimited => None,
        };
        base.map_or(unlimited, |b| b + p.offset_mm)
    };
    let (inf, ninf) = (f64::INFINITY, f64::NEG_INFINITY);
    let r = ResolvedRange {
        top: z(&range.top, inf),
        cut: z(&range.cut, inf),
        bottom: z(&range.bottom, ninf),
        depth: match look {
            Look::Down => z(&range.depth, ninf),
            Look::Up => z(&range.depth, inf),
        },
    };
    for p in [&range.top, &range.cut, &range.bottom, &range.depth] {
        if !p.offset_mm.is_finite() || p.offset_mm.abs() > 1.0e6 {
            return Err("offsets must be numbers within ±1000 m".into());
        }
    }
    if range.cut.level == LevelRef::Unlimited {
        return Err("the cut plane needs a level".into());
    }
    let ok = match look {
        Look::Down => r.depth <= r.bottom && r.bottom <= r.cut && r.cut <= r.top,
        Look::Up => r.bottom <= r.cut && r.cut <= r.top && r.top <= r.depth,
    };
    if !ok {
        return Err(match look {
            Look::Down => {
                "looking down, the planes must be in order: view depth ≤ bottom ≤ cut ≤ top"
            }
            Look::Up => "looking up, the planes must be in order: bottom ≤ cut ≤ top ≤ view depth",
        }
        .into());
    }
    Ok(r)
}

/// What is hidden in a view: material families (by label), IFC classes, storeys (by name),
/// linked models (by file name).
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Visibility {
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub hidden_families: Vec<String>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub hidden_classes: Vec<String>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub hidden_storeys: Vec<String>,
    /// Linked models hidden in this view, by file name (#39).
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub hidden_links: Vec<String>,
}

impl Visibility {
    pub fn is_empty(&self) -> bool {
        self.hidden_families.is_empty()
            && self.hidden_classes.is_empty()
            && self.hidden_storeys.is_empty()
            && self.hidden_links.is_empty()
    }
}

/// A reference plan (another office's PDF or DXF) placed in a 2D view: "dead" geometry, drawn,
/// measured and snapped to, but never edited. Its lines are in mm, in its own plane; the
/// placement moves and turns them onto the view (world x, y for plans; along the cut for
/// sections: x across, y up).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Reference {
    pub id: GlobalId,
    /// The file it came from (name only).
    pub name: String,
    /// "pdf" or "dxf".
    pub format: String,
    pub offset_mm: [f64; 2],
    pub rotation_deg: f64,
    #[serde(default = "yes")]
    pub visible: bool,
    pub polylines: Vec<Vec<[f64; 2]>>,
}

fn yes() -> bool {
    true
}

/// Most points a reference may hold (untrusted files).
pub const MAX_REFERENCE_POINTS: usize = 2_000_000;

impl Reference {
    /// A point of the reference, placed (rotated about its own origin, then moved).
    pub fn place(&self, p: [f64; 2]) -> [f64; 2] {
        let (s, c) = self.rotation_deg.to_radians().sin_cos();
        [
            p[0] * c - p[1] * s + self.offset_mm[0],
            p[0] * s + p[1] * c + self.offset_mm[1],
        ]
    }

    pub fn validate(&self) -> Result<(), String> {
        let n: usize = self.polylines.iter().map(Vec::len).sum();
        if n > MAX_REFERENCE_POINTS {
            return Err(format!(
                "reference {}: {n} points, more than {MAX_REFERENCE_POINTS}",
                self.name
            ));
        }
        let finite = self
            .polylines
            .iter()
            .flatten()
            .chain([&self.offset_mm])
            .all(|p| p[0].is_finite() && p[1].is_finite())
            && self.rotation_deg.is_finite();
        if !finite {
            return Err(format!("reference {}: non-finite coordinates", self.name));
        }
        Ok(())
    }
}

impl View {
    pub fn validate(&self, levels: &[Level]) -> Result<(), String> {
        let ctx = |m: String| format!("view {} ({}): {m}", self.name, self.id);
        if self.name.trim().is_empty() {
            return Err(ctx("a view needs a name".into()));
        }
        match &self.kind {
            ViewKind::ThreeD => {
                if !self.references.is_empty() || !self.annotations.is_empty() {
                    return Err(ctx("reference plans and annotations go in 2D views".into()));
                }
            }
            ViewKind::Plan {
                storey,
                look,
                range,
            } => {
                resolve(range, *look, storey, levels).map_err(ctx)?;
            }
            ViewKind::Section {
                origin, depth_mm, ..
            } => {
                if !(origin.iter().all(|v| v.is_finite())
                    && depth_mm.is_finite()
                    && *depth_mm >= 0.0)
                {
                    return Err(ctx(
                        "section origin and depth must be numbers, depth ≥ 0".into()
                    ));
                }
            }
        }
        for r in &self.references {
            r.validate().map_err(ctx)?;
        }
        if self.annotations.len() > MAX_ANNOTATIONS {
            return Err(ctx("too many dimensions and texts".into()));
        }
        for a in &self.annotations {
            a.validate().map_err(ctx)?;
        }
        Ok(())
    }

    pub fn is_2d(&self) -> bool {
        !matches!(self.kind, ViewKind::ThreeD)
    }
}

/// The views every model has until changed: one 3D view, a plan looking down and one looking up
/// per storey. Their ids are derived from the storey keys, so a changed default view, once
/// stored, replaces the generated one (see `effective`).
pub fn default_views(levels: &[Level]) -> Vec<View> {
    let mut out = vec![View {
        id: GlobalId::derived(b"bimble view 3d"),
        name: "3D".into(),
        kind: ViewKind::ThreeD,
        visibility: Visibility::default(),
        references: Vec::new(),
        annotations: Vec::new(),
    }];
    let mut sorted: Vec<&Level> = levels.iter().collect();
    sorted.sort_by(|a, b| a.elevation_mm.total_cmp(&b.elevation_mm));
    for look in [Look::Down, Look::Up] {
        for (i, l) in sorted.iter().enumerate() {
            // A storey lower than the usual cut height (a foundation level 0.8 m under the
            // ground floor) gets its cut lower, so the default view is valid.
            let height = sorted[i + 1..]
                .iter()
                .map(|n| n.elevation_mm - l.elevation_mm)
                .find(|&h| h > 1.0);
            let mut range = ViewRange::default_for(look);
            if let Some(h) = height
                && range.cut.offset_mm >= h
            {
                range.cut.offset_mm = (h / 2.0).round();
            }
            let tag = match look {
                Look::Down => "down",
                Look::Up => "up",
            };
            out.push(View {
                id: GlobalId::derived(format!("bimble view plan {tag} {}", l.key).as_bytes()),
                name: match look {
                    Look::Down => format!("Plan {}", l.name),
                    Look::Up => format!("Ceiling {}", l.name),
                },
                kind: ViewKind::Plan {
                    storey: l.key.clone(),
                    look,
                    range,
                },
                visibility: Visibility::default(),
                references: Vec::new(),
                annotations: Vec::new(),
            });
        }
    }
    out
}

/// The views to show: the defaults, replaced by stored views with the same id, then the other
/// stored views (in id order).
pub fn effective(stored: &BTreeMap<GlobalId, View>, levels: &[Level]) -> Vec<View> {
    let defaults = default_views(levels);
    let mut out: Vec<View> = defaults
        .iter()
        .map(|d| stored.get(&d.id).cloned().unwrap_or_else(|| d.clone()))
        .collect();
    out.extend(
        stored
            .values()
            .filter(|v| !defaults.iter().any(|d| d.id == v.id))
            .cloned(),
    );
    out
}

/// The views of an IFC file, kept next to it (`<file>.bimble-views.json`).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ViewsFile {
    pub format: String,
    pub version: u32,
    pub views: BTreeMap<GlobalId, View>,
    /// Sheets (#33): version 2 files.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub sheets: BTreeMap<GlobalId, crate::sheet::Sheet>,
}

pub const VIEWS_FORMAT: &str = "bimble-views";
const MAX_VIEWS_FILE_BYTES: usize = 256 * 1024 * 1024;

/// The side file of a model file: `house.ifc` → `house.ifc.bimble-views.json`.
pub fn side_file(model: &std::path::Path) -> std::path::PathBuf {
    let mut p = model.as_os_str().to_owned();
    p.push(".bimble-views.json");
    std::path::PathBuf::from(p)
}

impl ViewsFile {
    /// Version 1 without sheets (read by Bimble 0.6), 2 with them.
    pub fn new(
        views: BTreeMap<GlobalId, View>,
        sheets: BTreeMap<GlobalId, crate::sheet::Sheet>,
    ) -> ViewsFile {
        ViewsFile {
            format: VIEWS_FORMAT.into(),
            version: if sheets.is_empty() { 1 } else { 2 },
            views,
            sheets,
        }
    }

    pub fn from_bytes(b: &[u8]) -> Result<ViewsFile, String> {
        if b.len() > MAX_VIEWS_FILE_BYTES {
            return Err("views file too large".into());
        }
        let f: ViewsFile = serde_json::from_slice(b).map_err(|e| e.to_string())?;
        if f.format != VIEWS_FORMAT || !(1..=2).contains(&f.version) {
            return Err(format!(
                "not a bimble views file (format {:?} version {})",
                f.format, f.version
            ));
        }
        for (k, v) in &f.views {
            if *k != v.id {
                return Err(format!("view {k}: key does not match id"));
            }
            for r in &v.references {
                r.validate()?;
            }
        }
        // Which views exist depends on the model's storeys (default views): checked when the
        // file is applied to its model.
        let mut numbers = std::collections::BTreeSet::new();
        for (k, sh) in &f.sheets {
            if *k != sh.id {
                return Err(format!("sheet {k}: key does not match id"));
            }
            sh.validate(|_| true)?;
            if !numbers.insert(sh.number.as_str()) {
                return Err(format!("two sheets are numbered {}", sh.number));
            }
        }
        Ok(crate::version::canonical(f))
    }

    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).expect("views serialize")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn levels() -> Vec<Level> {
        [
            ("a", "N00", 0.0),
            ("b", "N01", 3000.0),
            ("c", "N02", 6200.0),
        ]
        .map(|(k, n, e)| Level {
            key: k.into(),
            name: n.into(),
            elevation_mm: e,
        })
        .to_vec()
    }

    #[test]
    fn default_ranges_resolve_like_revit() {
        let l = levels();
        let down = resolve(&ViewRange::default_for(Look::Down), Look::Down, "b", &l).unwrap();
        assert_eq!(
            (down.top, down.cut, down.bottom, down.depth),
            (6200.0, 4200.0, 3000.0, 3000.0)
        );
        assert_eq!(down.visible(Look::Down), (3000.0, 4200.0));
        let up = resolve(&ViewRange::default_for(Look::Up), Look::Up, "b", &l).unwrap();
        assert_eq!(up.visible(Look::Up), (5300.0, 6200.0));
        // The top storey has no level above: unlimited.
        let top = resolve(&ViewRange::default_for(Look::Down), Look::Down, "c", &l).unwrap();
        assert_eq!(top.top, f64::INFINITY);
        // Level below at the lowest storey: the storey itself.
        let mut r = ViewRange::default_for(Look::Down);
        r.depth = plane(LevelRef::Below, -500.0);
        assert_eq!(resolve(&r, Look::Down, "a", &l).unwrap().depth, -500.0);
    }

    /// The usual cut heights where they fit (a 3 m storey keeps 1.2 m and 2.3 m), half the
    /// storey where they don't (a 0.8 m foundation level).
    #[test]
    fn default_cuts_are_lowered_only_where_they_dont_fit() {
        let mut lv = levels();
        lv.push(Level {
            key: "f".into(),
            name: "Foundations".into(),
            elevation_mm: -800.0,
        });
        let cut = |name: &str| {
            default_views(&lv)
                .into_iter()
                .find(|v| v.name == name)
                .and_then(|v| match v.kind {
                    ViewKind::Plan { range, .. } => Some(range.cut.offset_mm),
                    _ => None,
                })
                .unwrap()
        };
        assert_eq!(cut("Plan N00"), 1200.0);
        assert_eq!(cut("Ceiling N00"), 2300.0);
        assert_eq!(cut("Plan Foundations"), 400.0);
        assert_eq!(cut("Ceiling Foundations"), 400.0);
        for v in default_views(&lv) {
            v.validate(&lv).unwrap();
        }
    }

    #[test]
    fn planes_out_of_order_are_refused() {
        let l = levels();
        let mut r = ViewRange::default_for(Look::Down);
        r.cut.offset_mm = 9000.0; // above the level above
        assert!(
            resolve(&r, Look::Down, "b", &l)
                .unwrap_err()
                .contains("order")
        );
        let mut r = ViewRange::default_for(Look::Down);
        r.depth = plane(LevelRef::Associated, 100.0); // depth above bottom
        assert!(resolve(&r, Look::Down, "b", &l).is_err());
        assert!(resolve(&ViewRange::default_for(Look::Down), Look::Down, "zz", &l).is_err());
        let mut r = ViewRange::default_for(Look::Down);
        r.cut.offset_mm = f64::NAN;
        assert!(resolve(&r, Look::Down, "b", &l).is_err());
    }

    #[test]
    fn defaults_have_stable_ids_and_stored_views_replace_them() {
        let l = levels();
        let d = default_views(&l);
        assert_eq!(d.len(), 1 + 2 * 3);
        assert_eq!(d, default_views(&l));
        assert_eq!(d[1].name, "Plan N00");
        // Every default view is valid, also on low storeys.
        let low = [("f", "Foundations", -800.0), ("g", "Ground", 0.0)]
            .map(|(k, n, e)| Level {
                key: k.into(),
                name: n.into(),
                elevation_mm: e,
            })
            .to_vec();
        for v in default_views(&low).iter().chain(&d) {
            v.validate(if v.name.contains("Found") || v.name.contains("Ground") {
                &low
            } else {
                &l
            })
            .unwrap();
        }
        let mut changed = d[2].clone();
        changed.name = "Plan N01 (formwork)".into();
        let extra = View {
            id: GlobalId::new(),
            name: "Section A".into(),
            kind: ViewKind::Section {
                origin: [0.0, 1000.0],
                facing: Facing::North,
                depth_mm: 0.0,
            },
            visibility: Visibility::default(),
            references: Vec::new(),
            annotations: Vec::new(),
        };
        let stored: BTreeMap<_, _> =
            [(changed.id, changed.clone()), (extra.id, extra.clone())].into();
        let e = effective(&stored, &l);
        assert_eq!(e.len(), d.len() + 1);
        assert_eq!(e[2], changed);
        assert_eq!(e.last(), Some(&extra));
    }

    #[test]
    fn references_are_placed_and_checked() {
        let r = Reference {
            id: GlobalId::new(),
            name: "archi.dxf".into(),
            format: "dxf".into(),
            offset_mm: [1000.0, 0.0],
            rotation_deg: 90.0,
            visible: true,
            polylines: vec![vec![[0.0, 0.0], [100.0, 0.0]]],
        };
        let p = r.place([100.0, 0.0]);
        assert!((p[0] - 1000.0).abs() < 1e-9 && (p[1] - 100.0).abs() < 1e-9);
        assert!(r.validate().is_ok());
        let mut bad = r.clone();
        bad.polylines[0][0][0] = f64::INFINITY;
        assert!(bad.validate().is_err());
    }

    #[test]
    fn side_file_round_trips_and_rejects_others() {
        let l = levels();
        let v = default_views(&l).remove(1);
        let f = ViewsFile::new([(v.id, v)].into(), Default::default());
        assert_eq!(ViewsFile::from_bytes(f.to_json().as_bytes()).unwrap(), f);
        assert!(ViewsFile::from_bytes(br#"{"format":"x","version":1,"views":{}}"#).is_err());
        assert!(ViewsFile::from_bytes(b"not json").is_err());
        assert_eq!(
            side_file(std::path::Path::new("/a/b.ifc")),
            std::path::PathBuf::from("/a/b.ifc.bimble-views.json")
        );
    }

    /// Views in a .bimble model: added, changed and removed by Commands (undoable), saved,
    /// versioned; a model without views serializes exactly as before.
    #[test]
    fn views_in_a_model() {
        use crate::demo::{FrameSpec, concrete_frame};
        use crate::{Command, CommandError, History};
        let mut m = concrete_frame(&FrameSpec::default());
        let before = crate::file::to_string(&m);
        assert!(!before.contains("\"views\""));
        let hash0 = crate::version::model_hash(&m);
        let levels = m.levels();
        let mut plan = default_views(&levels)
            .into_iter()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        let mut h = History::default();
        h.apply(&mut m, vec![Command::AddView(plan.clone())])
            .unwrap();
        assert_ne!(crate::version::model_hash(&m), hash0);
        // A bad range is refused before anything changes.
        if let ViewKind::Plan { range, .. } = &mut plan.kind {
            range.cut.offset_mm = -5000.0;
        }
        assert!(matches!(
            h.apply(&mut m, vec![Command::SetView(plan.clone())]),
            Err(CommandError::Invalid(_))
        ));
        if let ViewKind::Plan { range, .. } = &mut plan.kind {
            range.cut.offset_mm = 900.0;
        }
        h.apply(&mut m, vec![Command::SetView(plan.clone())])
            .unwrap();
        assert_eq!(m.views[&plan.id], plan);
        // The plan's storey can't be removed while the view uses it.
        let storey = match &plan.kind {
            ViewKind::Plan { storey, .. } => storey.parse().unwrap(),
            _ => unreachable!(),
        };
        assert!(matches!(
            Command::RemoveStorey { id: storey }.apply(&mut m.clone()),
            Err(CommandError::InUse(_))
        ));
        // Saved and read back; the diff names the view.
        let text = crate::file::to_string(&m);
        assert_eq!(crate::file::from_str(&text).unwrap(), m);
        let d = crate::version::diff(&concrete_frame(&FrameSpec::default()), &m);
        assert!(
            d.iter().any(|c| c.to_string().contains(&plan.name)),
            "{d:?}"
        );
        // Undo twice: the model is back, byte for byte.
        assert!(h.undo(&mut m) && h.undo(&mut m));
        assert_eq!(crate::file::to_string(&m), before);
        assert_eq!(crate::version::model_hash(&m), hash0);
    }
}
