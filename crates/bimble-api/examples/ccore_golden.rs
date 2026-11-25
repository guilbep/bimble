//! Golden files for the C core (docs/research/c-core-spec.md §4): Rust is the oracle, each C
//! slice must print the same bytes. Formats and masking rules: `ccore/golden/README.md`.
//!
//! `ccore_golden --generate-frame`: build the demo concrete frame and write `frame.json` (its ids
//! are random: run once, commit the file).
//! `ccore_golden --generate-frame2`: build the second synthetic model through commands and
//! write `frame2.json` (random ids too: run once, commit).
//! `ccore_golden`: derive every other file from the committed `frame.json` and `frame2.json`
//! and write them.
//! `ccore_golden --check`: derive them again and byte-compare with `ccore/golden/`; exit 1
//! listing the files that differ, are missing or are not produced any more.
//!
//! IFC (`ifc/`): exports in IFC4 and IFC2x3, then `import_str` (`ifc/import/`) and the display
//! reader `read_str` (`ifc/read/`) on the crate fixtures, the hand-written `ifc/fixtures/` and the
//! exports.
//!
//! `ccore_golden --dump-read <in.ifc> <outdir>`, `--dump-import <in.ifc> <outdir>`: the same
//! formats for any IFC file, into `<outdir>` (for comparing the C core on files that are not
//! goldens; `ccore/tools/ifc_dump.c` is the C side). Nothing is masked.
//!
//! Every verb (`verbs_all.txt`, `frame2/verbs_all.txt`), the verbs' JSON schemas
//! (`verbs_schema/`, `verbs_list.json`), the viewer export (`view/*.bimbleview`) and the
//! quantities and schedule tables (`schedule/`).
//!
//! Georeference (`geo/`, ADR 0045): three synthetic models made from the frame (a map conversion,
//! a turn only, a model drawn in LV95), their files, IFC exports, imports, display reads, view
//! files and coordinate tables.
//!
//! The measure tool (`measure.txt`, ticket T1): `bimble_api::measure` (the viewer's) on fixed
//! lengths and point pairs.

use bimble_api::dispatch;
use bimble_core::demo::{FrameSpec, concrete_frame};
use bimble_core::library::{self, MaterialProps};
use bimble_core::{
    Command, Element, Generator, GlobalId, History, IfcClass, Material, MaterialCategory, Model,
    Placement, Storey, file,
};
use bimble_geom::scene::Geometry;
use bimble_geom::{Mesh, Scene};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

/// Fixed IFC header timestamp, so the export is reproducible.
const IFC_TIMESTAMP: &str = "2026-01-01T00:00:00";
/// Files in `ccore/golden/` this program does not produce (input, docs, another ticket's).
const NOT_PRODUCED: [&str; 3] = ["frame.json", "frame2.json", "README.md"];
const OTHER_TICKET_DIR: &str = "floats/";
/// The C core's own goldens, where Rust has no such behaviour (`ccore/golden/DEVIATIONS.md`), and
/// `mcp/`, which `ccore/tools/mcp_golden.sh` makes from `bimble mcp` (not this dumper).
const C_OWN: [&str; 6] = [
    "DEVIATIONS.md",
    "relations/",
    "mcp/",
    "grids/",
    "sheets/",
    "ifc_c/",
];

type Files = BTreeMap<String, Vec<u8>>;

fn golden_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../ccore/golden")
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_default();
    let dir = golden_dir();
    match mode.as_str() {
        "--generate-frame" => {
            let path = dir.join("frame.json");
            if path.exists() {
                eprintln!("frame.json exists; delete it first to make a new frame (new ids)");
                std::process::exit(2);
            }
            std::fs::create_dir_all(&dir).expect("create ccore/golden");
            let m = concrete_frame(&FrameSpec::default());
            std::fs::write(&path, file::to_string(&m)).expect("write frame.json");
            println!("wrote frame.json ({} elements)", m.elements.len());
        }
        "--generate-frame2" => {
            let path = dir.join("frame2.json");
            if path.exists() {
                eprintln!("frame2.json exists; delete it first to make a new one (new ids)");
                std::process::exit(2);
            }
            std::fs::create_dir_all(&dir).expect("create ccore/golden");
            let m = frame2();
            std::fs::write(&path, file::to_string(&m)).expect("write frame2.json");
            println!("wrote frame2.json ({} elements)", m.elements.len());
        }
        "" | "--check" => {
            let files = derive(&dir);
            if mode.is_empty() {
                write_all(&dir, &files);
                println!("wrote {} files", files.len());
            } else {
                let bad = check(&dir, &files);
                if bad.is_empty() {
                    println!("check: {} files identical", files.len());
                } else {
                    for b in &bad {
                        println!("differs: {b}");
                    }
                    std::process::exit(1);
                }
            }
        }
        "--dump-read" | "--dump-import" => {
            let a: Vec<String> = std::env::args().skip(2).collect();
            let [input, out] = a.as_slice() else {
                eprintln!("usage: ccore_golden {mode} <in.ifc> <outdir>");
                std::process::exit(2);
            };
            if mode == "--dump-read" {
                dump_read(input, out);
            } else {
                dump_import(input, out);
            }
        }
        _ => {
            eprintln!(
                "usage: ccore_golden [--generate-frame | --generate-frame2 | --check | \
                 --dump-read <in.ifc> <outdir> | --dump-import <in.ifc> <outdir>]"
            );
            std::process::exit(2);
        }
    }
}

fn write_all(dir: &Path, files: &Files) {
    for (rel, bytes) in files {
        let p = dir.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).expect("create dir");
        std::fs::write(&p, bytes).expect("write golden file");
    }
}

/// Paths (relative, `/`-separated) of every file under `dir`.
fn list(dir: &Path, rel: &str, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = format!("{rel}{name}");
        if e.path().is_dir() {
            list(&e.path(), &format!("{r}/"), out);
        } else {
            out.push(r);
        }
    }
}

/// The derived files are regenerated in memory (equivalent to a temp dir) and compared byte for
/// byte with the committed ones; committed files no longer produced are reported too.
fn check(dir: &Path, files: &Files) -> Vec<String> {
    let mut bad = Vec::new();
    for (rel, bytes) in files {
        match std::fs::read(dir.join(rel)) {
            Ok(b) if &b == bytes => {}
            Ok(_) => bad.push(rel.clone()),
            Err(_) => bad.push(format!("{rel} (missing)")),
        }
    }
    let mut on_disk = Vec::new();
    list(dir, "", &mut on_disk);
    for rel in on_disk {
        if !files.contains_key(&rel)
            && !NOT_PRODUCED.contains(&rel.as_str())
            && !rel.starts_with(OTHER_TICKET_DIR)
            && !C_OWN.iter().any(|p| rel.starts_with(p))
            && !is_golden_fixture(&rel)
        {
            bad.push(format!("{rel} (not produced any more)"));
        }
    }
    bad.sort();
    bad
}

fn derive(dir: &Path) -> Files {
    let text = std::fs::read_to_string(dir.join("frame.json"))
        .expect("ccore/golden/frame.json: run with --generate-frame first");
    let m = file::from_str(&text).expect("frame.json loads");
    assert_eq!(
        file::to_string(&m),
        text,
        "frame.json: load + save must give identical bytes"
    );
    let mut known: BTreeSet<GlobalId> = BTreeSet::new();
    known.insert(m.project_id);
    known.extend(m.storeys.keys());
    known.extend(m.materials.keys());
    known.extend(m.elements.keys());
    // The all-zero id the transcripts use as an unknown id is written literally, never masked.
    known.insert(GlobalId::from_u128(0));

    let mut files = Files::new();
    files.insert("guid.txt".into(), guid_txt(&m).into_bytes());
    mesh_files(&m, "mesh/", &mut files);
    bin_files(&m, "frame", &mut files);
    files.insert("query.txt".into(), query_txt(&m).into_bytes());
    verbs_files(&m, &known, &mut files);
    ifc_files(&m, &known, &mut files);
    frame2_files(dir, &mut files);
    ifc_more_files(dir, &mut files);
    verbs_all_files(dir, &m, &mut files);
    schema_files(&mut files);
    view_files(dir, &m, &mut files);
    schedule_files(dir, &m, &mut files);
    geo_files(&m, &mut files);
    drawing_files(dir, &m, &mut files);
    files.insert("measure.txt".into(), measure_txt().into_bytes());
    history_files(&m, &mut files);
    // ---- L1: reference plans (`reference/`), at the end of the list: see `reference_files`.
    reference_files(dir, &mut files);
    files
}

// ---------------------------------------------------------------- measure (ticket T1)

/// `measure.txt`: tab-separated lines the C test reads (`ccore/test/measure_test.c`):
///   `format_mm <x as f32 {:?}> <text>` for fixed lengths, edge cases included;
///   `pair <ax ay az> <bx by bz> <distance {:?}>`: the viewer's `Dimension::distance` (glam
///     `Vec3::length` of b - a: f32, x*x + y*y + z*z, then sqrt);
///   `measurements <json>`: the `measurements` query for those pairs, in order.
fn measure_txt() -> String {
    use bimble_api::measure::{format_mm, measurements_json};
    let mut s = String::new();
    let lengths: [f32; 34] = [
        0.0,
        -0.0,
        0.04,
        -0.04,
        0.05,
        0.25,
        0.35,
        0.4,
        0.75,
        2.25,
        12.345,
        99.94,
        99.95,
        99.96,
        99.99,
        100.0,
        100.4,
        100.5,
        999.5,
        1000.5,
        2500.0,
        2500.5,
        -4500.0,
        12345.6,
        123456.7,
        1e6,
        1234567.0,
        16777217.0,
        1e10,
        -1e10,
        3.4e38,
        f32::INFINITY,
        f32::NEG_INFINITY,
        f32::NAN,
    ];
    for x in lengths {
        writeln!(s, "format_mm\t{x:?}\t{}", format_mm(x)).unwrap();
    }
    let pairs: [([f32; 3], [f32; 3]); 3] = [
        ([-150.0, -150.0, 3000.0], [5850.0, -150.0, 3000.0]),
        ([0.0, 0.0, 0.0], [3000.0, 4000.0, 250.0]),
        ([100.5, 200.25, -12.75], [100.5, 200.25, -0.4]),
    ];
    let mut distances = Vec::new();
    for (a, b) in pairs {
        let d = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
        let mm = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
        distances.push(mm);
        writeln!(
            s,
            "pair\t{:?} {:?} {:?}\t{:?} {:?} {:?}\t{mm:?}",
            a[0], a[1], a[2], b[0], b[1], b[2]
        )
        .unwrap();
    }
    writeln!(s, "measurements\t{}", measurements_json(&distances)).unwrap();
    s
}

// ---------------------------------------------------------------- GlobalId

fn hex_bytes(b: &[u8]) -> String {
    if b.is_empty() {
        return "-".into();
    }
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn guid_txt(m: &Model) -> String {
    let mut s = String::new();
    let seeds: [&str; 24] = [
        "bimble view 3d",
        "",
        "a",
        "abc",
        "bimble",
        "element/proxy",
        "element/proxy-copy",
        "material/C30/37",
        "storey/0",
        "Niveau 1",
        "Fondations",
        "Maître d'ouvrage",
        "Béton armé C30/37",
        "日本語の建物",
        "emoji 🏗️ building",
        "tab\tand\nnewline",
        "0000000000000000000000",
        "3$$$$$$$$$$$$$$$$$$$$$",
        "The quick brown fox jumps over the lazy dog",
        "64 bytes: 0123456789012345678901234567890123456789012345678901234",
        "65 bytes: 01234567890123456789012345678901234567890123456789012345",
        &"x".repeat(1024),
        &"y".repeat(1025),
        &"z".repeat(3000),
    ];
    for seed in seeds {
        let g = GlobalId::derived(seed.as_bytes());
        writeln!(
            s,
            "derived {} {} {:032x}",
            hex_bytes(seed.as_bytes()),
            g,
            g.to_u128()
        )
        .unwrap();
    }
    for n in [
        0u128,
        1,
        2,
        63,
        64,
        u128::MAX,
        u128::MAX >> 1,
        1 << 127,
        0x0123456789abcdef0fedcba987654321,
    ] {
        let g = GlobalId::from_u128(n);
        writeln!(s, "from_u128 {n:032x} {g}").unwrap();
    }
    // Every id of the frame: parse, then format and the 16 raw bytes (big-endian u128).
    let ids = std::iter::once(m.project_id)
        .chain(m.storeys.keys().copied())
        .chain(m.materials.keys().copied())
        .chain(m.elements.keys().copied());
    for g in ids {
        let back: GlobalId = g.as_str().parse().expect("frame id parses");
        assert_eq!(back, g);
        assert_eq!(GlobalId::from_u128(g.to_u128()), g);
        writeln!(s, "parse {} {:032x}", back, back.to_u128()).unwrap();
    }
    for bad in [
        "",
        "short",
        "4$$$$$$$$$$$$$$$$$$$$$",
        "z000000000000000000000",
        "000000000000000000000",
        "00000000000000000000000",
        "000000000000000000000-",
        "00000000000000000000é",
        "0000000000000000000 00",
    ] {
        let e = bad.parse::<GlobalId>().unwrap_err();
        writeln!(s, "invalid {} {e}", hex_bytes(bad.as_bytes())).unwrap();
    }
    s
}

// ---------------------------------------------------------------- meshes

fn hex32(c: f32) -> String {
    format!("{:08x}", c.to_bits())
}

fn hex64(c: f64) -> String {
    format!("{:016x}", c.to_bits())
}

/// The format of `spikes/c-slab/expected/README.md`: v, i, e, then n lines.
fn mesh_text(m: &Mesh) -> String {
    let mut s = String::new();
    for p in &m.positions {
        writeln!(s, "v {} {} {}", hex32(p[0]), hex32(p[1]), hex32(p[2])).unwrap();
    }
    for t in m.indices.as_chunks::<3>().0 {
        writeln!(s, "i {} {} {}", t[0], t[1], t[2]).unwrap();
    }
    for [a, b] in &m.edges {
        let h: Vec<String> = a.iter().chain(b).map(|&c| hex32(c)).collect();
        writeln!(s, "e {}", h.join(" ")).unwrap();
    }
    for n in &m.edge_normals {
        let h: Vec<String> = n.iter().flatten().map(|v| v.to_string()).collect();
        writeln!(s, "n {}", h.join(" ")).unwrap();
    }
    s
}

fn category_index(c: MaterialCategory) -> usize {
    MaterialCategory::ALL.iter().position(|&x| x == c).unwrap()
}

fn mesh_files(m: &Model, prefix: &str, files: &mut Files) {
    let scene = Scene::from_model(m);
    let o = scene.origin_mm;
    let mut s = String::new();
    writeln!(s, "origin {} {} {}", hex64(o[0]), hex64(o[1]), hex64(o[2])).unwrap();
    writeln!(s, "elements {}", scene.elements.len()).unwrap();
    for (k, e) in scene.elements.iter().enumerate() {
        let mesh = e.local_mesh();
        let mut text = String::new();
        if let Geometry::Instance { to_scene: p, .. } = &e.geometry {
            let h: Vec<String> = [p.x, p.y, p.z, p.t]
                .iter()
                .flatten()
                .map(|&c| hex32(c))
                .collect();
            writeln!(text, "t {}", h.join(" ")).unwrap();
        }
        text.push_str(&mesh_text(mesh));
        writeln!(
            s,
            "{k:03} {} {} {} {:?} v={} i={} e={} n={}",
            e.global_id,
            e.class,
            category_index(e.category),
            e.category,
            mesh.positions.len(),
            mesh.indices.len() / 3,
            mesh.edges.len(),
            mesh.edge_normals.len()
        )
        .unwrap();
        files.insert(format!("{prefix}{k:03}.txt"), text.into_bytes());
    }
    for w in &scene.warnings {
        writeln!(s, "warning {w}").unwrap();
    }
    files.insert(format!("{prefix}scene.txt"), s.into_bytes());
}

// ---------------------------------------------------------------- queries

/// One record: `>>> <op> <name> <args-json>`, then `200 <answer>` or `400 <error text>`, then a
/// blank line.
fn record(s: &mut String, op: &str, name: &str, args: &Value, answer: &Result<String, String>) {
    writeln!(s, ">>> {op} {name} {args}").unwrap();
    match answer {
        Ok(a) => writeln!(s, "200 {a}").unwrap(),
        Err(e) => writeln!(s, "400 {e}").unwrap(),
    }
    s.push('\n');
}

fn query_txt(m: &Model) -> String {
    let first = |class: &str, nth: usize| {
        m.elements
            .values()
            .filter(|e| e.class_name() == class)
            .nth(nth)
            .unwrap()
            .id
            .to_string()
    };
    // The first beam along y (the first by id may run along x).
    let y_beam = m
        .elements
        .values()
        .find(|e| e.class_name() == "IfcBeam" && e.placement.z_axis == [0.0, 1.0, 0.0])
        .unwrap()
        .id
        .to_string();
    let storey = m
        .storeys
        .values()
        .find(|s| s.elevation == 0.0)
        .unwrap()
        .id
        .to_string();
    let calls: Vec<(&str, Value)> = vec![
        ("storeys", json!({})),
        ("materials", json!({})),
        ("project", json!({})),
        ("elements", json!({})),
        ("elements", json!({"class": "IfcBeam"})),
        ("elements", json!({"storey": storey})),
        ("elements", json!({"limit": 3})),
        (
            "elements",
            json!({"class": "IfcColumn", "storey": storey, "limit": 5}),
        ),
        ("element", json!({"id": first("IfcFooting", 0)})),
        ("element", json!({"id": first("IfcColumn", 0)})),
        ("element", json!({"id": first("IfcBeam", 0)})),
        ("element", json!({"id": y_beam})),
        ("element", json!({"id": first("IfcSlab", 0)})),
        ("nope", json!({})),
        ("element", json!({"id": "0000000000000000000000"})),
        ("element", json!({"id": "not an id"})),
        ("elements", json!({"class": "IfcNope"})),
        ("storeys", json!({"x": 1})),
    ];
    let mut s = String::new();
    for (name, args) in calls {
        let answer = dispatch::query(m, name, args.clone()).map(|v| v.to_string());
        record(&mut s, "query", name, &args, &answer);
    }
    s
}

// ---------------------------------------------------------------- verbs

/// New GlobalIds (absent from frame.json) numbered by first appearance across the verbs outputs.
struct Masker<'a> {
    known: &'a BTreeSet<GlobalId>,
    new: HashMap<GlobalId, u128>,
}

