//! Schedules drawn as tables (ADR 0029): each listed element measured from its geometry.
//!
//! An element's direction in plan is that of its longest level edge; its length, width and
//! height are its extents along that direction, across it and in z. A column (standing: taller
//! than wide) has its height as length and its plan extents as section. Volumes come from the
//! closed mesh. Locations are the grid crossing nearest the element's centre in plan.

use crate::plan::{clusters, cm, letter_tag, section_text};
use bimble_core::MaterialCategory;
use bimble_core::schedule::{Field, ScheduleDef};
use bimble_geom::{Scene, SceneElement};

/// One cell of a schedule.
#[derive(Clone, Debug, PartialEq)]
pub enum Cell {
    Empty,
    Text(String),
    /// A number and how many decimals it is written with.
    Num(f64, usize),
}

impl Cell {
    pub fn text(&self) -> String {
        match self {
            Cell::Empty => String::new(),
            Cell::Text(t) => t.clone(),
            Cell::Num(v, d) => format!("{v:.d$}", d = *d),
        }
    }

    /// For sorting: numbers by value, text in natural order ("B-10" after "B-9").
    pub fn sort_key(&self) -> (u8, f64, Vec<(String, u64)>) {
        match self {
            Cell::Empty => (2, 0.0, Vec::new()),
            Cell::Num(v, _) => (0, *v, Vec::new()),
            Cell::Text(t) => (1, 0.0, natural(t)),
        }
    }
}

/// Text split into runs of non-digits and numbers, so "P10" sorts after "P9".
fn natural(t: &str) -> Vec<(String, u64)> {
    let mut out = Vec::new();
    let mut text = String::new();
    let mut num = String::new();
    for ch in t.chars() {
        if ch.is_ascii_digit() {
            num.push(ch);
        } else {
            if !num.is_empty() {
                out.push((std::mem::take(&mut text), num.parse().unwrap_or(u64::MAX)));
                num.clear();
            }
            text.extend(ch.to_lowercase());
        }
    }
    out.push((text, num.parse().unwrap_or(0)));
    out
}

/// A row: the scene element it is about, and its cells (one per field).
#[derive(Clone, Debug, PartialEq)]
pub struct Row {
    pub element: usize,
    pub cells: Vec<Cell>,
}

