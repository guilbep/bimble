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
        _ => {
            eprintln!("usage: ccore_golden [--generate-frame | --generate-frame2 | --check]");
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
    files.insert("query.txt".into(), query_txt(&m).into_bytes());
    verbs_files(&m, &known, &mut files);
    ifc_files(&m, &known, &mut files);
    frame2_files(dir, &mut files);
    files
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

fn ifc_files(m: &Model, known: &BTreeSet<GlobalId>, files: &mut Files) {
    let ifc = bimble_ifc::export(m, "frame.ifc", IFC_TIMESTAMP);
    // The header names the Rust build's version; the golden keeps a placeholder.
    let ifc = ifc.replacen(
        &format!("'bimble {}'", env!("CARGO_PKG_VERSION")),
        "'bimble <version>'",
        1,
    );
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
    files.insert("frame2/query.txt".into(), frame2_query_txt(&m).into_bytes());
}