impl Masker<'_> {
    fn number(&mut self, g: GlobalId) -> Option<u128> {
        if self.known.contains(&g) {
            return None;
        }
        let n = self.new.len() as u128 + 1;
        Some(*self.new.entry(g).or_insert(n))
    }

    /// Every maximal run of IFC-alphabet characters, 22 long, first character 0-3, that is not a
    /// frame id becomes `<new-N>`.
    fn mask(&mut self, text: &str) -> String {
        let is_alpha = |c: u8| c.is_ascii_alphanumeric() || c == b'_' || c == b'$';
        let b = text.as_bytes();
        let mut out = String::with_capacity(text.len());
        let mut i = 0;
        while i < b.len() {
            if is_alpha(b[i]) {
                let start = i;
                while i < b.len() && is_alpha(b[i]) {
                    i += 1;
                }
                let run = &text[start..i];
                match run.parse::<GlobalId>().ok().and_then(|g| self.number(g)) {
                    Some(n) => write!(out, "<new-{n}>").unwrap(),
                    None => out.push_str(run),
                }
            } else {
                let ch = text[i..].chars().next().unwrap();
                out.push(ch);
                i += ch.len_utf8();
            }
        }
        out
    }

    /// A model's canonical text with its new ids replaced by `GlobalId::from_u128(N)` (so the
    /// sorted order no longer depends on random ids), then masked as `<new-N>`.
    fn model_text(&mut self, m: &Model) -> String {
        let mut text = file::to_string(m);
        // Number the new ids in the order the text shows them (all already seen in verbs.txt).
        let _ = self.mask(&text);
        for (g, n) in &self.new {
            text = text.replace(g.as_str(), GlobalId::from_u128(*n).as_str());
        }
        let sorted = file::to_string(&file::from_str(&text).expect("renumbered model loads"));
        let mut out = sorted;
        for n in self.new.values() {
            out = out.replace(GlobalId::from_u128(*n).as_str(), &format!("<new-{n}>"));
        }
        out
    }
}

fn history_lines(s: &mut String, h: &History) {
    let u: Vec<&str> = h.undo_labels().collect();
    let r: Vec<&str> = h.redo_labels().collect();
    writeln!(s, "undo {}", serde_json::to_string(&u).unwrap()).unwrap();
    writeln!(s, "redo {}", serde_json::to_string(&r).unwrap()).unwrap();
}

fn new_element(cmds: &[Command]) -> GlobalId {
    cmds.iter()
        .find_map(|c| match c {
            Command::AddElement(e) => Some(e.id),
            _ => None,
        })
        .expect("place adds an element")
}

/// The verbs session: model, history, transcript and masker.
struct Run<'a> {
    m: Model,
    h: History,
    s: String,
    mask: Masker<'a>,
}

impl Run<'_> {
    /// A verb through `dispatch::run`, then `History::apply`; the answer is the commands, masked.
    fn verb(&mut self, name: &str, args: Value) -> Option<Vec<Command>> {
        let r = dispatch::run(&self.m, name, args.clone()).and_then(|cmds| {
            let text = serde_json::to_string(&cmds).unwrap();
            self.h
                .apply(&mut self.m, cmds.clone())
                .map_err(|e| e.to_string())?;
            Ok((text, cmds))
        });
        let answer = match &r {
            Ok((t, _)) => Ok(self.mask.mask(t)),
            Err(e) => Err(e.clone()),
        };
        self.step("verb", name, &args, &answer);
        r.ok().map(|(_, c)| c)
    }

    fn undo_redo(&mut self, redo: bool) {
        let ok = if redo {
            self.h.redo(&mut self.m)
        } else {
            self.h.undo(&mut self.m)
        };
        let op = if redo { "redo" } else { "undo" };
        self.step(op, "-", &Value::Null, &Ok(ok.to_string()));
    }

    /// `>>> op name args`, the answer, the history (newest first), the element count, a blank.
    fn step(&mut self, op: &str, name: &str, args: &Value, answer: &Result<String, String>) {
        let args = self.mask.mask(&args.to_string());
        let s = &mut self.s;
        writeln!(s, ">>> {op} {name} {args}").unwrap();
        match answer {
            Ok(a) => writeln!(s, "200 {a}").unwrap(),
            Err(e) => writeln!(s, "400 {e}").unwrap(),
        }
        history_lines(s, &self.h);
        writeln!(s, "elements {}", self.m.elements.len()).unwrap();
        s.push('\n');
    }
}

fn verbs_files(frame: &Model, known: &BTreeSet<GlobalId>, files: &mut Files) {
    let storey = frame
        .storeys
        .values()
        .find(|s| s.elevation == 0.0)
        .unwrap()
        .id
        .to_string();
    let col0 = frame
        .elements
        .values()
        .find(|e| e.class_name() == "IfcColumn")
        .unwrap()
        .id
        .to_string();
    let mut r = Run {
        m: frame.clone(),
        h: History::default(),
        s: String::new(),
        mask: Masker {
            known,
            new: HashMap::new(),
        },
    };
    let place = |kind: &str, a: [f64; 2], b: Option<[f64; 2]>, mat: Option<&str>| {
        let mut v = json!({"kind": kind, "storey": storey, "a": a});
        if let Some(b) = b {
            v["b"] = json!(b);
        }
        if let Some(mat) = mat {
            v["material"] = json!(mat);
        }
        v
    };
    let placed = |r: &mut Run, args: Value| {
        new_element(&r.verb("place", args).expect("place succeeds")).to_string()
    };
    let column = placed(&mut r, place("column", [1500.0, 1500.0], None, None));
    let beam = placed(
        &mut r,
        place("beam", [0.0, 2500.0], Some([6000.0, 2500.0]), None),
    );
    let wall = placed(
        &mut r,
        place("wall", [0.0, -2000.0], Some([6000.0, -2000.0]), None),
    );
    let slab = placed(
        &mut r,
        place(
            "slab",
            [20000.0, 0.0],
            Some([24000.0, 4000.0]),
            Some("C35/45"),
        ),
    );
    r.verb(
        "move",
        json!({"ids": [column, col0], "d": [100.0, -50.0, 0.0]}),
    );
    r.verb(
        "set_generator",
        json!({"id": beam, "params": {"height": 600.0}}),
    );
    r.verb(
        "set_material",
        json!({"ids": [wall, col0], "material": "C35/45"}),
    );
    r.verb("set_note", json!({"id": column, "text": "  Check cover  "}));
    r.verb("delete", json!({"ids": [slab]}));
    for _ in 0..3 {
        r.undo_redo(false);
    }
    let after_undo = r.mask.model_text(&r.m);
    r.undo_redo(true);
    // Invalid calls: each is refused and changes nothing.
    r.verb(
        "place",
        json!({"kind": "door", "storey": storey, "a": [0.0, 0.0]}),
    );
    r.verb("place", place("beam", [0.0, 0.0], None, None));
    r.verb(
        "move",
        json!({"ids": ["0000000000000000000000"], "d": [1.0, 0.0, 0.0]}),
    );
    r.verb(
        "set_generator",
        json!({"id": column, "params": {"width": -5.0}}),
    );
    r.verb("fly", json!({}));
    let after = r.mask.model_text(&r.m);
    files.insert("verbs.txt".into(), r.s.into_bytes());
    files.insert("verbs_after_undo.json".into(), after_undo.into_bytes());
    files.insert("verbs_after.json".into(), after.into_bytes());
}

// ---------------------------------------------------------------- IFC

/// The Rust build's version in an export, replaced by a placeholder: the header's
/// `'bimble <v>'` and, in IFC2x3, the owning application's `IFCAPPLICATION(#n,'<v>',…)`.
fn mask_version(ifc: &str) -> String {
    let v = env!("CARGO_PKG_VERSION");
    ifc.replacen(&format!("'bimble {v}'"), "'bimble <version>'", 1)
        .replacen(
            &format!(",'{v}','Bimble','bimble')"),
            ",'<version>','Bimble','bimble')",
            1,
        )
}

fn ifc_files(m: &Model, known: &BTreeSet<GlobalId>, files: &mut Files) {
    let ifc = bimble_ifc::export(m, "frame.ifc", IFC_TIMESTAMP);
    // The header names the Rust build's version; the golden keeps a placeholder.
    let ifc = mask_version(&ifc);
    let (back, _report) = bimble_ifc::import::import_str(&ifc).expect("frame.ifc imports");
    let mut mask = Masker {
        known,
        new: HashMap::new(),
    };
    files.insert("ifc/frame.ifc".into(), ifc.into_bytes());
    files.insert(
        "ifc/frame_reimport.json".into(),
        mask.model_text(&back).into_bytes(),
    );
}

// ---------------------------------------------------------------- frame2

/// A second synthetic model with what the demo frame lacks: walls (one rotated 30° in plan),
/// extruded polygons (non-convex, with a void, a triangle), a mesh, `ifc_entity`, element
/// properties, project information, three materials with full, partial and library props, and
/// names that need JSON escapes. Built through commands on an empty model.
fn frame2() -> Model {
    let mut m = Model::new("Synthetic \"frame 2\" \\ golden");
    let mut h = History::default();
    let mut apply = |m: &mut Model, cmds: Vec<Command>| h.apply(m, cmds).expect("frame2 command");

    let ground = Storey {
        id: GlobalId::new(),
        name: "Rez-de-chaussée".into(),
        elevation: 0.0,
    };
    let upper = Storey {
        id: GlobalId::new(),
        name: "Étage \"1\"".into(),
        elevation: 3150.5,
    };
    let (s0, s1) = (ground.id, upper.id);
    apply(
        &mut m,
        vec![Command::AddStorey(ground), Command::AddStorey(upper)],
    );

    let grade = |name: &str| library::to_material(&library::lookup(name).expect("library grade"));
    let concrete = grade("C30/37");
    let steel = grade("S355");
    let timber = Material {
        id: GlobalId::new(),
        name: "Bois \"local\" \\ épicéa".into(),
        category: MaterialCategory::Timber,
        props: MaterialProps {
            density_kg_m3: Some(450.0),
            strength_mpa: Some(22.5),
            ..Default::default()
        },
    };
    let (mc, ms, mt) = (concrete.id, steel.id, timber.id);
    apply(
        &mut m,
        vec![
            Command::AddMaterial(concrete),
            Command::AddMaterial(steel),
            Command::AddMaterial(timber),
        ],
    );

    let el = |class, name: &str, storey, material, placement, generator| Element {
        id: GlobalId::new(),
        class,
        name: name.into(),
        storey,
        material,
        placement,
        generator,
        openings: Vec::new(),
        properties: Default::default(),
        ifc_entity: None,
    };
    let (c, s) = (30f64.to_radians().cos(), 30f64.to_radians().sin());
    let footing = el(
        IfcClass::IfcFooting,
        "F1",
        s0,
        mc,
        Placement::at(0.0, 0.0, -600.0),
        Generator::PadFooting {
            width: 1500.0,
            depth: 1500.0,
            height: 600.0,
        },
    );
    let column = el(
        IfcClass::IfcColumn,
        "C1",
        s0,
        mc,
        Placement::at(0.0, 0.0, 0.0),
        Generator::RectColumn {
            width: 300.0,
            depth: 400.0,
            height: 2899.9,
        },
    );
    let steel_column = el(
        IfcClass::IfcColumn,
        "HEB \"200\"",
        s0,
        ms,
        Placement::at(6000.0, 0.0, 0.0),
        Generator::RectColumn {
            width: 200.0,
            depth: 200.0,
            height: 2899.9,
        },
    );
    let beam = el(
        IfcClass::IfcBeam,
        "Poutre \\ \"B1\"",
        s0,
        mt,
        Placement {
            origin: [0.0, 0.0, 2650.0],
            z_axis: [1.0, 0.0, 0.0],
            x_axis: [0.0, 1.0, 0.0],
        },
        Generator::RectBeam {
            width: 160.0,
            height: 400.0,
            length: 6000.0,
        },
    );
    let wall = el(
        IfcClass::IfcWall,
        "W1",
        s0,
        mc,
        Placement::at(0.0, -1000.0, 0.0),
        Generator::Wall {
            length: 6000.0,
            thickness: 200.0,
            height: 2900.0,
        },
    );
    let rotated_wall = el(
        IfcClass::IfcWall,
        "W2\trotated 30°",
        s0,
        mc,
        Placement {
            origin: [6000.0, -1000.0, 0.0],
            z_axis: [0.0, 0.0, 1.0],
            x_axis: [c, s, 0.0],
        },
        Generator::Wall {
            length: 3500.0,
            thickness: 180.0,
            height: 2900.0,
        },
    );
    let slab = el(
        IfcClass::IfcSlab,
        "D1",
        s1,
        mc,
        Placement::at(0.0, -1000.0, -250.0),
        Generator::Slab {
            outline: vec![[0.0, 0.0], [6000.0, 0.0], [6000.0, 4000.0], [0.0, 4000.0]],
            thickness: 250.0,
        },
    );
    let l_slab = el(
        IfcClass::IfcSlab,
        "D2 (L)",
        s1,
        mc,
        Placement::at(8000.0, 0.0, -200.0),
        Generator::ExtrudedPolygon {
            outline: vec![
                [0.0, 0.0],
                [6000.0, 0.0],
                [6000.0, 2000.0],
                [2000.0, 2000.0],
                [2000.0, 5000.0],
                [0.0, 5000.0],
            ],
            voids: Vec::new(),
            height: 200.0,
        },
    );
    let void_slab = el(
        IfcClass::IfcSlab,
        "D3 trémie",
        s1,
        mc,
        Placement::at(0.0, 4000.0, -200.0),
        Generator::ExtrudedPolygon {
            outline: vec![[0.0, 0.0], [4000.0, 0.0], [4000.0, 3000.0], [0.0, 3000.0]],
            // Clockwise, as voids are.
            voids: vec![vec![
                [1000.0, 1000.0],
                [1000.0, 2200.0],
                [2500.5, 2200.0],
                [2500.5, 1000.0],
            ]],
            height: 200.0,
        },
    );
    let mut plate = el(
        IfcClass::IfcBuildingElementProxy,
        "Gousset",
        s1,
        ms,
        Placement {
            origin: [6000.0, 0.0, 500.0],
            z_axis: [0.0, -1.0, 0.0],
            x_axis: [1.0, 0.0, 0.0],
        },
        Generator::ExtrudedPolygon {
            outline: vec![[0.0, 0.0], [300.0, 0.0], [0.0, 300.0]],
            voids: Vec::new(),
            height: 15.0,
        },
    );
    plate.ifc_entity = Some("IfcPlate".into());
    // A tetrahedron, outward faces.
    let mut railing = el(
        IfcClass::IfcBuildingElementProxy,
        "Garde-corps",
        s1,
        ms,
        Placement::at(1000.0, 1000.0, 0.0),
        Generator::Mesh {
            positions: std::sync::Arc::new(vec![
                [0.0, 0.0, 0.0],
                [1000.0, 0.0, 0.0],
                [0.0, 1000.0, 0.0],
                [0.0, 0.0, 1100.0],
            ]),
            indices: std::sync::Arc::new(vec![0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]),
        },
    );
    railing.ifc_entity = Some("IfcRailing".into());
    let (col_id, rot_id, rail_id, beam_id) = (column.id, rotated_wall.id, railing.id, beam.id);
    apply(
        &mut m,
        [
            footing,
            column,
            steel_column,
            beam,
            wall,
            rotated_wall,
            slab,
            l_slab,
            void_slab,
            plate,
            railing,
        ]
        .into_iter()
        .map(Command::AddElement)
        .collect(),
    );

    let prop = |id, key: &str, value: &str| Command::SetProperty {
        id,
        key: key.into(),
        value: Some(value.into()),
    };
    apply(
        &mut m,
        vec![
            prop(col_id, "Pset_ColumnCommon/Reference", "C-1"),
            prop(
                col_id,
                "Pset_ConcreteElementGeneral/StrengthClass",
                "C30/37",
            ),
            prop(
                col_id,
                "Remarque \"chantier\"",
                "Béton \"apparent\" — vérifier",
            ),
            prop(col_id, "Chemin\\Clé", "C:\\temp\\x"),
            prop(col_id, "日本語/名前", "柱 🏗️"),
            prop(col_id, "Note", "a / b"),
            prop(rot_id, "Pset_WallCommon/IsExternal", "true"),
            prop(rot_id, "Pset_WallCommon/FireRating", "EI 60"),
            prop(rail_id, "Pset_RailingCommon/Height", "1100"),
            Command::Rename {
                id: beam_id,
                name: "Poutre \\ \"B1\" bois".into(),
            },
        ],
    );
    let project = [
        ("project_number", "2026-T2b"),
        ("project_title", "Synthetic \"frame 2\""),
        ("client", "Maître d'ouvrage — Ville"),
        ("project_address", "Rue du \"Lac\" 1\n1000 Ville"),
        ("phase", "51"),
        ("custom_note_2", "back\\slash\ttab 日本"),
    ]
    .into_iter()
    .map(|(k, v)| (k.to_string(), v.to_string()))
    .collect();
    apply(&mut m, vec![Command::SetProject { project }]);
    m
}