/// The rows of one level, and their subtotal.
#[derive(Clone, Debug, PartialEq)]
pub struct Group {
    /// The level's name; empty when the schedule isn't grouped.
    pub level: String,
    pub rows: Vec<Row>,
    pub subtotal: Vec<Cell>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Table {
    pub name: String,
    pub fields: Vec<Field>,
    pub groups: Vec<Group>,
    pub total: Option<Vec<Cell>>,
    pub count: usize,
}

/// An element's measures, mm (and m³ for the volume).
#[derive(Clone, Debug, PartialEq)]
pub struct Measures {
    pub length: f64,
    pub width: f64,
    pub height: f64,
    pub area_m2: f64,
    pub volume_m3: f64,
    /// The plan centre, world mm.
    pub centre: [f64; 2],
    pub section: String,
}

/// Measure one element from its mesh.
pub fn measure(scene: &Scene, e: &SceneElement) -> Option<Measures> {
    let m = e.world_mesh();
    let o = scene.origin_mm;
    if m.positions.is_empty() {
        return None;
    }
    // Direction in plan: the longest level edge.
    let mut dir = [1.0, 0.0];
    let mut best = 0.0;
    for s in &m.edges {
        let (p, q) = (s[0].map(f64::from), s[1].map(f64::from));
        let (dx, dy, dz) = (q[0] - p[0], q[1] - p[1], q[2] - p[2]);
        let l = dx.hypot(dy);
        if l > best && dz.abs() < 1.0 {
            best = l;
            dir = [dx / l, dy / l];
        }
    }
    let n = [-dir[1], dir[0]];
    let (mut s0, mut s1, mut t0, mut t1, mut z0, mut z1) = (
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
    );
    let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
    for p in &m.positions {
        let p = p.map(f64::from);
        let (s, t) = (p[0] * dir[0] + p[1] * dir[1], p[0] * n[0] + p[1] * n[1]);
        (s0, s1, t0, t1) = (s0.min(s), s1.max(s), t0.min(t), t1.max(t));
        (z0, z1) = (z0.min(p[2]), z1.max(p[2]));
        for k in 0..2 {
            lo[k] = lo[k].min(p[k]);
            hi[k] = hi[k].max(p[k]);
        }
    }
    let (along, across, height) = (s1 - s0, t1 - t0, z1 - z0);
    let volume_m3 = m.volume().abs() / 1e9;
    let centre = [(lo[0] + hi[0]) / 2.0 + o[0], (lo[1] + hi[1]) / 2.0 + o[1]];
    let class = e.class.to_ascii_lowercase();
    let standing = matches!(class.as_str(), "ifccolumn" | "ifcpile" | "ifcmember")
        && height > along.max(across);
    let wall = class.starts_with("ifcwall");
    // Plan sizes along x then y, for a standing element's section.
    let (sx, sy) = if dir[0].abs() >= dir[1].abs() {
        (along, across)
    } else {
        (across, along)
    };
    let round = std::f64::consts::PI * (sx / 2.0) * (sx / 2.0) * height / 1e9;
    let circular = standing
        && m.positions.len() >= 24
        && (sx - sy).abs() <= 0.02 * sx
        && (round - volume_m3).abs() <= 0.03 * round;
    let section = if circular {
        format!("Ø{}", cm(sx))
    } else if standing {
        section_text(e.category, &e.material, sx, sy)
    } else if class == "ifcbeam" || class == "ifcfooting" {
        section_text(e.category, &e.material, across, height)
    } else {
        String::new()
    };
    let (length, width) = if standing {
        (height, sx.min(sy))
    } else {
        (along, across)
    };
    let area_m2 = if wall {
        along * height / 1e6
    } else if standing {
        volume_m3 * 1e9 / height.max(1e-9) / 1e6
    } else if height > 1e-6 {
        volume_m3 * 1e9 / height / 1e6
    } else {
        0.0
    };
    Some(Measures {
        length,
        width,
        height,
        area_m2,
        volume_m3,
        centre,
        section,
    })
}

/// Grid crossings with their names ("B-2"): the model's grid axes, else axes through the
/// columns (A, B… across x; 1, 2… along y, as on plans).
fn crossings(scene: &Scene) -> Vec<([f64; 2], String)> {
    let mut axes: Vec<(String, [f64; 2], [f64; 2])> = scene
        .grids
        .iter()
        .map(|g| (g.tag.clone(), g.start_mm, g.end_mm))
        .collect();
    if axes.is_empty() {
        let o = scene.origin_mm;
        let centres: Vec<[f64; 2]> = scene
            .elements
            .iter()
            .filter(|e| e.class.eq_ignore_ascii_case("IfcColumn"))
            .filter_map(|e| {
                let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
                for p in &e.world_mesh().positions {
                    for k in 0..2 {
                        lo[k] = lo[k].min(p[k] as f64 + o[k]);
                        hi[k] = hi[k].max(p[k] as f64 + o[k]);
                    }
                }
                lo[0]
                    .is_finite()
                    .then(|| [(lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0])
            })
            .collect();
        for (i, x) in clusters(centres.iter().map(|c| c[0]).collect(), 50.0)
            .into_iter()
            .enumerate()
        {
            axes.push((letter_tag(i), [x, 0.0], [x, 1.0]));
        }
        for (i, y) in clusters(centres.iter().map(|c| c[1]).collect(), 50.0)
            .into_iter()
            .enumerate()
        {
            axes.push(((i + 1).to_string(), [0.0, y], [1.0, y]));
        }
    }
    let mut out = Vec::new();
    for (i, a) in axes.iter().enumerate() {
        for b in &axes[i + 1..] {
            let d1 = [a.2[0] - a.1[0], a.2[1] - a.1[1]];
            let d2 = [b.2[0] - b.1[0], b.2[1] - b.1[1]];
            let den = d1[0] * d2[1] - d1[1] * d2[0];
            if den.abs() <= 1e-9 * d1[0].hypot(d1[1]) * d2[0].hypot(d2[1]) {
                continue;
            }
            let w = [b.1[0] - a.1[0], b.1[1] - a.1[1]];
            let t = (w[0] * d2[1] - w[1] * d2[0]) / den;
            let p = [a.1[0] + d1[0] * t, a.1[1] + d1[1] * t];
            // Letters first: "B-2", whichever axis is which.
            let (x, y) = if a.0.chars().next().is_some_and(|c| c.is_ascii_digit()) {
                (&b.0, &a.0)
            } else {
                (&a.0, &b.0)
            };
            out.push((p, format!("{x}-{y}")));
        }
    }
    out
}

/// A cell of a field for an element.
/// A material's density (kg/m³): the model's own value, else its library grade's (ADR 0031).
pub fn density(scene: &Scene, material: &str) -> Option<f64> {
    scene
        .densities
        .get(material)
        .copied()
        .or_else(|| bimble_core::library::lookup(material).and_then(|l| l.props.density_kg_m3))
}

/// `density`: kg/m³ of the element's material, if known (for Mass).
fn cell(f: Field, e: &SceneElement, m: &Measures, location: &str, density: Option<f64>) -> Cell {
    let text = |s: &str| {
        if s.is_empty() {
            Cell::Empty
        } else {
            Cell::Text(s.to_string())
        }
    };
    match f {
        Field::Level => text(&e.storey),
        Field::Location => text(location),
        Field::Name => text(&e.name),
        Field::Class => text(&e.class),
        Field::Material => text(&e.material),
        Field::Section => text(&m.section),
        Field::Length => Cell::Num(m.length / 1000.0, 3),
        Field::Width => Cell::Num(m.width / 10.0, 1),
        Field::Height => Cell::Num(m.height / 10.0, 1),
        Field::Area => Cell::Num(m.area_m2, 2),
        Field::Volume => Cell::Num(m.volume_m3, 3),
        Field::Mass => match density {
            Some(rho) => Cell::Num(m.volume_m3 * rho, 0),
            None => Cell::Empty,
        },
        Field::GlobalId => text(&e.global_id),
    }
}

/// Sums of the summed fields, and the label (with the row count) in the first other column.
fn totals(fields: &[Field], rows: &[Row], label: String) -> Vec<Cell> {
    let mut out: Vec<Cell> = fields
        .iter()
        .enumerate()
        .map(|(k, f)| {
            if f.summed() {
                let s: f64 = rows
                    .iter()
                    .filter_map(|r| match r.cells[k] {
                        Cell::Num(v, _) => Some(v),
                        _ => None,
                    })
                    .sum::<f64>()
                    // An empty sum is -0.0: written "0.000", never "-0.000".
                    + 0.0;
                let decimals = match f {
                    Field::Area => 2,
                    Field::Mass => 0,
                    _ => 3,
                };
                Cell::Num(s, decimals)
            } else {
                Cell::Empty
            }
        })
        .collect();
    // The label goes in the first non-summed column; when every column is summed, the sums
    // win and the row has no label (cells stay aligned with the headers).
    if let Some(k) = fields.iter().position(|f| !f.summed()) {
        out[k] = Cell::Text(label);
    }
    out
}

/// The schedule's table for a scene: the listed elements, measured, grouped by level (lowest
/// first) and sorted by location and name, with subtotals and a total.
pub fn schedule(scene: &Scene, def: &ScheduleDef) -> Table {
    let cross = if def.fields.contains(&Field::Location) {
        crossings(scene)
    } else {
        Vec::new()
    };
    let mut rows: Vec<(String, Row, Vec<Cell>)> = Vec::new();
    for (i, e) in scene.elements.iter().enumerate() {
        if !def.lists(&e.class, e.category) {
            continue;
        }
        let Some(m) = measure(scene, e) else { continue };
        let location = cross
            .iter()
            .map(|(p, n)| ((p[0] - m.centre[0]).hypot(p[1] - m.centre[1]), n))
            .filter(|(d, _)| *d <= 1500.0)
            .min_by(|a, b| a.0.total_cmp(&b.0))
            .map(|(_, n)| n.clone())
            .unwrap_or_default();
        let rho = density(scene, &e.material);
        let cells: Vec<Cell> = def
            .fields
            .iter()
            .map(|f| cell(*f, e, &m, &location, rho))
            .collect();
        let key = vec![Cell::Text(location.clone()), Cell::Text(e.name.clone())];
        rows.push((e.storey.clone(), Row { element: i, cells }, key));
    }
    let count = rows.len();
    rows.sort_by(|a, b| {
        a.2.iter()
            .map(Cell::sort_key)
            .partial_cmp(b.2.iter().map(Cell::sort_key))
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    // Levels lowest first; elements on no known storey last.
    let order = |name: &str| {
        scene
            .storeys
            .iter()
            .position(|s| s.name == name)
            .unwrap_or(usize::MAX)
    };
    let mut groups: Vec<Group> = Vec::new();
    if def.group_by_level {
        let mut levels: Vec<String> = rows.iter().map(|r| r.0.clone()).collect();
        levels.sort_by_key(|l| (order(l), l.clone()));
        levels.dedup();
        for l in levels {
            let rs: Vec<Row> = rows
                .iter()
                .filter(|r| r.0 == l)
                .map(|r| r.1.clone())
                .collect();
            let label = format!(
                "{} ({})",
                if l.is_empty() { "No level" } else { &l },
                rs.len()
            );
            let subtotal = totals(&def.fields, &rs, label);
            groups.push(Group {
                level: l,
                rows: rs,
                subtotal,
            });
        }
    } else {
        let rs: Vec<Row> = rows.iter().map(|r| r.1.clone()).collect();
        groups.push(Group {
            level: String::new(),
            subtotal: Vec::new(),
            rows: rs,
        });
    }
    let all: Vec<Row> = groups.iter().flat_map(|g| g.rows.clone()).collect();
    let total = def
        .totals
        .then(|| totals(&def.fields, &all, format!("Total ({count})")));
    Table {
        name: def.name.clone(),
        fields: def.fields.clone(),
        groups,
        total,
        count,
    }
}

/// The table as CSV (comma separated, quoted where needed, numbers with a point), subtotals and
/// total included: for a spreadsheet.
pub fn to_csv(t: &Table) -> String {
    let q = |s: &str| {
        if s.contains([',', '"', '\n']) {
            format!("\"{}\"", s.replace('"', "\"\""))
        } else {
            s.to_string()
        }
    };
    let line = |cells: &[String]| cells.iter().map(|c| q(c)).collect::<Vec<_>>().join(",");
    let mut out = String::new();
    out.push_str(&line(
        &t.fields
            .iter()
            .map(|f| f.title().to_string())
            .collect::<Vec<_>>(),
    ));
    out.push('\n');
    for g in &t.groups {
        for r in &g.rows {
            out.push_str(&line(&r.cells.iter().map(Cell::text).collect::<Vec<_>>()));
            out.push('\n');
        }
        if !g.subtotal.is_empty() {
            out.push_str(&line(
                &g.subtotal.iter().map(Cell::text).collect::<Vec<_>>(),
            ));
            out.push('\n');
        }
    }
    if let Some(tot) = &t.total {
        out.push_str(&line(&tot.iter().map(Cell::text).collect::<Vec<_>>()));
        out.push('\n');
    }
    out
}

/// The table as aligned text, for a terminal: numbers right, text left, a blank line between
/// levels.
pub fn to_text(t: &Table) -> String {
    let mut lines: Vec<(Vec<String>, bool)> = vec![(
        t.fields.iter().map(|f| f.title().to_string()).collect(),
        false,
    )];
    for g in &t.groups {
        for r in &g.rows {
            lines.push((r.cells.iter().map(Cell::text).collect(), false));
        }
        if !g.subtotal.is_empty() {
            lines.push((g.subtotal.iter().map(Cell::text).collect(), true));
        }
    }
    if let Some(tot) = &t.total {
        lines.push((tot.iter().map(Cell::text).collect(), true));
    }
    let n = t.fields.len();
    let w: Vec<usize> = (0..n)
        .map(|k| {
            lines
                .iter()
                .map(|l| l.0.get(k).map_or(0, |c| c.chars().count()))
                .max()
                .unwrap_or(0)
        })
        .collect();
    let numeric: Vec<bool> = t
        .fields
        .iter()
        .map(|f| {
            matches!(
                f,
                Field::Length
                    | Field::Width
                    | Field::Height
                    | Field::Area
                    | Field::Volume
                    | Field::Mass
            )
        })
        .collect();
    let mut out = format!("{} ({})\n", t.name, t.count);
    for (cells, sum) in lines {
        let row: Vec<String> = cells
            .iter()
            .enumerate()
            .map(|(k, c)| {
                if numeric[k] {
                    format!("{c:>w$}", w = w[k])
                } else {
                    format!("{c:<w$}", w = w[k])
                }
            })
            .collect();
        out.push_str(row.join("  ").trim_end());
        out.push('\n');
        if sum {
            out.push('\n');
        }
    }
    out
}

/// The family a schedule is about, for a title: "Concrete" for concrete columns.
pub fn family_label(f: Option<MaterialCategory>) -> &'static str {
    f.map_or("Any", |f| f.label())
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use bimble_core::schedule::default_schedules;

    fn table(name: &str) -> Table {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let def = default_schedules()
            .into_iter()
            .find(|d| d.name == name)
            .unwrap();
        schedule(&scene, &def)
    }

    /// The demo frame: 12 columns of 300 × 300 per storey, on axes A–D × 1–3, 3 m high.
    #[test]
    fn concrete_columns_are_measured_located_and_totalled() {
        let t = table("Concrete columns");
        assert_eq!(t.count, 24);
        assert_eq!(t.groups.len(), 2);
        let f = |field: Field| t.fields.iter().position(|x| *x == field).unwrap();
        let first = &t.groups[0].rows[0];
        assert_eq!(first.cells[f(Field::Section)], Cell::Text("30/30".into()));
        assert_eq!(first.cells[f(Field::Location)], Cell::Text("A-1".into()));
        let Cell::Num(len, _) = first.cells[f(Field::Length)] else {
            panic!()
        };
        assert!((len - 3.0).abs() < 0.31, "{len}");
        // Every location once per storey.
        let mut locs: Vec<String> = t.groups[0]
            .rows
            .iter()
            .map(|r| r.cells[f(Field::Location)].text())
            .collect();
        locs.dedup();
        assert_eq!(locs.len(), 12, "{locs:?}");
        // Total volume = sum of rows.
        let Some(tot) = &t.total else { panic!() };
        let Cell::Num(v, _) = tot[f(Field::Volume)] else {
            panic!()
        };
        let sum: f64 = t
            .groups
            .iter()
            .flat_map(|g| &g.rows)
            .map(|r| match r.cells[f(Field::Volume)] {
                Cell::Num(x, _) => x,
                _ => 0.0,
            })
            .sum();
        assert!((v - sum).abs() < 1e-9 && v > 0.0);
    }

    #[test]
    fn beams_have_their_section_and_length() {
        let t = table("Concrete beams");
        assert_eq!(t.count, 34);
        let f = |field: Field| t.fields.iter().position(|x| *x == field).unwrap();
        let r = &t.groups[0].rows[0];
        assert_eq!(r.cells[f(Field::Section)], Cell::Text("25/50".into()));
        assert_eq!(r.cells[f(Field::Width)].text(), "25.0");
        assert_eq!(r.cells[f(Field::Height)].text(), "50.0");
        let csv = to_csv(&t);
        assert!(csv.starts_with("Level,Name,Material,Section,Length (m)"));
        assert!(csv.lines().last().unwrap().starts_with("Total (34)"));
        // Nothing in steel in the demo: an empty table, totals of zero (never "-0.000").
        let empty = table("Steel beams");
        assert_eq!(empty.count, 0);
        assert!(!to_csv(&empty).contains("-0"), "{}", to_csv(&empty));
    }

    #[test]
    fn natural_order() {
        let mut v = ["P10", "P9", "P1"].map(|s| Cell::Text(s.into()));
        v.sort_by(|a, b| a.sort_key().partial_cmp(&b.sort_key()).unwrap());
        assert_eq!(v.map(|c| c.text()), ["P1", "P9", "P10"]);
    }

    /// Mass uses the density the model gives its material (edited in the material browser), not
    /// only the library grade's: 2000 kg/m³ set on C30/37 gives 2000 × volume.
    #[test]
    fn mass_uses_the_models_density() {
        let mut model = concrete_frame(&FrameSpec::default());
        for x in model.materials.values_mut() {
            x.props.density_kg_m3 = Some(2000.0);
        }
        let scene = Scene::from_model(&model);
        let def = default_schedules()
            .into_iter()
            .find(|d| d.name == "All elements")
            .unwrap();
        let (v, k) = (
            def.fields.iter().position(|f| *f == Field::Volume).unwrap(),
            def.fields.iter().position(|f| *f == Field::Mass).unwrap(),
        );
        let t = schedule(&scene, &def);
        let r = &t.groups[0].rows[0];
        let (Cell::Num(vol, _), Cell::Num(mass, _)) = (&r.cells[v], &r.cells[k]) else {
            panic!("volume and mass are numbers");
        };
        assert!((mass - vol * 2000.0).abs() < 1e-6, "{mass} vs {vol}");
    }

    /// A summed first column keeps its sum in the totals: the label goes in the first
    /// non-summed column, or nowhere when every column is summed.
    #[test]
    fn summed_first_column_keeps_its_total() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut def = default_schedules()
            .into_iter()
            .find(|d| d.name == "Concrete columns")
            .unwrap();
        for fields in [
            vec![Field::Volume, Field::Name],
            vec![Field::Volume, Field::Area],
        ] {
            def.fields = fields;
            let t = schedule(&scene, &def);
            let tot = t.total.as_ref().unwrap();
            assert_eq!(tot.len(), def.fields.len());
            let Cell::Num(v, _) = tot[0] else {
                panic!("volume total lost: {tot:?}");
            };
            assert!(v > 0.0, "{tot:?}");
            assert!(
                t.groups
                    .iter()
                    .all(|g| matches!(g.subtotal[0], Cell::Num(..)))
            );
            if def.fields[1] == Field::Name {
                assert_eq!(tot[1], Cell::Text(format!("Total ({})", t.count)));
            }
        }
    }
}