fn frame2_query_txt(m: &Model) -> String {
    let storey = |elev: f64| {
        m.storeys
            .values()
            .find(|s| s.elevation == elev)
            .unwrap()
            .id
            .to_string()
    };
    let (s0, s1) = (storey(0.0), storey(3150.5));
    let mut calls: Vec<(&str, Value)> = vec![
        ("storeys", json!({})),
        ("materials", json!({})),
        ("project", json!({})),
        ("elements", json!({})),
    ];
    for class in [
        "IfcColumn",
        "IfcBeam",
        "IfcSlab",
        "IfcWall",
        "IfcFooting",
        "IfcBuildingElementProxy",
    ] {
        calls.push(("elements", json!({ "class": class })));
    }
    calls.push(("elements", json!({ "storey": s0 })));
    calls.push(("elements", json!({ "storey": s1 })));
    calls.push(("elements", json!({"limit": 2})));
    calls.push((
        "elements",
        json!({"class": "IfcSlab", "storey": s1, "limit": 2}),
    ));
    for id in m.elements.keys() {
        calls.push(("element", json!({"id": id.to_string()})));
    }
    let mut s = String::new();
    for (name, args) in calls {
        let answer = dispatch::query(m, name, args.clone()).map(|v| v.to_string());
        record(&mut s, "query", name, &args, &answer);
    }
    s
}

fn frame2_files(dir: &Path, files: &mut Files) {
    let text = std::fs::read_to_string(dir.join("frame2.json"))
        .expect("ccore/golden/frame2.json: run with --generate-frame2 first");
    let m = file::from_str(&text).expect("frame2.json loads");
    assert_eq!(
        file::to_string(&m),
        text,
        "frame2.json: load + save must give identical bytes"
    );
    mesh_files(&m, "frame2/mesh/", files);
    bin_files(&m, "frame2", files);
    files.insert("frame2/query.txt".into(), frame2_query_txt(&m).into_bytes());
}

/// `bin/<name>.bimble`: the binary file Rust's `save` writes (`file::to_bytes`, version 2), and
/// `bin/<name>.cbor`: its body inflated (the CBOR the C core must write byte for byte).
fn bin_files(m: &Model, name: &str, files: &mut Files) {
    use std::io::Read as _;
    let bytes = file::to_bytes(m);
    let body = &bytes[file::MAGIC.len()..];
    let mut cbor = Vec::new();
    flate2::read::GzDecoder::new(body)
        .read_to_end(&mut cbor)
        .expect("to_bytes gives gzip");
    let back = file::from_bytes(&bytes).expect("to_bytes loads");
    assert_eq!(
        file::to_string(&back),
        file::to_string(m),
        "{name}: binary save + load must give the same model"
    );
    files.insert(format!("bin/{name}.bimble"), bytes);
    files.insert(format!("bin/{name}.cbor"), cbor);
}

// ---------------------------------------------------------------- IFC import, export, display

/// The six fixtures of `crates/bimble-ifc/tests/fixtures/`.
const CRATE_FIXTURES: [&str; 6] = [
    "booleans",
    "chain18",
    "curves",
    "cyclic_map",
    "mirror",
    "tilt",
];
/// The hand-written fixtures of `ccore/golden/ifc/fixtures/`.
const GOLDEN_FIXTURES: [&str; 9] = [
    "circle_column",
    "faceted_brep",
    "indexed_arc",
    "ishape_beam",
    "mapped_item",
    "opening",
    "rect_hollow",
    "same_level",
    "triangulated",
];
/// Imported models whose text is larger than this keep `<name>.size.txt` instead of the text.
const IMPORT_MODEL_MAX_BYTES: usize = 1 << 20;
/// Display goldens with more triangles than this keep `scene.txt` only (no per-element files).
const READ_MESH_MAX_TRIANGLES: usize = 20_000;

/// An input of `ifc/fixtures/` the dumper reads (hand-written, not produced).
fn is_golden_fixture(rel: &str) -> bool {
    rel.strip_prefix("ifc/fixtures/")
        .and_then(|r| r.strip_suffix(".ifc"))
        .is_some_and(|n| GOLDEN_FIXTURES.contains(&n))
}

fn ifc_more_files(dir: &Path, files: &mut Files) {
    let read = |p: PathBuf| std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{p:?}: {e}"));
    let frame = file::from_str(&read(dir.join("frame.json"))).expect("frame.json loads");
    let frame2 = file::from_str(&read(dir.join("frame2.json"))).expect("frame2.json loads");
    let export = |m: &Model, name: &str, schema| {
        mask_version(&bimble_ifc::export_as(m, name, IFC_TIMESTAMP, schema))
    };
    let frame_ifc = export(&frame, "frame.ifc", bimble_ifc::Schema::Ifc4);
    let frame2_ifc = export(&frame2, "frame2.ifc", bimble_ifc::Schema::Ifc4);
    let frame_2x3 = export(&frame, "frame.ifc2x3.ifc", bimble_ifc::Schema::Ifc2x3);
    let frame2_2x3 = export(&frame2, "frame2.ifc2x3.ifc", bimble_ifc::Schema::Ifc2x3);
    files.insert("ifc/frame2.ifc".into(), frame2_ifc.clone().into_bytes());
    files.insert(
        "ifc/frame.ifc2x3.ifc".into(),
        frame_2x3.clone().into_bytes(),
    );
    files.insert(
        "ifc/frame2.ifc2x3.ifc".into(),
        frame2_2x3.clone().into_bytes(),
    );

    let crate_dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../bimble-ifc/tests/fixtures");
    let mut inputs: Vec<(String, String)> = Vec::new();
    for name in CRATE_FIXTURES {
        inputs.push((name.into(), read(crate_dir.join(format!("{name}.ifc")))));
    }
    for name in GOLDEN_FIXTURES {
        inputs.push((
            name.into(),
            read(dir.join(format!("ifc/fixtures/{name}.ifc"))),
        ));
    }
    inputs.push(("frame2".into(), frame2_ifc));
    inputs.push(("frame2.ifc2x3".into(), frame2_2x3));
    for (name, text) in &inputs {
        import_files(name, text, files);
    }
    // The display reader also on the frame (IFC4 and IFC2x3), which import covers already.
    inputs.push(("frame".into(), frame_ifc));
    inputs.push(("frame.ifc2x3".into(), frame_2x3));
    for (name, text) in &inputs {
        read_files(name, text, files);
    }
}

/// Every token of `text` that is a valid GlobalId: the ids an import may keep.
fn ids_in(text: &str) -> BTreeSet<GlobalId> {
    text.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_' || c == '$'))
        .filter_map(|t| t.parse::<GlobalId>().ok())
        .chain(std::iter::once(GlobalId::from_u128(0)))
        .collect()
}

/// `ifc/import/<name>.json` (the model, masked) and `ifc/import/<name>.report.txt`.
fn import_files(name: &str, text: &str, files: &mut Files) {
    let report = match bimble_ifc::import::import_str(text) {
        Ok((m, rep)) => {
            let known = ids_in(text);
            let mut mask = Masker {
                known: &known,
                new: HashMap::new(),
            };
            let text = mask.model_text(&m);
            if text.len() <= IMPORT_MODEL_MAX_BYTES {
                files.insert(format!("ifc/import/{name}.json"), text.into_bytes());
            } else {
                files.insert(
                    format!("ifc/import/{name}.size.txt"),
                    import_size(&m, &text, &mut mask).into_bytes(),
                );
            }
            let mut s = String::new();
            for l in rep.lines() {
                writeln!(s, "{}", mask.mask(&l)).unwrap();
            }
            s
        }
        Err(e) => format!("error {e}\n"),
    };
    files.insert(format!("ifc/import/{name}.report.txt"), report.into_bytes());
}

/// A model too large to keep (the `chain18` bomb): its text's size and one line per element.
fn import_size(m: &Model, text: &str, mask: &mut Masker) -> String {
    let mut s = String::new();
    writeln!(s, "bytes {}", text.len()).unwrap();
    writeln!(s, "elements {}", m.elements.len()).unwrap();
    for e in m.elements.values() {
        let (v, i) = match &e.generator {
            Generator::Mesh { positions, indices } => (positions.len(), indices.len() / 3),
            _ => (0, 0),
        };
        let line = format!(
            "{} {} {} v={v} i={i}",
            e.id,
            e.class_name(),
            e.generator.name()
        );
        writeln!(s, "{}", mask.mask(&line)).unwrap();
    }
    s
}

fn json_str(s: &str) -> String {
    serde_json::to_string(s).unwrap()
}

/// `ifc/read/<name>/scene.txt` and `<k>.txt`: what `bimble_ifc::read::read_str` gives the viewer.
fn read_files(name: &str, text: &str, files: &mut Files) {
    let prefix = format!("ifc/read/{name}/");
    match bimble_ifc::read::read_str(text) {
        Ok(scene) => scene_files(&scene, &prefix, false, files),
        Err(e) => {
            files.insert(
                format!("{prefix}scene.txt"),
                format!("error {e}\n").into_bytes(),
            );
        }
    }
}

/// The `ifc/read/` files of a read scene under `prefix`. `hashes`: also `meshes.txt`, one line
/// per element `<k> <bytes> <FNV-1a 64 hex>` of its `<k>.txt` text, written whatever the
/// triangle count (the dump modes: a large file's meshes compared without writing them all).
fn scene_files(scene: &Scene, prefix: &str, hashes: bool, files: &mut Files) {
    let mut hash_lines = String::new();
    let o = scene.origin_mm;
    let mut s = String::new();
    writeln!(s, "schema {}", json_str(&scene.schema)).unwrap();
    writeln!(s, "origin {} {} {}", hex64(o[0]), hex64(o[1]), hex64(o[2])).unwrap();
    writeln!(
        s,
        "georef {}",
        serde_json::to_string(&scene.georef).unwrap()
    )
    .unwrap();
    writeln!(s, "storeys {}", scene.storeys.len()).unwrap();
    for st in &scene.storeys {
        writeln!(
            s,
            "storey {} {} {}",
            hex64(st.elevation_mm),
            json_str(&st.id),
            json_str(&st.name)
        )
        .unwrap();
    }
    writeln!(s, "grids {}", scene.grids.len()).unwrap();
    for g in &scene.grids {
        let (a, b) = (g.start_mm, g.end_mm);
        writeln!(
            s,
            "grid {} {} {} {} {}",
            hex64(a[0]),
            hex64(a[1]),
            hex64(b[0]),
            hex64(b[1]),
            json_str(&g.tag)
        )
        .unwrap();
    }
    let triangles = scene.triangle_count();
    let meshes = triangles <= READ_MESH_MAX_TRIANGLES;
    writeln!(
        s,
        "elements {} triangles {triangles} meshes {}",
        scene.elements.len(),
        if meshes { "yes" } else { "no" }
    )
    .unwrap();
    for (k, e) in scene.elements.iter().enumerate() {
        let mesh = e.local_mesh();
        writeln!(
            s,
            "{k:03} {} {} {} {:?} v={} i={} e={} n={} name={} storey={} material={}",
            e.global_id,
            e.class,
            category_index(e.category),
            e.category,
            mesh.positions.len(),
            mesh.indices.len() / 3,
            mesh.edges.len(),
            mesh.edge_normals.len(),
            json_str(&e.name),
            json_str(&e.storey),
            json_str(&e.material)
        )
        .unwrap();
        if meshes || hashes {
            let mut t = String::new();
            if let Geometry::Instance { to_scene: p, .. } = &e.geometry {
                let h: Vec<String> = [p.x, p.y, p.z, p.t]
                    .iter()
                    .flatten()
                    .map(|&c| hex32(c))
                    .collect();
                writeln!(t, "t {}", h.join(" ")).unwrap();
            }
            t.push_str(&mesh_text(mesh));
            if hashes {
                writeln!(
                    hash_lines,
                    "{k:03} {} {:016x}",
                    t.len(),
                    fnv64(t.as_bytes())
                )
                .unwrap();
            }
            if meshes {
                files.insert(format!("{prefix}{k:03}.txt"), t.into_bytes());
            }
        }
    }
    for w in &scene.warnings {
        writeln!(s, "warning {w}").unwrap();
    }
    files.insert(format!("{prefix}scene.txt"), s.into_bytes());
    if hashes {
        files.insert(format!("{prefix}meshes.txt"), hash_lines.into_bytes());
    }
}

/// FNV-1a, 64 bits: the dump modes' fingerprint of a text (the C driver computes the same).
fn fnv64(b: &[u8]) -> u64 {
    b.iter().fold(0xcbf2_9ce4_8422_2325, |h, &c| {
        (h ^ u64::from(c)).wrapping_mul(0x0100_0000_01b3)
    })
}

// ---------------------------------------------------------------- dump modes

/// Model text kept whole by `--dump-import` up to this size; over it only `model.size.txt`.
const DUMP_MODEL_MAX_BYTES: usize = 1 << 30;

fn dump_input(path: &str) -> String {
    let bytes = std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"));
    String::from_utf8_lossy(&bytes).into_owned()
}

fn dump_write(out: &str, files: &Files) {
    std::fs::create_dir_all(out).unwrap_or_else(|e| panic!("{out}: {e}"));
    write_all(Path::new(out), files);
}

/// `--dump-read <in.ifc> <outdir>`: the `ifc/read/` files of any IFC file, plus `meshes.txt`
/// (see `scene_files`) and `time.txt` (`read_ms <ms>`: `read_str` alone).
fn dump_read(input: &str, out: &str) {
    let text = dump_input(input);
    let t = std::time::Instant::now();
    let r = bimble_ifc::read::read_str(&text);
    let ms = t.elapsed().as_secs_f64() * 1e3;
    drop(text);
    let mut files = Files::new();
    match r {
        Ok(scene) => scene_files(&scene, "", true, &mut files),
        Err(e) => {
            files.insert("scene.txt".into(), format!("error {e}\n").into_bytes());
        }
    }
    files.insert("time.txt".into(), format!("read_ms {ms:.1}\n").into_bytes());
    dump_write(out, &files);
}

/// `--dump-import <in.ifc> <outdir>`: `import_str` of any IFC file. `report.txt` (the report's
/// lines, or `error <text>`); `model.size.txt`: `bytes` and `fnv` of the model text, and when the
/// model has a georeference `fnv_without_georef` and `georef <json>`, then `elements`, then per
/// element in id order `<id> <IfcClass> <generator kind> v=<positions> i=<triangles>`;
/// `model.json`: the model text without its georeference (the C model has none), when at most
/// 1 GiB; `time.txt` (`import_ms`). Ids are not masked: what the import derives
/// (`GlobalId::derived`) is deterministic, the C core derives the same.
fn dump_import(input: &str, out: &str) {
    let text = dump_input(input);
    let t = std::time::Instant::now();
    let r = bimble_ifc::import::import_str(&text);
    let ms = t.elapsed().as_secs_f64() * 1e3;
    drop(text);
    let mut files = Files::new();
    match r {
        Ok((mut m, rep)) => {
            let text = file::to_string(&m);
            let mut s = String::new();
            writeln!(s, "bytes {}", text.len()).unwrap();
            writeln!(s, "fnv {:016x}", fnv64(text.as_bytes())).unwrap();
            // The C model keeps no georeference: the text without it, for comparison.
            let mut text = text;
            if !m.georef.is_default() {
                let georef = std::mem::take(&mut m.georef);
                text = file::to_string(&m);
                writeln!(s, "fnv_without_georef {:016x}", fnv64(text.as_bytes())).unwrap();
                writeln!(s, "georef {}", serde_json::to_string(&georef).unwrap()).unwrap();
                m.georef = georef;
            }
            writeln!(s, "elements {}", m.elements.len()).unwrap();
            for e in m.elements.values() {
                let (v, i) = match &e.generator {
                    Generator::Mesh { positions, indices } => (positions.len(), indices.len() / 3),
                    _ => (0, 0),
                };
                let kind = e.generator.name();
                writeln!(s, "{} {} {kind} v={v} i={i}", e.id, e.class_name()).unwrap();
            }
            files.insert("model.size.txt".into(), s.into_bytes());
            if text.len() <= DUMP_MODEL_MAX_BYTES {
                files.insert("model.json".into(), text.into_bytes());
            }
            let mut r = String::new();
            for l in rep.lines() {
                writeln!(r, "{l}").unwrap();
            }
            files.insert("report.txt".into(), r.into_bytes());
        }
        Err(e) => {
            files.insert("report.txt".into(), format!("error {e}\n").into_bytes());
        }
    }
    files.insert(
        "time.txt".into(),
        format!("import_ms {ms:.1}\n").into_bytes(),
    );
    dump_write(out, &files);
}

// ---------------------------------------------------------------- every verb

/// The ids a `verbs_all` transcript writes as they are: the model's own (project, storeys,
/// materials, elements), the derived ids of its default views and of its site and building (the
/// `project` query), and the all-zero id.
fn verbs_all_known(m: &Model) -> BTreeSet<GlobalId> {
    let mut known: BTreeSet<GlobalId> = BTreeSet::new();
    known.insert(m.project_id);
    known.extend(m.storeys.keys());
    known.extend(m.materials.keys());
    known.extend(m.elements.keys());
    known.extend(m.view_ids());
    for part in ["site", "building"] {
        known.insert(GlobalId::derived(
            format!("{}/{part}", m.project_id).as_bytes(),
        ));
    }
    known.insert(GlobalId::from_u128(0));
    known
}

fn added_ids(cmds: &[Command]) -> Vec<String> {
    cmds.iter()
        .filter_map(|c| match c {
            Command::AddElement(e) => Some(e.id.to_string()),
            Command::AddSheet(s) => Some(s.id.to_string()),
            _ => None,
        })
        .collect()
}

impl Run<'_> {
    /// A verb that must succeed; the ids of the elements or sheets it adds.
    fn ok(&mut self, name: &str, args: Value) -> Vec<String> {
        let Some(cmds) = self.verb(name, args) else {
            let last = self.s.trim_end().rsplit("\n\n").next().unwrap_or_default();
            panic!("a verb that must succeed was refused:\n{last}");
        };
        added_ids(&cmds)
    }

    /// A verb that must be refused.
    fn bad(&mut self, name: &str, args: Value) {
        assert!(
            self.verb(name, args.clone()).is_none(),
            "{name} {args} is refused"
        );
    }

    fn undo_then_redo(&mut self) {
        self.undo_redo(false);
        self.undo_redo(true);
    }

    /// A read-only query, recorded as a step (`query <name> {}`), its answer masked.
    fn query(&mut self, name: &str) {
        let r = dispatch::query(&self.m, name, json!({}));
        let answer = r.map(|v| self.mask.mask(&v.to_string()));
        self.step("query", name, &json!({}), &answer);
    }

    /// The new verbs' shared part: project fields, views, schedules, sheets, then the queries.
    fn views_sheets_schedules(
        &mut self,
        fields: [(&str, &str); 4],
        plan: &str,
        schedules: [&str; 2],
    ) {
        // set_project_field: add, change, a second one, the same again (no command), remove.
        let [(l1, v1), (l1b, v1b), (l2, v2), (bad_label, _)] = fields;
        self.ok("set_project_field", json!({"label": l1, "value": v1}));
        self.undo_then_redo();
        self.ok("set_project_field", json!({"label": l1b, "value": v1b}));
        self.ok("set_project_field", json!({"label": l2, "value": v2}));
        self.ok("set_project_field", json!({"label": l2, "value": v2}));
        self.ok("set_project_field", json!({"label": l1, "value": ""}));
        self.bad(
            "set_project_field",
            json!({"label": bad_label, "value": "x"}),
        );
        self.bad("set_project_field", json!({"label": l1}));

        // set_view: a default view's first change adds it (AddView), the next sets it (SetView).
        let v3d = GlobalId::derived(b"bimble view 3d").to_string();
        self.ok(
            "set_view",
            json!({"id": v3d, "scale": 50, "true_north": true}),
        );
        self.undo_then_redo();
        self.ok("set_view", json!({"id": v3d, "scale": 200}));
        self.ok("set_view", json!({"id": v3d, "crop": null}));
        self.ok(
            "set_view",
            json!({"id": plan, "scale": 20, "crop": {"min": [-1000.0, -2000.5], "max": [30000.0, 25000.0]}}),
        );
        self.ok(
            "set_view",
            json!({"id": plan, "range": {"cut": {"offset_mm": 1000.0}, "bottom": {"offset_mm": -100.0}, "depth": {"offset_mm": -100.0}}}),
        );
        self.ok("set_view", json!({"id": plan, "crop": null}));
        self.ok(
            "set_view",
            json!({"id": plan, "range": {"bottom": {"offset_mm": -0.0}}}),
        );
        self.bad(
            "set_view",
            json!({"id": v3d, "crop": {"min": [0.0, 0.0], "max": [1.0, 1.0]}}),
        );
        self.bad(
            "set_view",
            json!({"id": plan, "crop": {"min": [10.0, 0.0], "max": [1.0, 1.0]}}),
        );
        self.bad(
            "set_view",
            json!({"id": "0000000000000000000000", "scale": 50}),
        );
        self.bad(
            "set_view",
            json!({"id": v3d, "range": {"cut": {"offset_mm": 900.0}}}),
        );
        self.bad("set_view", json!({"id": v3d, "scale": 0}));
        self.bad(
            "set_view",
            json!({"id": plan, "range": {"cut": {"offset_mm": -5000.0}}}),
        );
        self.bad("set_view", json!({"id": v3d, "zoom": 2}));

        // set_schedule: a built-in one's first change adds it to the stored list.
        let [a, b] = schedules;
        self.ok("set_schedule", json!({"name": a, "group_by_level": false}));
        self.undo_then_redo();
        self.ok(
            "set_schedule",
            json!({"name": a.to_uppercase(), "columns": ["level", "name", "volume", "global_id"]}),
        );
        self.ok("set_schedule", json!({"name": b, "family": null}));
        self.ok(
            "set_schedule",
            json!({"name": b, "family": "timber", "group_by_level": true}),
        );
        self.bad(
            "set_schedule",
            json!({"name": "Nope", "group_by_level": false}),
        );
        self.bad(
            "set_schedule",
            json!({"name": a, "columns": ["level", "bogus"]}),
        );
        self.bad("set_schedule", json!({"name": a, "columns": []}));

        // add_sheet: defaults, then given fields, then the next free number.
        let s1 = self.ok("add_sheet", json!({})).remove(0);
        self.undo_then_redo();
        let s2 = self
            .ok(
                "add_sheet",
                json!({"number": "A-009", "name": "Plans \"RDC\" \\ é", "paper": "A3", "portrait": true}),
            )
            .remove(0);
        self.ok("add_sheet", json!({"name": "Coupes"}));
        self.bad("add_sheet", json!({"paper": "A5"}));
        self.bad("add_sheet", json!({"number": "A-009"}));
        self.bad("add_sheet", json!({"number": "  "}));

        // set_sheet: change, the same again (no command), remove the revision.
        self.ok(
            "set_sheet",
            json!({"id": s1, "name": "Vue en plan", "revision": "B"}),
        );
        self.undo_then_redo();
        self.ok(
            "set_sheet",
            json!({"id": s2, "paper": "A0", "portrait": false}),
        );
        self.ok("set_sheet", json!({"id": s2, "paper": "A0"}));
        self.ok("set_sheet", json!({"id": s1, "revision": ""}));
        self.bad("set_sheet", json!({"id": s1, "number": "A-009"}));
        self.bad(
            "set_sheet",
            json!({"id": "0000000000000000000000", "name": "x"}),
        );
        self.bad("set_sheet", json!({"id": s1, "revision": "r".repeat(201)}));

        // Not `sheets`: it lists the sheets in id order, and new sheet ids are random.
        for q in ["project", "views", "schedules"] {
            self.query(q);
        }
    }
}

fn by_name(m: &Model, name: &str) -> String {
    m.elements
        .values()
        .find(|e| e.name == name)
        .unwrap_or_else(|| panic!("element {name:?}"))
        .id
        .to_string()
}

/// The id of the plan (looking down) of the storey at `elevation`.
fn plan_view(m: &Model, elevation: f64) -> String {
    let storey = m
        .storeys
        .values()
        .find(|s| s.elevation == elevation)
        .unwrap()
        .id;
    GlobalId::derived(format!("bimble view plan down {storey}").as_bytes()).to_string()
}

fn new_run<'a>(m: &Model, known: &'a BTreeSet<GlobalId>) -> Run<'a> {
    Run {
        m: m.clone(),
        h: History::default(),
        s: String::new(),
        mask: Masker {
            known,
            new: HashMap::new(),
        },
    }
}

/// `verbs_all.txt` on the demo frame: copy, rotate, mirror and the view, schedule, sheet and
/// project verbs.
fn verbs_all_frame(frame: &Model, known: &BTreeSet<GlobalId>) -> (String, String) {
    let mut r = new_run(frame, known);
    let nth = |class: &str, n: usize| {
        frame
            .elements
            .values()
            .filter(|e| e.class_name() == class)
            .nth(n)
            .unwrap()
            .id
            .to_string()
    };
    let (col0, col1, beam0, slab0) = (
        nth("IfcColumn", 0),
        nth("IfcColumn", 1),
        nth("IfcBeam", 0),
        nth("IfcSlab", 0),
    );
    // copy
    let c = r.ok(
        "copy",
        json!({"ids": [col0, beam0], "d": [7000.0, 0.0, 0.0]}),
    );
    r.undo_then_redo();
    let c2 = r.ok(
        "copy",
        json!({"ids": [c[0], slab0], "d": [0.0, -250.5, 3000.0]}),
    );
    r.ok("copy", json!({"ids": [c[0]], "d": [0.0, 0.0, 6000.0]}));
    r.ok(
        "copy",
        json!({"ids": ["0000000000000000000000"], "d": [1.0, 0.0, 0.0]}),
    );
    r.bad("copy", json!({"ids": [col0], "d": [1.0e8, 0.0, 0.0]}));
    r.bad("copy", json!({"ids": [col0]}));
    // rotate
    r.ok("rotate", json!({"ids": [col0, beam0], "angle_deg": 30.0}));
    r.undo_then_redo();
    r.ok(
        "rotate",
        json!({"ids": [c[0], c2[1]], "angle_deg": -45.5, "about": [1000.0, 2000.0]}),
    );
    r.ok("rotate", json!({"ids": [c[1]], "angle_deg": 90.0}));
    r.ok("rotate", json!({"ids": [c[1]], "angle_deg": 360.0}));
    r.bad("rotate", json!({"ids": [col0], "angle_deg": 400.0}));
    r.bad(
        "rotate",
        json!({"ids": ["0000000000000000000000"], "angle_deg": 10.0}),
    );
    // mirror
    r.ok(
        "mirror",
        json!({"ids": [col1, beam0], "axis": {"point": [3000.0, 0.0], "dir": [0.0, 1.0]}}),
    );
    r.undo_then_redo();
    r.ok(
        "mirror",
        json!({"ids": [c2[0], slab0], "axis": {"point": [0.0, 0.0], "dir": [1.0, 1.0]}}),
    );
    r.bad(
        "mirror",
        json!({"ids": [col1], "axis": {"point": [0.0, 0.0], "dir": [0.0, 0.0]}}),
    );
    r.bad(
        "mirror",
        json!({"ids": ["0000000000000000000000"], "axis": {"point": [0.0, 0.0], "dir": [1.0, 0.0]}}),
    );
    r.views_sheets_schedules(
        [
            ("Maître d'ouvrage", "Ville"),
            ("  maître D'OUVRAGE ", "Commune \"Lac\""),
            ("Phase", "51"),
            ("  --- ", ""),
        ],
        &plan_view(frame, 0.0),
        ["Concrete columns", "Steel beams"],
    );
    let after = r.mask.model_text(&r.m);
    (r.s, after)
}

/// `frame2/verbs_all.txt`: the six first verbs on walls, polygons and a mesh, then the others.
fn verbs_all_frame2(m2: &Model, known: &BTreeSet<GlobalId>) -> (String, String) {
    let mut r = new_run(m2, known);
    let id = |n: &str| by_name(m2, n);
    let storey = |elev: f64| {
        m2.storeys
            .values()
            .find(|s| s.elevation == elev)
            .unwrap()
            .id
            .to_string()
    };
    let (s0, s1) = (storey(0.0), storey(3150.5));
    let (w1, w2, d1, d2, d3) = (
        id("W1"),
        id("W2\trotated 30°"),
        id("D1"),
        id("D2 (L)"),
        id("D3 trémie"),
    );
    let (plate, rail, col) = (id("Gousset"), id("Garde-corps"), id("C1"));
    // place
    let w3 = r
        .ok(
            "place",
            json!({"kind": "wall", "storey": s0, "a": [0.0, 5000.0], "b": [4000.0, 8000.0]}),
        )
        .remove(0);
    r.undo_then_redo();
    r.ok(
        "place",
        json!({"kind": "slab", "storey": s1, "a": [0.0, 0.0], "b": [-3000.0, 2000.5], "material": "Bois \"local\" \\ épicéa"}),
    );
    r.ok(
        "place",
        json!({"kind": "column", "storey": s1, "a": [6000.0, 0.0], "sizes": {"column": 250.0}, "material": "S355"}),
    );
    r.bad(
        "place",
        json!({"kind": "wall", "storey": s0, "a": [0.0, 0.0], "b": [0.0, 0.0], "sizes": {"wall": -1.0}}),
    );
    // move
    r.ok("move", json!({"ids": [w2, d2], "d": [100.0, 200.0, 0.0]}));
    r.undo_then_redo();
    r.ok(
        "move",
        json!({"ids": [plate, rail], "d": [0.0, 0.0, -50.5]}),
    );
    r.bad("move", json!({"ids": [w2], "d": [0.0, 0.0, -2.0e7]}));
    // set_generator
    r.ok(
        "set_generator",
        json!({"id": w1, "params": {"thickness": 250.0}}),
    );
    r.undo_then_redo();
    r.ok(
        "set_generator",
        json!({"id": d2, "params": {"height": 300.0}}),
    );
    r.ok(
        "set_generator",
        json!({"id": w3, "params": {"length": 4500.0, "height": 3000.0}}),
    );
    r.bad(
        "set_generator",
        json!({"id": d2, "params": {"width": 300.0}}),
    );
    r.bad(
        "set_generator",
        json!({"id": rail, "params": {"height": 1.0}}),
    );
    r.bad("set_generator", json!({"id": w1, "params": {}}));
    // set_material
    r.ok("set_material", json!({"ids": [d2, d3], "material": "S355"}));
    r.undo_then_redo();
    r.ok(
        "set_material",
        json!({"ids": [w2, d1], "material": "C40/50"}),
    );
    r.ok("set_material", json!({"ids": [w2], "material": "C40/50"}));
    r.bad(
        "set_material",
        json!({"ids": [w2], "material": "Unobtainium"}),
    );
    // set_note
    r.ok(
        "set_note",
        json!({"id": d3, "text": " trémie à vérifier — \"urgent\" "}),
    );
    r.undo_then_redo();
    r.ok("set_note", json!({"id": d3, "text": "   "}));
    r.ok("set_note", json!({"id": col, "text": "ok"}));
    r.bad(
        "set_note",
        json!({"id": "0000000000000000000000", "text": "x"}),
    );
    // delete
    r.ok("delete", json!({"ids": [d3, rail]}));
    r.undo_then_redo();
    r.undo_redo(false);
    r.bad("delete", json!({"ids": "W1"}));
    // copy
    let c = r.ok(
        "copy",
        json!({"ids": [w2, d2, rail], "d": [0.0, 10000.0, 0.0]}),
    );
    r.undo_then_redo();
    r.ok("copy", json!({"ids": [d3, c[1]], "d": [0.0, 0.0, 3150.5]}));
    r.bad("copy", json!({"ids": [d3], "d": [0.0, 0.0, 2.0e7]}));
    // rotate
    r.ok("rotate", json!({"ids": [w1, d2], "angle_deg": 30.0}));
    r.undo_then_redo();
    r.ok(
        "rotate",
        json!({"ids": [plate, rail], "angle_deg": 17.25, "about": [0.0, 0.0]}),
    );
    r.ok("rotate", json!({"ids": [w2], "angle_deg": -90.0}));
    r.bad("rotate", json!({"ids": [w2], "angle_deg": -361.0}));
    // mirror
    r.ok(
        "mirror",
        json!({"ids": [w2, d3], "axis": {"point": [0.0, 0.0], "dir": [1.0, 0.0]}}),
    );
    r.undo_then_redo();
    r.ok(
        "mirror",
        json!({"ids": [rail, c[0]], "axis": {"point": [500.0, 500.0], "dir": [3.0, 4.0]}}),
    );
    r.bad(
        "mirror",
        json!({"ids": [w2], "axis": {"point": [1.0e8, 0.0], "dir": [1.0, 0.0]}}),
    );
    r.views_sheets_schedules(
        [
            ("Phase", "52"),
            ("Client", ""),
            ("Bureau d'études", "BET \"Structure\" 日本"),
            ("!!!", ""),
        ],
        &plan_view(m2, 3150.5),
        ["Timber beams", "Steel columns"],
    );
    let after = r.mask.model_text(&r.m);
    (r.s, after)
}

fn verbs_all_files(dir: &Path, frame: &Model, files: &mut Files) {
    let known = verbs_all_known(frame);
    let (s, after) = verbs_all_frame(frame, &known);
    files.insert("verbs_all.txt".into(), s.into_bytes());
    files.insert("verbs_all_after.json".into(), after.into_bytes());

    let text = std::fs::read_to_string(dir.join("frame2.json")).expect("frame2.json");
    let m2 = file::from_str(&text).expect("frame2.json loads");
    let known = verbs_all_known(&m2);
    let (s, after) = verbs_all_frame2(&m2, &known);
    files.insert("frame2/verbs_all.txt".into(), s.into_bytes());
    files.insert("frame2/verbs_all_after.json".into(), after.into_bytes());
}

// ---------------------------------------------------------------- verb schemas

fn pretty(v: &impl serde::Serialize) -> Vec<u8> {
    let mut s = serde_json::to_string_pretty(v).unwrap();
    s.push('\n');
    s.into_bytes()
}

/// `verbs_list.json` (`dispatch::list`, the `verbs` query) and `verbs_schema/<verb>.json`
/// (`dispatch::describe`).
fn schema_files(files: &mut Files) {
    let list = dispatch::list();
    files.insert("verbs_list.json".into(), pretty(&list));
    for v in &list {
        let schema = dispatch::describe(v.name).expect("every listed verb has a schema");
        files.insert(format!("verbs_schema/{}.json", v.name), pretty(&schema));
    }
}

// ---------------------------------------------------------------- viewer export

/// `view/<name>.bimbleview`: `viewfile::write(Scene::from_model(m), true)` (gzipped, as the
/// viewer's share and `bimble pack` write it), and `view/<name>.bimbleview.raw`: the same
/// without gzip (what the gzip stream inflates to).
fn view_files(dir: &Path, frame: &Model, files: &mut Files) {
    use bimble_geom::viewfile;
    use std::io::Read as _;
    let text = std::fs::read_to_string(dir.join("frame2.json")).expect("frame2.json");
    let m2 = file::from_str(&text).expect("frame2.json loads");
    for (name, m) in [("frame", frame), ("frame2", &m2)] {
        let scene = Scene::from_model(m);
        let gz = viewfile::write(&scene, true);
        let raw = viewfile::write(&scene, false);
        let mut inflated = Vec::new();
        flate2::read::GzDecoder::new(&gz[..])
            .read_to_end(&mut inflated)
            .expect("bimbleview is gzip");
        assert_eq!(inflated, raw, "{name}: gzip inflates to the raw stream");
        let back = viewfile::read_bytes(&gz).expect("bimbleview reads back");
        assert_eq!(back.elements.len(), scene.elements.len());
        files.insert(format!("view/{name}.bimbleview"), gz);
        files.insert(format!("view/{name}.bimbleview.raw"), raw);
    }
}

// ---------------------------------------------------------------- quantities and schedules

/// The schedules a golden table is made for: the effective ones of the model (the built-in
/// ten, as neither frame stores one), then two that no built-in one covers: every field, not
/// grouped; and only summed fields (no label in the totals), grouped, no total.
fn golden_schedules(m: &Model) -> Vec<bimble_core::schedule::ScheduleDef> {
    use bimble_core::schedule::{Field, ScheduleDef, effective};
    let mut v = effective(&m.schedules);
    v.push(ScheduleDef {
        name: "Every field".into(),
        classes: Vec::new(),
        family: None,
        fields: Field::ALL.to_vec(),
        group_by_level: false,
        totals: true,
    });
    v.push(ScheduleDef {
        name: "Sums only".into(),
        classes: vec!["IfcColumn".into(), "ifcbeam".into()],
        family: None,
        fields: vec![Field::Volume, Field::Area, Field::Mass, Field::Length],
        group_by_level: true,
        totals: false,
    });
    v
}

/// A schedule's file name: lower case, spaces as `-` ("concrete-columns").
fn schedule_slug(name: &str) -> String {
    name.to_lowercase().replace(' ', "-")
}

/// `schedule/<model>/`: per schedule `<slug>.json` (`table_json` with max 50, compact, `\n`)
/// and `<slug>.csv` (`to_csv`); `measures.txt`, one line per scene element (`measure` and the
/// mass from `density`, numbers as `{:?}`). Both frames get every effective schedule; the
/// hand-written IFC fixtures, read for display (`read_str`), get the two extra ones
/// (`schedule/ifc-<name>/`).
fn schedule_files(dir: &Path, frame: &Model, files: &mut Files) {
    let text = std::fs::read_to_string(dir.join("frame2.json")).expect("frame2.json");
    let m2 = file::from_str(&text).expect("frame2.json loads");
    for (name, m) in [("frame", frame), ("frame2", &m2)] {
        schedule_scene(&Scene::from_model(m), &golden_schedules(m), name, files);
    }
    let extra: Vec<_> = golden_schedules(&Model::default())
        .into_iter()
        .skip(bimble_core::schedule::default_schedules().len())
        .collect();
    for name in SCHEDULE_IFC {
        let path = dir.join(format!("ifc/fixtures/{name}.ifc"));
        let text = std::fs::read_to_string(&path).expect("ifc fixture");
        let scene = bimble_ifc::read::read_str(&text).expect("ifc fixture reads");
        schedule_scene(&scene, &extra, &format!("ifc-{name}"), files);
    }
}

/// The `ifc/fixtures/` given schedule tables (not `opening`: the C core cuts no openings).
const SCHEDULE_IFC: [&str; 7] = [
    "circle_column",
    "ishape_beam",
    "rect_hollow",
    "triangulated",
    "faceted_brep",
    "mapped_item",
    "indexed_arc",
];

fn schedule_scene(
    scene: &Scene,
    defs: &[bimble_core::schedule::ScheduleDef],
    name: &str,
    files: &mut Files,
) {
    use bimble_drawing::schedule::{density, measure, schedule, table_json, to_csv};
    let mut s = String::new();
    for (k, e) in scene.elements.iter().enumerate() {
        let Some(x) = measure(scene, e) else {
            writeln!(s, "{k:03} {} none", e.global_id).unwrap();
            continue;
        };
        let mass = density(scene, &e.material)
            .map_or("-".to_string(), |rho| format!("{:?}", x.volume_m3 * rho));
        writeln!(
            s,
            "{k:03} {} volume={:?} area={:?} length={:?} width={:?} height={:?} mass={mass} \
             centre={:?},{:?} section={}",
            e.global_id,
            x.volume_m3,
            x.area_m2,
            x.length,
            x.width,
            x.height,
            x.centre[0],
            x.centre[1],
            json_str(&x.section)
        )
        .unwrap();
    }
    files.insert(format!("schedule/{name}/measures.txt"), s.into_bytes());
    for def in defs {
        let t = schedule(scene, def);
        let slug = schedule_slug(&def.name);
        let mut j = serde_json::to_string(&table_json(&t, scene, 50)).unwrap();
        j.push('\n');
        files.insert(format!("schedule/{name}/{slug}.json"), j.into_bytes());
        files.insert(
            format!("schedule/{name}/{slug}.csv"),
            to_csv(&t).into_bytes(),
        );
    }
}

// ---------------------------------------------------------------- georeference (ADR 0045)

/// The frame with a place: `geo` (an IFC4 map conversion to LV95 and a turn), `turned` (a turn
/// only, no survey system) and `survey` (drawn in LV95 coordinates, as Revit exports are).
fn geo_models(frame: &Model) -> Vec<(&'static str, Model)> {
    use bimble_core::georef::{Georef, LV95};
    let mut geo = frame.clone();
    geo.georef = Georef {
        rotation_deg: 12.5,
        crs: Some(LV95.into()),
        east_m: 2_600_123.456,
        north_m: 1_199_876.5,
        height_m: 432.1,
        map_rotation_deg: -3.25,
    };
    let mut turned = frame.clone();
    turned.georef = Georef {
        rotation_deg: 30.0,
        ..Default::default()
    };
    let mut survey = frame.clone();
    for e in survey.elements.values_mut() {
        e.placement.origin[0] += 2_600_000_000.0;
        e.placement.origin[1] += 1_200_000_000.0;
    }
    survey.georef = Georef {
        rotation_deg: -20.0,
        crs: Some(LV95.into()),
        ..Default::default()
    };
    vec![("geo", geo), ("turned", turned), ("survey", survey)]
}

/// The status bar's coordinates, a copy of `bimble-viewer`'s `coords::text` and `swiss` (the
/// viewer is not a dependency of this example): the survey system, or WGS84 when asked.
fn coords_text(g: &bimble_core::georef::Georef, world_mm: [f64; 3], wgs84: bool) -> String {
    use bimble_core::georef::{LV95, Target};
    if wgs84 && let Some(([lat, lon, h], _)) = g.in_system(world_mm, Target::Wgs84) {
        let ns = if lat >= 0.0 { "N" } else { "S" };
        let ew = if lon >= 0.0 { "E" } else { "W" };
        return format!(
            "{:.6}° {ns}, {:.6}° {ew} · {} m (WGS84)",
            lat.abs(),
            lon.abs(),
            swiss(h, 2)
        );
    }
    let s = g.survey(world_mm);
    let height = format!("{} m", swiss(s[2], 2));
    match g.crs.as_deref() {
        Some(LV95) => format!(
            "E {} N {} · {height} (LV95)",
            swiss(s[0], 2),
            swiss(s[1], 2)
        ),
        Some(crs) => format!(
            "E {} N {} · {height} ({crs})",
            swiss(s[0], 2),
            swiss(s[1], 2)
        ),
        None => format!("x {} y {} · {height}", swiss(s[0], 3), swiss(s[1], 3)),
    }
}

fn swiss(x: f64, decimals: usize) -> String {
    let t = format!("{:.*}", decimals, x.abs());
    let (int, frac) = t.split_once('.').unwrap_or((&t, ""));
    let mut out = String::new();
    for (i, c) in int.chars().enumerate() {
        if i > 0 && (int.len() - i) % 3 == 0 {
            out.push('\'');
        }
        out.push(c);
    }
    let sign = if x < 0.0 && t.chars().any(|c| c.is_ascii_digit() && c != '0') {
        "-"
    } else {
        ""
    };
    if frac.is_empty() {
        format!("{sign}{out}")
    } else {
        format!("{sign}{out}.{frac}")
    }
}

/// Twelve world points (mm), moved by `at` (a model drawn in survey coordinates).
fn geo_points(at: [f64; 2]) -> Vec<[f64; 3]> {
    [
        [0.0, 0.0, 0.0],
        [12_345.6, -6_789.0, 3_000.0],
        [-50_000.0, 25_000.0, -1_500.0],
        [1e6, 1e6, 0.0],
        [-123_456.789, 98_765.432_1, 432_100.0],
        [0.5, -0.5, 0.004],
        [250_000.0, -180_000.0, 10_000.0],
        [-0.001, 0.001, -0.001],
        [99_999.995, 0.0, 0.0],
        [5e6, -5e6, 0.0],
        [-2e5, -3e5, 2e5],
        [777.777, 888.888, 999.999],
    ]
    .iter()
    .map(|p| [p[0] + at[0], p[1] + at[1], p[2]])
    .collect()
}

/// `geo/<name>.coords.txt`: the georeference, project north, and per point its survey
/// coordinates, the status bar's two texts, WGS84 by PROJ (9 decimals: a C build matches it within
/// 2e-9 degrees, `geo/README` in golden/README.md) and by swisstopo's formulas.
fn geo_coords(m: &Model, at: [f64; 2]) -> String {
    use bimble_core::georef::Target;
    let g = &m.georef;
    let mut s = String::new();
    writeln!(s, "georef {}", serde_json::to_string(g).unwrap()).unwrap();
    writeln!(
        s,
        "north rotation_deg {} map_rotation_deg {} plan_turn {} {} has_map {}",
        hex64(g.rotation_deg),
        hex64(g.map_rotation_deg),
        hex64(g.plan_turn(false)),
        hex64(g.plan_turn(true)),
        g.has_map()
    )
    .unwrap();
    let (rot, off) = g.survey_plan_to_world();
    writeln!(
        s,
        "survey_plan_to_world {} {} {}",
        hex64(rot),
        hex64(off[0]),
        hex64(off[1])
    )
    .unwrap();
    for p in geo_points(at) {
        writeln!(s, "point {:?} {:?} {:?}", p[0], p[1], p[2]).unwrap();
        let v = g.survey(p);
        writeln!(s, "survey {} {} {}", hex64(v[0]), hex64(v[1]), hex64(v[2])).unwrap();
        writeln!(s, "text {}", coords_text(g, p, false)).unwrap();
        writeln!(s, "text_wgs84 {}", coords_text(g, p, true)).unwrap();
        match g.in_system(p, Target::Wgs84) {
            Some(([lat, lon, _], _)) => writeln!(s, "wgs84 {lat:.9} {lon:.9}").unwrap(),
            None => writeln!(s, "wgs84 none").unwrap(),
        }
        match g.wgs84(p) {
            Some([lat, lon]) => writeln!(s, "approx {lat:.9} {lon:.9}").unwrap(),
            None => writeln!(s, "approx none").unwrap(),
        }
    }
    s
}

fn geo_files(frame: &Model, files: &mut Files) {
    use bimble_geom::viewfile;
    for (name, m) in geo_models(frame) {
        let text = file::to_string(&m);
        let back = file::from_str(&text).expect("geo model loads");
        assert_eq!(file::to_string(&back), text, "{name}: load + save");
        files.insert(format!("geo/{name}.json"), text.into_bytes());
        let mut bin = Files::new();
        bin_files(&m, name, &mut bin);
        for (k, v) in bin {
            files.insert(k.replace("bin/", "geo/"), v);
        }
        let ifc4 = mask_version(&bimble_ifc::export_as(
            &m,
            &format!("{name}.ifc"),
            IFC_TIMESTAMP,
            bimble_ifc::Schema::Ifc4,
        ));
        let ifc2x3 = mask_version(&bimble_ifc::export_as(
            &m,
            &format!("{name}.ifc2x3.ifc"),
            IFC_TIMESTAMP,
            bimble_ifc::Schema::Ifc2x3,
        ));
        files.insert(format!("geo/{name}.ifc"), ifc4.clone().into_bytes());
        files.insert(
            format!("geo/{name}.ifc2x3.ifc"),
            ifc2x3.clone().into_bytes(),
        );
        for (n, t) in [
            (name.to_string(), &ifc4),
            (format!("{name}.ifc2x3"), &ifc2x3),
        ] {
            let mut tmp = Files::new();
            import_files(&n, t, &mut tmp);
            read_files(&n, t, &mut tmp);
            for (k, v) in tmp {
                if let Some(r) = k.strip_prefix("ifc/import/") {
                    files.insert(format!("geo/import/{r}"), v);
                } else if let Some(r) = k.strip_prefix("ifc/read/")
                    && r.ends_with("/scene.txt")
                {
                    files.insert(format!("geo/read/{r}"), v);
                }
            }
        }
        let scene = Scene::from_model(&m);
        files.insert(
            format!("geo/{name}.bimbleview.raw"),
            viewfile::write(&scene, false),
        );
        let at = if name == "survey" {
            [2_600_000_000.0, 1_200_000_000.0]
        } else {
            [0.0, 0.0]
        };
        files.insert(
            format!("geo/{name}.coords.txt"),
            geo_coords(&m, at).into_bytes(),
        );
    }
}

// ---------------------------------------------------------------- drawings (round 3, D1–D6)

/// Fixed title block inputs of every drawing golden.
const DRAWING_PROJECT: &str = "frame";
const DRAWING_DATE: &str = "2026-01-01";

/// The ground plan's storey of each frame.
fn ground_storey(name: &str) -> &'static str {
    if name == "frame" {
        "Niveau 1"
    } else {
        "Rez-de-chaussée"
    }
}

/// Points as `{:?}` pairs: "x y x y …".
fn dbg_points(pts: &[[f64; 2]]) -> String {
    let v: Vec<String> = pts
        .iter()
        .map(|p| format!("{:?} {:?}", p[0], p[1]))
        .collect();
    v.join(" ")
}

/// The template of the minimal plan (D2): no axes, no annotation, no legend.
fn min_template() -> bimble_drawing::template::PlanTemplate {
    use bimble_drawing::template::{Axes, PlanTemplate};
    let mut t = PlanTemplate {
        axes: Axes::Off,
        dimensions: false,
        slab_levels: false,
        opening_crosses: false,
        opening_dimensions: false,
        element_labels: false,
        section_markers: false,
        ..Default::default()
    };
    t.legend.show = false;
    t
}

fn plan_options(
    m: &Model,
    storey: &str,
    template: bimble_drawing::template::PlanTemplate,
) -> bimble_drawing::plan::PlanOptions {
    bimble_drawing::plan::PlanOptions {
        storey: storey.into(),
        project: DRAWING_PROJECT.into(),
        date: DRAWING_DATE.into(),
        template: template.with_project(&m.project),
        ..Default::default()
    }
}

/// `drawing/`: every golden of the drawing tickets (D1–D6), made in one Rust build. Formats:
/// `ccore/golden/README.md` "Drawings".
fn drawing_files(dir: &Path, frame: &Model, files: &mut Files) {
    let text = std::fs::read_to_string(dir.join("frame2.json")).expect("frame2.json");
    let m2 = file::from_str(&text).expect("frame2.json loads");
    for (name, m) in [("frame", frame), ("frame2", &m2)] {
        let scene = Scene::from_model(m);
        drawing_cut(name, &scene, files);
        drawing_plans(name, m, &scene, files);
    }
    drawing_sample(files);
    drawing_roboto(files);
    drawing_style(files);
    drawing_fmt(files);
    let model = drawing_model(&m2);
    let model_text = file::to_string(&model);
    files.insert("drawing/model.json".into(), model_text.clone().into_bytes());
    let back = file::from_str(&model_text).expect("drawing/model.json loads");
    assert_eq!(
        file::to_string(&back),
        model_text,
        "drawing/model.json round trip"
    );
    drawing_views(&back, files);
    drawing_sheets(&back, files);
    drawing_pdfs(frame, &back, files);
}

/// `cut/<model>.txt`: what the ground plan's cut plane gives for each scene element.
fn drawing_cut(name: &str, scene: &Scene, files: &mut Files) {
    use bimble_drawing::cut::{facing_up_region, section};
    let storey = ground_storey(name);
    let st = scene
        .storeys
        .iter()
        .find(|s| s.name == storey)
        .expect("ground storey");
    let oz = scene.origin_mm[2];
    let level = st.elevation_mm - oz;
    let cut = level + 1200.0;
    let mut s = String::new();
    writeln!(s, "storey {}", json_str(storey)).unwrap();
    writeln!(
        s,
        "origin {:?} {:?} {:?}",
        scene.origin_mm[0], scene.origin_mm[1], scene.origin_mm[2]
    )
    .unwrap();
    writeln!(s, "level {level:?}").unwrap();
    writeln!(s, "cut {cut:?}").unwrap();
    for e in &scene.elements {
        let m = e.world_mesh();
        let sec = section(&m, cut);
        let up = facing_up_region(&m);
        writeln!(
            s,
            "element {} {} loops {} open {} up {}",
            e.global_id,
            e.class,
            sec.loops.len(),
            sec.open.len(),
            up.len()
        )
        .unwrap();
        for l in &sec.loops {
            writeln!(s, "loop {} {}", l.len(), dbg_points(l)).unwrap();
        }
        for l in &sec.open {
            writeln!(s, "open {} {}", l.len(), dbg_points(l)).unwrap();
        }
        for l in &up {
            writeln!(s, "up {} {}", l.len(), dbg_points(l)).unwrap();
        }
    }
    files.insert(format!("drawing/cut/{name}.txt"), s.into_bytes());
    drawing_cut_more(name, scene, level, cut, files);
}

/// `cut/<model>.more.txt`: per scene element its sections at every storey's cut (level + 1200)
/// and at its own mid height, then each edge clipped to the ground plan's heights (`clip_z` from
/// level − 1 to the cut, as `plan` does) with `edge_seen_from_above`.
fn drawing_cut_more(name: &str, scene: &Scene, level: f64, cut: f64, files: &mut Files) {
    use bimble_drawing::cut::{clip_z, edge_seen_from_above, section};
    let oz = scene.origin_mm[2];
    let cuts: Vec<f64> = scene
        .storeys
        .iter()
        .map(|s| s.elevation_mm - oz + 1200.0)
        .collect();
    let mut s = String::new();
    for e in &scene.elements {
        let m = e.world_mesh();
        let (zmin, zmax) = m
            .positions
            .iter()
            .fold((f64::INFINITY, f64::NEG_INFINITY), |(a, b), p| {
                (a.min(p[2] as f64), b.max(p[2] as f64))
            });
        writeln!(s, "element {} {}", e.global_id, e.class).unwrap();
        for h in cuts.iter().copied().chain([(zmin + zmax) / 2.0]) {
            let sec = section(&m, h);
            writeln!(
                s,
                "at {h:?} loops {} open {}",
                sec.loops.len(),
                sec.open.len()
            )
            .unwrap();
            for l in &sec.loops {
                writeln!(s, "loop {} {}", l.len(), dbg_points(l)).unwrap();
            }
            for l in &sec.open {
                writeln!(s, "open {} {}", l.len(), dbg_points(l)).unwrap();
            }
        }
        for (i, seg) in m.edges.iter().enumerate() {
            let seen = m.edge_normals.get(i).is_none_or(edge_seen_from_above);
            let (a, b) = (seg[0].map(f64::from), seg[1].map(f64::from));
            match clip_z(a, b, level - 1.0, cut) {
                Some(c) => writeln!(s, "clip {i} {} {}", seen as u8, dbg_points(&c)).unwrap(),
                None => writeln!(s, "clip {i} {} none", seen as u8).unwrap(),
            }
        }
    }
    files.insert(format!("drawing/cut/{name}.more.txt"), s.into_bytes());
}

fn frame_text(
    sheet: &bimble_drawing::sheet::Sheet,
    f: &bimble_drawing::viewdraw::ViewFrame,
) -> String {
    let mut s = String::new();
    writeln!(s, "sheet {:?} {:?}", sheet.width_mm, sheet.height_mm).unwrap();
    writeln!(s, "turn {:?}", f.turn).unwrap();
    writeln!(
        s,
        "origin {:?} {:?} {:?}",
        f.origin[0], f.origin[1], f.origin[2]
    )
    .unwrap();
    writeln!(s, "x0 {:?}", f.x0).unwrap();
    writeln!(s, "y1 {:?}", f.y1).unwrap();
    writeln!(s, "k {:?}", f.k).unwrap();
    writeln!(s, "ox {:?}", f.ox).unwrap();
    writeln!(s, "oy {:?}", f.oy).unwrap();
    writeln!(s, "cut {:?}", f.cut).unwrap();
    s
}

/// `plan/<model>/`: the cut alone on the minimal plan's paper (D1), the minimal plan (D2), the
/// default template's plans and its errors (D3).
fn drawing_plans(name: &str, m: &Model, scene: &Scene, files: &mut Files) {
    use bimble_core::style::Rgb;
    use bimble_drawing::cut::section;
    use bimble_drawing::plan::plan;
    use bimble_drawing::sheet::{Item, Paper, Pen, Sheet};
    use bimble_drawing::template::PlanTemplate;
    let storey = ground_storey(name);
    let dirp = format!("drawing/plan/{name}");
    let mut o = plan_options(m, storey, min_template());
    o.bare = true;
    let (min, info) = plan(scene, &o).expect("minimal plan");
    files.insert(format!("{dirp}/min.svg"), min.to_svg().into_bytes());
    files.insert(
        format!("{dirp}/min.info.txt"),
        format!("{info:#?}\n").into_bytes(),
    );
    // The cut alone: one area per element through the frame, one pen, no fill or hatch.
    let f = info.frame;
    let o3 = scene.origin_mm;
    let to_paper = |p: &[f64; 2]| f.to_paper([p[0] + o3[0], p[1] + o3[1], f.cut + o3[2]]);
    let pen = Pen {
        width_mm: 0.35,
        color: Rgb([0, 0, 0]),
        dash: Vec::new(),
    };
    let thin = Pen {
        width_mm: 0.18,
        color: Rgb([200, 30, 30]),
        dash: vec![1.0, 0.5],
    };
    let mut items = Vec::new();
    for e in &scene.elements {
        let s = section(&e.world_mesh(), f.cut);
        if !s.loops.is_empty() {
            items.push(Item::Area {
                loops: s
                    .loops
                    .iter()
                    .map(|l| l.iter().map(to_paper).collect())
                    .collect(),
                fill: None,
                hatches: Vec::new(),
                outline: Some(pen.clone()),
            });
        }
        if !s.open.is_empty() {
            items.push(Item::Lines {
                lines: s
                    .open
                    .iter()
                    .map(|l| l.iter().map(to_paper).collect())
                    .collect(),
                pen: thin.clone(),
            });
        }
    }
    let cut_sheet = Sheet {
        width_mm: min.width_mm,
        height_mm: min.height_mm,
        title: format!("Cut {storey}"),
        items,
        font: min.font,
    };
    files.insert(format!("{dirp}/cut.svg"), cut_sheet.to_svg().into_bytes());
    files.insert(
        format!("{dirp}/frame.txt"),
        frame_text(&min, &f).into_bytes(),
    );
    // The default template: auto paper at 1:100, bare, A1 at 1:50.
    let full = plan_options(m, storey, PlanTemplate::default());
    let mut bare = full.clone();
    bare.bare = true;
    let mut a1 = full.clone();
    a1.scale = 50;
    a1.paper = Some(Paper::A1);
    for (file, opts) in [("full", &full), ("bare", &bare), ("a1_50", &a1)] {
        let (sheet, info) = plan(scene, opts).expect("plan");
        files.insert(format!("{dirp}/{file}.svg"), sheet.to_svg().into_bytes());
        files.insert(
            format!("{dirp}/{file}.info.txt"),
            format!("{info:#?}\n").into_bytes(),
        );
    }
    let mut errors = String::new();
    let mut zero = full.clone();
    zero.scale = 0;
    let mut nope = full.clone();
    nope.storey = "No such storey".into();
    let mut small = full.clone();
    small.scale = 1;
    small.paper = Some(Paper::A4);
    for (what, opts) in [
        ("scale 0", &zero),
        ("unknown storey", &nope),
        ("A4 at 1:1", &small),
    ] {
        let Err(e) = plan(scene, opts) else {
            panic!("{what}: an error expected");
        };
        writeln!(errors, "{what}: {e}").unwrap();
    }
    files.insert(format!("{dirp}/errors.txt"), errors.into_bytes());
}

/// `svg/sample.svg` (Helvetica) and `svg/sample-roboto.svg`: a sheet with each kind of item.
fn drawing_sample_sheet(font: bimble_core::style::SheetFont) -> bimble_drawing::sheet::Sheet {
    use bimble_core::style::Rgb;
    use bimble_drawing::sheet::{Anchor, Hatch, Item, Pen, Sheet};
    let black = Rgb([0, 0, 0]);
    let red = Rgb([200, 30, 30]);
    let pen = |w: f64, dash: Vec<f64>| Pen {
        width_mm: w,
        color: black,
        dash,
    };
    let text =
        |pos: [f64; 2], size: f64, t: &str, anchor: Anchor, angle: f64, bold: bool| Item::Text {
            pos,
            size_mm: size,
            text: t.into(),
            color: if bold { red } else { black },
            anchor,
            angle_deg: angle,
            bold,
        };
    let mut items = vec![
        // A square with a hole: fill, two hatches, outline.
        Item::Area {
            loops: vec![
                vec![[10.0, 10.0], [60.0, 10.0], [60.0, 60.0], [10.0, 60.0]],
                vec![[20.0, 20.0], [50.0, 20.0], [50.0, 50.0], [20.0, 50.0]],
            ],
            fill: Some(Rgb([214, 214, 210])),
            hatches: vec![
                Hatch {
                    angle_deg: 45.0,
                    spacing_mm: 2.0,
                    offset_mm: 0.0,
                    pen: pen(0.13, Vec::new()),
                },
                Hatch {
                    angle_deg: -30.0,
                    spacing_mm: 3.3,
                    offset_mm: 0.7,
                    pen: pen(0.1, vec![0.8, 0.4]),
                },
            ],
            outline: Some(pen(0.5, Vec::new())),
        },
        // A triangle, hatched at 0° and 90°, not filled, not outlined.
        Item::Area {
            loops: vec![vec![[70.0, 60.0], [110.33333333333333, 12.5], [95.0, 60.0]]],
            fill: None,
            hatches: vec![
                Hatch {
                    angle_deg: 0.0,
                    spacing_mm: 1.5,
                    offset_mm: 0.25,
                    pen: pen(0.1, Vec::new()),
                },
                Hatch {
                    angle_deg: 90.0,
                    spacing_mm: 1.5,
                    offset_mm: f64::NAN,
                    pen: pen(0.1, Vec::new()),
                },
            ],
            outline: None,
        },
        // A filled area with a hatch of no spacing (no lines), and an empty area.
        Item::Area {
            loops: vec![vec![[120.0, 10.0], [140.0, 10.0], [130.0, 30.0]]],
            fill: Some(Rgb([255, 255, 255])),
            hatches: vec![Hatch {
                angle_deg: 15.0,
                spacing_mm: 0.0,
                offset_mm: 0.0,
                pen: pen(0.1, Vec::new()),
            }],
            outline: Some(pen(0.25, vec![2.0, 1.0, 0.25, 1.0])),
        },
        Item::Area {
            loops: Vec::new(),
            fill: None,
            hatches: vec![Hatch {
                angle_deg: 45.0,
                spacing_mm: 1.0,
                offset_mm: 0.0,
                pen: pen(0.1, Vec::new()),
            }],
            outline: Some(pen(0.25, Vec::new())),
        },
        // Lines: two polylines, dashed; numbers that round at the third decimal.
        Item::Lines {
            lines: vec![
                vec![[0.0625, 0.1875], [1.0005, 2.0004999], [-0.0004, -0.0]],
                vec![[150.123456, 70.98765], [290.9999, 200.00049999999]],
            ],
            pen: Pen {
                width_mm: 0.0625,
                color: Rgb([1, 2, 254]),
                dash: vec![5.0, 1.25, 0.3333, 1.25],
            },
        },
        Item::Lines {
            lines: vec![vec![[1e-7, 123456789.0], [-1e21, 3.0]]],
            pen: pen(0.18, Vec::new()),
        },
    ];
    // Texts: the three anchors, turned, bold, escaped, accents.
    let mut y = 80.0;
    for (i, anchor) in [Anchor::Start, Anchor::Middle, Anchor::End]
        .into_iter()
        .enumerate()
    {
        for (j, angle) in [0.0, 90.0, -33.5].into_iter().enumerate() {
            items.push(text(
                [150.0, y],
                2.5 + i as f64 * 0.5,
                "Plan Niveau 1, 1:100",
                anchor,
                angle,
                j == 1,
            ));
            y += 9.0;
        }
    }
    items.push(text(
        [20.0, 100.0],
        3.5,
        "<b> & \"q\" 'a' — Étage \"1\" Rez-de-chaussée ±0.00 × 2 µ",
        Anchor::Start,
        0.0,
        false,
    ));
    items.push(text(
        [20.0, 110.0],
        3.5,
        "Bold: Ÿ ÿ Œ œ ĳ ł ő ș ț 50 × 30 cm",
        Anchor::Middle,
        0.0,
        true,
    ));
    items.push(text([20.0, 120.0], 2.0, "", Anchor::End, 12.0, false));
    items.push(text(
        [280.0, 200.0],
        5.0,
        "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz {|}~",
        Anchor::End,
        0.0,
        false,
    ));
    items.push(text(
        [20.0, 130.0],
        3.0,
        "日本 ĀŻ ǅ ɏ \u{2009}\u{2013}\u{2014}\u{2019}",
        Anchor::Middle,
        180.0,
        false,
    ));
    Sheet {
        width_mm: 297.0,
        height_mm: 210.0,
        title: "Sample <sheet> & \"é\"".into(),
        items,
        font,
    }
}

fn drawing_sample(files: &mut Files) {
    use bimble_core::style::SheetFont;
    files.insert(
        "drawing/svg/sample.svg".into(),
        drawing_sample_sheet(SheetFont::Helvetica)
            .to_svg()
            .into_bytes(),
    );
    files.insert(
        "drawing/svg/sample-roboto.svg".into(),
        drawing_sample_sheet(SheetFont::Roboto)
            .to_svg()
            .into_bytes(),
    );
}

/// Characters `roboto.txt` lists: U+0020–U+024F and the punctuation plans use.
fn roboto_chars() -> Vec<char> {
    let mut v: Vec<char> = (0x20u32..=0x24f).filter_map(char::from_u32).collect();
    v.extend(
        [
            0x2009u32, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x2032,
            0x2033, 0x20ac, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2264, 0x2265,
        ]
        .iter()
        .filter_map(|&c| char::from_u32(c)),
    );
    v
}

/// `roboto.txt`: per face its metrics (1/1000 em, f32 `{:?}`), the advance of the missing
/// glyph, then one line per character: code point, glyph id, advance.
fn drawing_roboto(files: &mut Files) {
    use bimble_drawing::fonts::Face;
    let mut s = String::new();
    for bold in [false, true] {
        let f = Face::roboto(bold);
        writeln!(s, "face {}", f.name).unwrap();
        writeln!(s, "ascent {:?}", f.ascent).unwrap();
        writeln!(s, "descent {:?}", f.descent).unwrap();
        writeln!(s, "cap_height {:?}", f.cap_height).unwrap();
        writeln!(
            s,
            "bbox {:?} {:?} {:?} {:?}",
            f.bbox[0], f.bbox[1], f.bbox[2], f.bbox[3]
        )
        .unwrap();
        writeln!(s, "missing {:?}", f.advances([0u16])[0]).unwrap();
        let chars = roboto_chars();
        writeln!(s, "chars {}", chars.len()).unwrap();
        for c in chars {
            let g = f.glyph(c);
            writeln!(s, "{:04x} {} {:?}", c as u32, g, f.advances([g])[0]).unwrap();
        }
    }
    files.insert("drawing/roboto.txt".into(), s.into_bytes());
}

/// `style/`: the default graphic style, the pen of each line role at each standard scale, the
/// default plan template.
fn drawing_style(files: &mut Files) {
    use bimble_core::style::GraphicStyle;
    use bimble_drawing::plan::{Role, STANDARD_SCALES, role_pen};
    let style = GraphicStyle::default();
    files.insert(
        "drawing/style/default.json".into(),
        style.to_json().into_bytes(),
    );
    let mut s = String::new();
    for role in [
        Role::Cut,
        Role::Seen,
        Role::Above,
        Role::Hatch,
        Role::Axis,
        Role::Dimension,
        Role::Frame,
    ] {
        for n in STANDARD_SCALES {
            let p = role_pen(&style, role, n);
            writeln!(
                s,
                "{role:?} 1:{n} width {:?} color {} dash {:?}",
                p.width_mm,
                p.color.to_hex(),
                p.dash
            )
            .unwrap();
        }
    }
    files.insert("drawing/style/pens.txt".into(), s.into_bytes());
    files.insert(
        "drawing/style/template.json".into(),
        (bimble_drawing::template::PlanTemplate::default().to_json() + "\n").into_bytes(),
    );
}

/// `fmt.txt`: f64 bits (hex), then `{}`, `{:.1}`, `{:.3}` of the value, one per line.
fn drawing_fmt(files: &mut Files) {
    let mut vals: Vec<f64> = vec![
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        1.5,
        2.5,
        -2.5,
        0.25,
        0.0625,
        0.1875,
        1.0005,
        2.0005,
        0.0005,
        -0.0005,
        0.00049999999999999,
        0.0004,
        -0.0004,
        9.9995,
        99.9995,
        999.9995,
        0.125,
        0.375,
        1e-7,
        1e-300,
        5e-324,
        1e15,
        1e16,
        1e21,
        1e22,
        123456789.0,
        1.7976931348623157e308,
        0.1,
        0.2,
        0.3,
        297.0,
        210.0,
        841.0,
        594.0,
        1189.0,
        0.35,
        0.18,
        0.13,
        2.675,
        1.005,
    ];
    for k in -20..=20 {
        vals.push(k as f64 * 0.0005);
        vals.push(k as f64 * 0.05);
        vals.push(k as f64 / 3.0);
    }
    // A fixed pseudo-random spread (xorshift), paper-sized and any size.
    let mut x: u64 = 0x9e37_79b9_7f4a_7c15;
    for i in 0..1500 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        let v = if i % 5 != 0 {
            (x >> 11) as f64 / (1u64 << 53) as f64 * 2000.0 - 1000.0
        } else {
            f64::from_bits(x)
        };
        if v.is_finite() {
            vals.push(v);
        }
    }
    let mut s = String::new();
    for v in vals {
        writeln!(s, "{:016x} {} {:.1} {:.3}", v.to_bits(), v, v, v).unwrap();
    }
    files.insert("drawing/fmt.txt".into(), s.into_bytes());
}

/// A fixed GlobalId for the drawing model's views, sheets and annotations.
fn dgid(seed: &str) -> GlobalId {
    GlobalId::derived(format!("ccore drawing {seed}").as_bytes())
}

/// `model.json`: `frame2.json` with views and sheets, added by Commands.
fn drawing_model(m2: &Model) -> Model {
    use bimble_core::sheet::{Placed, Sheet as SheetDef, SheetText};
    use bimble_core::view::{
        Anchor, Annotation, Crop, DimAxis, Facing, LevelRef, Look, RangePlane, View, ViewKind,
        ViewRange, Visibility,
    };
    let mut m = m2.clone();
    let levels = m.levels();
    let key = |name: &str| {
        levels
            .iter()
            .find(|l| l.name == name)
            .expect("storey")
            .key
            .clone()
    };
    let ground = key("Rez-de-chaussée");
    let upper = key("Étage \"1\"");
    let view = |seed: &str, name: &str, kind: ViewKind| View {
        id: dgid(seed),
        name: name.into(),
        kind,
        visibility: Visibility::default(),
        references: Vec::new(),
        annotations: Vec::new(),
        true_north: false,
        scale: 100,
        crop: None,
    };
    let plane = |level: LevelRef, offset_mm: f64| RangePlane { level, offset_mm };
    let mut ranged = view(
        "ranged",
        "Plan RDC range",
        ViewKind::Plan {
            storey: ground.clone(),
            look: Look::Down,
            range: ViewRange {
                top: plane(LevelRef::Above, -100.0),
                cut: plane(LevelRef::Associated, 900.0),
                bottom: plane(LevelRef::Associated, -100.0),
                depth: plane(LevelRef::Associated, -700.0),
            },
        },
    );
    ranged.annotations = vec![
        Annotation::Dimension {
            id: dgid("dimension"),
            a: Anchor {
                at: [0.0, 0.0, 900.0],
                element: None,
            },
            b: Anchor {
                at: [6000.0, 0.0, 900.0],
                element: None,
            },
            offset_mm: 800.0,
            axis: DimAxis::Aligned,
            kind: None,
        },
        Annotation::Text {
            id: dgid("text"),
            text: "Note <RDC> & \"é\"".into(),
            at: [3000.0, -2500.0, 900.0],
            size_mm: 3.5,
            angle_deg: 15.0,
            kind: None,
        },
    ];
    let ceiling = view(
        "ceiling",
        "Ceiling RDC",
        ViewKind::Plan {
            storey: ground.clone(),
            look: Look::Up,
            range: ViewRange::default_for(Look::Up),
        },
    );
    let mut north = view(
        "north",
        "Section A",
        ViewKind::Section {
            origin: [3000.0, 2000.0],
            facing: Facing::North,
            depth_mm: 0.0,
        },
    );
    north.scale = 50;
    let east = view(
        "east",
        "Section B-B",
        ViewKind::Section {
            origin: [3000.0, 2000.0],
            facing: Facing::East,
            depth_mm: 5000.0,
        },
    );
    let mut cropped = view(
        "cropped",
        "Plan Etage cropped",
        ViewKind::Plan {
            storey: upper.clone(),
            look: Look::Down,
            range: ViewRange::default_for(Look::Down),
        },
    );
    cropped.crop = Some(Crop {
        min: [-500.0, -1500.0],
        max: [4500.0, 2500.0],
    });
    let mut hiding = view(
        "hiding",
        "Plan RDC no slabs",
        ViewKind::Plan {
            storey: ground.clone(),
            look: Look::Down,
            range: ViewRange::default_for(Look::Down),
        },
    );
    hiding.visibility.hidden_classes = vec!["IfcSlab".into()];
    let mut cmds: Vec<Command> = [ranged, ceiling, north, east, cropped, hiding]
        .into_iter()
        .map(Command::AddView)
        .collect();
    let mut s1 = SheetDef::new("S-101", "Plan and section");
    s1.id = dgid("sheet S-101");
    s1.paper = "A3".into();
    s1.placed = vec![
        Placed {
            id: dgid("placed plan"),
            view: dgid("ranged"),
            scale: 100,
            at_mm: [130.0, 140.0],
        },
        Placed {
            id: dgid("placed section"),
            view: dgid("north"),
            scale: 50,
            at_mm: [320.0, 120.0],
        },
    ];
    s1.texts = vec![SheetText {
        id: dgid("sheet text"),
        text: "Sheet note: see <S-102> & \"détails\"".into(),
        at_mm: [20.0, 25.0],
        size_mm: 5.0,
        angle_deg: 0.0,
        kind: None,
    }];
    s1.fields.insert("revision".into(), "B".into());
    let mut s2 = SheetDef::new("S-102", "3D placed");
    s2.id = dgid("sheet S-102");
    s2.paper = "A4".into();
    s2.portrait = true;
    s2.placed = vec![Placed {
        id: dgid("placed 3d"),
        view: GlobalId::derived(b"bimble view 3d"),
        scale: 100,
        at_mm: [105.0, 140.0],
    }];
    cmds.push(Command::AddSheet(s1));
    cmds.push(Command::AddSheet(s2));
    for c in cmds {
        c.apply(&mut m).expect("drawing model command");
    }
    m
}

/// A file name for a view: its place in the effective views, two digits.
fn view_slug(i: usize) -> String {
    format!("{i:02}")
}

/// `view/`: each effective view of `model.json`, minimal template (`<n>.min.svg`) and default
/// template (`<n>.svg`), with its `PlanInfo` (`<n>.info.txt`); `index.txt` names them; a view
/// that cannot be drawn (3D) gives its error there.
fn drawing_views(m: &Model, files: &mut Files) {
    use bimble_drawing::plan::{PlanOptions, plan, section_marks};
    use bimble_drawing::template::PlanTemplate;
    let scene = Scene::from_model(m);
    let views = bimble_core::view::effective(&m.views, &scene.levels());
    let marks = section_marks(&views);
    let mut index = String::new();
    for (i, v) in views.iter().enumerate() {
        let n = view_slug(i);
        writeln!(index, "{n} {} {}", v.id, json_str(&v.name)).unwrap();
        for (suffix, tpl) in [("min.", min_template()), ("", PlanTemplate::default())] {
            let o = PlanOptions {
                scale: v.scale,
                project: DRAWING_PROJECT.into(),
                date: DRAWING_DATE.into(),
                template: tpl.with_project(&m.project),
                view: Some(v.clone()),
                section_marks: marks.clone(),
                ..Default::default()
            };
            match plan(&scene, &o) {
                Ok((sheet, info)) => {
                    files.insert(
                        format!("drawing/view/{n}.{suffix}svg"),
                        sheet.to_svg().into_bytes(),
                    );
                    files.insert(
                        format!("drawing/view/{n}.{suffix}info.txt"),
                        format!("{info:#?}\n").into_bytes(),
                    );
                }
                Err(e) => writeln!(index, "{n} {suffix}error: {e}").unwrap(),
            }
        }
    }
    files.insert("drawing/view/index.txt".into(), index.into_bytes());
}

fn sheet_options(
    m: &Model,
    font: bimble_core::style::SheetFont,
) -> bimble_drawing::sheetdraw::SheetOptions {
    let mut style = bimble_core::style::GraphicStyle::default();
    style.paper.font = font;
    bimble_drawing::sheetdraw::SheetOptions {
        style,
        template: bimble_drawing::template::PlanTemplate::default().with_project(&m.project),
        project: DRAWING_PROJECT.into(),
        date: DRAWING_DATE.into(),
    }
}

/// `sheet/<number>.svg` and `.info.txt`: `sheetdraw::draw` of each sheet of `model.json`.
fn drawing_sheets(m: &Model, files: &mut Files) {
    use bimble_core::style::SheetFont;
    let scene = Scene::from_model(m);
    let views = bimble_core::view::effective(&m.views, &scene.levels());
    for def in m.sheets.values() {
        let (sheet, info) = bimble_drawing::sheetdraw::draw(
            &scene,
            def,
            &views,
            &sheet_options(m, SheetFont::Roboto),
        )
        .expect("sheet draws");
        files.insert(
            format!("drawing/sheet/{}.svg", def.number),
            sheet.to_svg().into_bytes(),
        );
        files.insert(
            format!("drawing/sheet/{}.info.txt", def.number),
            format!("{info:#?}\n").into_bytes(),
        );
    }
}

/// The page's content stream of a PDF `to_pdf` wrote (object 4), inflated if compressed.
fn pdf_content(pdf: &[u8]) -> Vec<u8> {
    use std::io::Read as _;
    let find = |hay: &[u8], needle: &[u8], from: usize| {
        hay[from..]
            .windows(needle.len())
            .position(|w| w == needle)
            .map(|i| i + from)
    };
    let obj = find(pdf, b"\n4 0 obj", 0).expect("content object");
    let start = find(pdf, b"stream\n", obj).expect("stream") + 7;
    let end = find(pdf, b"\nendstream", start).expect("endstream");
    let dict = &pdf[obj..start];
    let body = &pdf[start..end];
    if find(dict, b"/FlateDecode", 0).is_some() {
        let mut out = Vec::new();
        flate2::read::ZlibDecoder::new(body)
            .read_to_end(&mut out)
            .expect("content inflates");
        out
    } else {
        body.to_vec()
    }
}

/// The Rust build's version in a PDF's producer, replaced by as many `x` (same length, so the
/// offsets stay right).
fn mask_pdf_version(mut pdf: Vec<u8>) -> Vec<u8> {
    let v = format!("(Bimble {})", env!("CARGO_PKG_VERSION"));
    let masked = format!("(Bimble {})", "x".repeat(env!("CARGO_PKG_VERSION").len()));
    if let Some(i) = pdf.windows(v.len()).position(|w| w == v.as_bytes()) {
        pdf[i..i + v.len()].copy_from_slice(masked.as_bytes());
    }
    pdf
}

/// `pdf/<name>-<font>.pdf` and `.content`: `to_pdf` of the sample, the frame's full plan and
/// sheet S-101, in Roboto and in Helvetica (Roboto `.pdf`: the sample's only).
fn drawing_pdfs(frame: &Model, dm: &Model, files: &mut Files) {
    use bimble_core::style::SheetFont;
    use bimble_drawing::plan::plan;
    use bimble_drawing::template::PlanTemplate;
    let fscene = Scene::from_model(frame);
    let dscene = Scene::from_model(dm);
    let views = bimble_core::view::effective(&dm.views, &dscene.levels());
    let s101 = dm
        .sheets
        .values()
        .find(|s| s.number == "S-101")
        .expect("S-101");
    for (font, tag) in [
        (SheetFont::Roboto, "roboto"),
        (SheetFont::Helvetica, "helvetica"),
    ] {
        let mut o = plan_options(frame, ground_storey("frame"), PlanTemplate::default());
        o.style.paper.font = font;
        let (full, _) = plan(&fscene, &o).expect("full plan");
        let (sheet, _) =
            bimble_drawing::sheetdraw::draw(&dscene, s101, &views, &sheet_options(dm, font))
                .expect("sheet draws");
        for (name, s) in [
            ("sample", drawing_sample_sheet(font)),
            ("frame-full", full),
            ("S-101", sheet),
        ] {
            let pdf = mask_pdf_version(s.to_pdf());
            files.insert(
                format!("drawing/pdf/{name}-{tag}.content"),
                pdf_content(&pdf),
            );
            // A Roboto PDF embeds both font files (about 410 KB): the sample's is kept whole,
            // the others only by their content stream.
            if font == SheetFont::Helvetica || name == "sample" {
                files.insert(format!("drawing/pdf/{name}-{tag}.pdf"), pdf);
            }
        }
    }
}

// ---------------------------------------------------------------- history (ticket H1)

/// `history/`: a history folder written by `bimble_core::repo` from the frame, and what Rust
/// reads from it (`ccore/golden/README.md`, "history/"). Fixed authors and times.
fn history_files(frame: &Model, files: &mut Files) {
    use bimble_core::repo::{Merged, Prefer, Repo};
    use bimble_core::version::diff;
    const T0: i64 = 1_767_225_600; // 2026-01-01T00:00:00Z
    const ALICE: &str = "alice@example.org";
    const BOB: &str = "bob@example.org";
    let tmp = std::env::temp_dir().join(format!("ccore-golden-h1-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let hdir = tmp.join("frame.bimble-history");
    let r = Repo::init_dir(&hdir).expect("history init");
    let first_of = |m: &Model, class: IfcClass| {
        *m.elements
            .iter()
            .find(|(_, e)| e.class == class)
            .expect("the frame has this class")
            .0
    };
    let m1 = frame.clone();
    let c1 = r.commit(&m1, "first version", ALICE, T0).expect("c1");
    let mut m2 = m1.clone();
    let beam = first_of(&m2, IfcClass::IfcBeam);
    m2.elements.get_mut(&beam).unwrap().name = "Poutre \"P1\" — renommée".into();
    let c2 = r.commit(&m2, "rename a beam", BOB, T0 + 3600).expect("c2");
    let mut m3 = m2.clone();
    let column = first_of(&m3, IfcClass::IfcColumn);
    m3.elements.get_mut(&column).unwrap().placement.origin[0] += 250.0;
    let c3 = r
        .commit(&m3, "move a column", ALICE, T0 + 7200)
        .expect("c3");
    let mut m4 = m3.clone();
    let mut sheet = bimble_core::sheet::Sheet::new("S-101", "Plan niveau 1");
    sheet.id = GlobalId::derived(b"h1 sheet");
    m4.sheets.insert(sheet.id, sheet);
    let c4 = r.commit(&m4, "add a sheet", BOB, T0 + 10800).expect("c4");
    // The variant: from the moved column, placed on the map, two copies of one mesh (ADR 0042:
    // stored once, referred to by hash).
    r.create_branch("variant", Some(&c3)).expect("branch");
    let mut m5 = r.switch("variant").expect("switch variant");
    m5.georef = bimble_core::georef::Georef::from_frame(
        [0.866025403784, 0.5],
        [2_600_000_000.0, 1_200_000_000.0, 0.0],
    );
    m5.project
        .insert("address".into(), "Rue du Test 1, Lausanne".into());
    let shape = Generator::Mesh {
        positions: std::sync::Arc::new(vec![
            [0.0, 0.0, 0.0],
            [1000.0, 0.0, 0.0],
            [0.0, 1000.0, 0.0],
            [0.0, 0.0, 1100.0],
        ]),
        indices: std::sync::Arc::new(vec![0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]),
    };
    for (k, x) in [(1u8, 1000.0), (2, 3000.0)] {
        let mut e = m5.elements[&column].clone();
        e.id = GlobalId::derived(&[b'h', b'1', k]);
        e.class = IfcClass::IfcBuildingElementProxy;
        e.ifc_entity = Some("IfcRailing".into());
        e.name = format!("Garde-corps {k}");
        e.placement = Placement::at(x, 1000.0, 0.0);
        e.generator = shape.clone();
        e.openings.clear();
        m5.elements.insert(e.id, e);
    }
    let c5 = r
        .commit(&m5, "place on the map", ALICE, T0 + 14400)
        .expect("c5");
    r.switch("main").expect("switch main");
    let c6 = match r.merge("variant", Prefer::Neither, BOB, T0 + 18000) {
        Ok(Ok((Merged::Commit(h), _))) => h,
        other => panic!("merge: {:?}", other.map(|x| x.map(|(m, _)| m))),
    };
    let commits = [&c1, &c2, &c3, &c4, &c5, &c6];

    // The folder itself, every file.
    let mut on_disk = Vec::new();
    list(&hdir, "", &mut on_disk);
    on_disk.sort();
    for rel in &on_disk {
        let bytes = std::fs::read(hdir.join(rel)).expect("read history file");
        files.insert(format!("history/frame.bimble-history/{rel}"), bytes);
    }

    // log.txt: `log <rev>`, then per commit `<hash> <time> <author> <parents or -> <message>`.
    let mut log = String::new();
    for rev in ["HEAD", "variant", "HEAD~2", "main~1"] {
        writeln!(log, "log {rev}").unwrap();
        for (h, c) in r.log(rev, 100).expect("log") {
            let parents = if c.parents.is_empty() {
                "-".to_string()
            } else {
                c.parents.join(",")
            };
            writeln!(log, "{h} {} {} {parents} {}", c.time, c.author, c.message).unwrap();
        }
    }
    writeln!(log, "log HEAD limit 2").unwrap();
    for (h, c) in r.log("HEAD", 2).expect("log") {
        writeln!(log, "{h} {}", c.message).unwrap();
    }
    files.insert("history/log.txt".into(), log.into_bytes());

    // branches.txt: HEAD, then each branch and its commit.
    let mut br = String::new();
    match r.head().expect("head") {
        bimble_core::repo::Head::Branch(b) => writeln!(br, "HEAD ref: {b}").unwrap(),
        bimble_core::repo::Head::Detached(h) => writeln!(br, "HEAD {h}").unwrap(),
    }
    for (name, h) in r.branches().expect("branches") {
        writeln!(br, "{name} {h}").unwrap();
    }
    files.insert("history/branches.txt".into(), br.into_bytes());

    // resolve.txt: `<rev> -> <hash>` or `<rev> error <text>`.
    let tree1 = r.commit_info(&c1).expect("c1").tree;
    let mut res = String::new();
    let revs: Vec<String> = vec![
        "HEAD".into(),
        "".into(),
        "main".into(),
        "variant".into(),
        "HEAD~".into(),
        "HEAD~1".into(),
        "HEAD~3".into(),
        "HEAD~4".into(),
        "variant~2".into(),
        c2.clone(),
        c2[..4].into(),
        c2[..12].to_uppercase(),
        format!("{}~1", &c4[..8]),
        tree1[..10].into(),
        "nope".into(),
        "abc".into(),
        "HEAD~x".into(),
        "main~-1".into(),
        "../main".into(),
    ];
    for rev in &revs {
        match r.resolve(rev) {
            Ok(h) => writeln!(res, "{rev} -> {h}").unwrap(),
            Err(e) => writeln!(res, "{rev} error {e}").unwrap(),
        }
    }
    files.insert("history/resolve.txt".into(), res.into_bytes());

    // model_at_<n>.json and diff_<a>_<b>.txt (`version::diff`, one Change per Display).
    let models: Vec<Model> = commits
        .iter()
        .map(|h| r.model_at(h).expect("model_at"))
        .collect();
    for (i, m) in models.iter().enumerate() {
        files.insert(
            format!("history/model_at_{}.json", i + 1),
            file::to_string(m).into_bytes(),
        );
    }
    let changes_text = |changes: &[bimble_core::version::Change]| {
        let mut s = String::new();
        for c in changes {
            writeln!(s, "{c}").unwrap();
        }
        s
    };
    for (a, b) in [
        (1, 2),
        (2, 3),
        (3, 4),
        (4, 6),
        (3, 5),
        (5, 6),
        (1, 6),
        (6, 1),
        (2, 2),
    ] {
        let d = diff(&models[a - 1], &models[b - 1]);
        files.insert(
            format!("history/diff_{a}_{b}.txt"),
            changes_text(&d).into_bytes(),
        );
    }
    // diff_6_extra.txt: values that are objects and arrays (a generator of another kind, a mesh
    // point, the schedules, a sheet's paper); `extra.json` is the second model.
    let mut extra = models[5].clone();
    let rail1 = GlobalId::derived(&[b'h', b'1', 1]);
    let rail2 = GlobalId::derived(&[b'h', b'1', 2]);
    extra.elements.get_mut(&rail1).unwrap().generator = extra.elements[&column].generator.clone();
    if let Generator::Mesh { positions, .. } =
        &mut extra.elements.get_mut(&rail2).unwrap().generator
    {
        let mut p = (**positions).clone();
        p[1] = [1000.0, 0.5, -2.0];
        *positions = std::sync::Arc::new(p);
    }
    extra.schedules = golden_schedules(&extra);
    let sid = GlobalId::derived(b"h1 sheet");
    extra.sheets.get_mut(&sid).unwrap().paper = "A1".into();
    extra.sheets.get_mut(&sid).unwrap().portrait = true;
    files.insert(
        "history/diff_6_extra.txt".into(),
        changes_text(&diff(&models[5], &extra)).into_bytes(),
    );
    files.insert(
        "history/extra.json".into(),
        file::to_string(&extra).into_bytes(),
    );
    // status.txt: the working model against HEAD (a renamed storey and project, a removed
    // element, a changed schedule-free project field).
    let mut working = models[5].clone();
    working.project_name = "Concrete frame (working)".into();
    let storey = *working.storeys.keys().next().unwrap();
    working.storeys.get_mut(&storey).unwrap().name = "Sous-sol".into();
    working.elements.remove(&beam);
    working.project.remove("address");
    working
        .project
        .insert("client".into(), "Synthetic SA".into());
    let st = r.status(&working).expect("status");
    files.insert("history/status.txt".into(), changes_text(&st).into_bytes());
    // working.json: that working model, for the C side's status.
    files.insert(
        "history/working.json".into(),
        file::to_string(&working).into_bytes(),
    );

    // errors.txt: `<case> <Rust's text>`; the history folder path reads `<dir>`.
    let mut err = String::new();
    let masked = |e: &dyn std::fmt::Display, d: &Path| {
        e.to_string().replace(&d.display().to_string(), "<dir>")
    };
    // A damaged object: c2's object as plain JSON with byte 10 flipped (xor 1).
    let copy = tmp.join("damaged.bimble-history");
    copy_dir(&hdir, &copy);
    let rd = Repo::open_dir(&copy).expect("open copy");
    let obj = |d: &Path, h: &str| d.join("objects").join(&h[..2]).join(&h[2..]);
    let plain = {
        use std::io::Read;
        let gz = std::fs::read(obj(&copy, &c2)).unwrap();
        let mut p = Vec::new();
        flate2::read::GzDecoder::new(&gz[..])
            .read_to_end(&mut p)
            .unwrap();
        p
    };
    let mut flipped = plain.clone();
    flipped[10] ^= 1;
    std::fs::write(obj(&copy, &c2), &flipped).unwrap();
    writeln!(
        err,
        "damaged_commit {}",
        masked(&rd.commit_info(&c2).unwrap_err(), &copy)
    )
    .unwrap();
    writeln!(
        err,
        "damaged_log {}",
        masked(&rd.log("HEAD", 100).unwrap_err(), &copy)
    )
    .unwrap();
    // The same object plain and intact (an older history): read as it is.
    std::fs::write(obj(&copy, &c2), &plain).unwrap();
    writeln!(err, "plain_commit {}", rd.commit_info(&c2).unwrap().message).unwrap();
    // A missing element object.
    let beam_obj = r.tree(&r.commit_info(&c6).unwrap().tree).unwrap().elements[&beam].clone();
    std::fs::remove_file(obj(&copy, &beam_obj)).unwrap();
    writeln!(
        err,
        "missing {}",
        masked(&rd.model_at("HEAD").unwrap_err(), &copy)
    )
    .unwrap();
    for rev in ["nope", "HEAD~9"] {
        writeln!(
            err,
            "unknown {}",
            masked(&r.resolve(rev).unwrap_err(), &hdir)
        )
        .unwrap();
    }
    writeln!(
        err,
        "not_a_hash {}",
        masked(&r.commit_info("abc").unwrap_err(), &hdir)
    )
    .unwrap();
    std::fs::write(copy.join("format"), "bimble-history 2\n").unwrap();
    writeln!(
        err,
        "format {}",
        masked(&Repo::open_dir(&copy).err().unwrap(), &copy)
    )
    .unwrap();
    let none = tmp.join("none.bimble-history");
    writeln!(
        err,
        "no_repo {}",
        masked(&Repo::open_dir(&none).err().unwrap(), &none)
    )
    .unwrap();
    files.insert("history/errors.txt".into(), err.into_bytes());
    let _ = std::fs::remove_dir_all(&tmp);
}

fn copy_dir(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).unwrap();
    for e in std::fs::read_dir(from).unwrap().flatten() {
        let p = e.path();
        if p.is_dir() {
            copy_dir(&p, &to.join(e.file_name()));
        } else {
            std::fs::copy(&p, to.join(e.file_name())).unwrap();
        }
    }
// ---------------------------------------------------------------- reference plans (ticket L1)

/// `reference/`: `bimble_drawing::reference::read` (ADR 0021) of every file of
/// `reference/fixtures/` (hand-written inputs, kept as they are: `ccore/tools/ref_fixtures.py`)
/// and of D1's `drawing/pdf/*.pdf`, at 1:100, as `reference/<file>.txt`:
///   `error <text>`, or `note <text>` lines, `polylines <n> points <m>`, then one polyline per
///   line as `{:?}` (`[[x, y], …]`, mm). When those lines pass 256 KiB: `fnv <hex> bytes <n>`
///   of them (`fnv64`) and their first 20 lines.
/// `reference/scales.txt`: `lines.pdf` at other scales (`scale <{:?}>`, then the same text).
/// `reference/names.txt`: the dispatch by file name (`name <name>`, then the same text).
fn reference_files(dir: &Path, files: &mut Files) {
    use bimble_drawing::reference::read;
    let fixtures = dir.join("reference/fixtures");
    let mut names: Vec<String> = std::fs::read_dir(&fixtures)
        .map(|d| {
            d.filter_map(|e| e.ok())
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    let mut inputs: Vec<(String, Vec<u8>)> = Vec::new();
    for n in names {
        let bytes = std::fs::read(fixtures.join(&n)).unwrap_or_else(|e| panic!("{n}: {e}"));
        files.insert(format!("reference/fixtures/{n}"), bytes.clone());
        inputs.push((n, bytes));
    }
    for (rel, bytes) in files.iter() {
        if let Some(n) = rel.strip_prefix("drawing/pdf/")
            && n.ends_with(".pdf")
        {
            inputs.push((n.to_string(), bytes.clone()));
        }
    }
    for (n, bytes) in &inputs {
        files.insert(
            format!("reference/{n}.txt"),
            reference_text(&read(n, bytes, 100.0)).into_bytes(),
        );
    }
    let lines = inputs
        .iter()
        .find(|(n, _)| n == "lines.pdf")
        .map(|(_, b)| b.clone())
        .unwrap_or_default();
    let mut s = String::new();
    for scale in [
        1.0,
        0.5,
        50.0,
        333.3,
        100_000.0,
        1e-7,
        0.0,
        -1.0,
        100_000.5,
        f64::NAN,
        f64::INFINITY,
    ] {
        writeln!(s, "scale {scale:?}").unwrap();
        s += &reference_text(&read("lines.pdf", &lines, scale));
    }
    files.insert("reference/scales.txt".into(), s.into_bytes());
    let mut s = String::new();
    for (name, bytes) in [
        ("PLAN.PDF", lines.as_slice()),
        ("plan.Pdf", b"%PDF-1.4 garbage".as_slice()),
        ("plan.dwg", b"AC1032".as_slice()),
        ("plan.txt", b"".as_slice()),
        ("plan", b"".as_slice()),
    ] {
        writeln!(s, "name {name}").unwrap();
        s += &reference_text(&read(name, bytes, 100.0));
    }
    files.insert("reference/names.txt".into(), s.into_bytes());
}

fn reference_text(r: &Result<bimble_drawing::reference::Read, String>) -> String {
    let r = match r {
        Ok(r) => r,
        Err(e) => return format!("error {e}\n"),
    };
    let mut s = String::new();
    for n in &r.notes {
        writeln!(s, "note {n}").unwrap();
    }
    let points: usize = r.polylines.iter().map(Vec::len).sum();
    writeln!(s, "polylines {} points {points}", r.polylines.len()).unwrap();
    let mut body = String::new();
    for p in &r.polylines {
        writeln!(body, "{p:?}").unwrap();
    }
    if body.len() > 256 * 1024 {
        writeln!(
            s,
            "fnv {:016x} bytes {}",
            fnv64(body.as_bytes()),
            body.len()
        )
        .unwrap();
        for l in body.lines().take(20) {
            writeln!(s, "{l}").unwrap();
        }
    } else {
        s += &body;
    }
    s
}
