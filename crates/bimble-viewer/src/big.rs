//! Models too large for the machine (ADR 0033): what a model will cost before it is read, and
//! what to offer when it does not fit: one storey, some IFC classes, a material.

use bimble_geom::Scene;
use bimble_ifc::read::{Counted, Parsed, stored_triangles};
use std::time::Duration;
use web_time::Instant;

/// What loading a file gave: a scene to show, or a question first.
// Moved once per load: boxing would buy nothing.
#[allow(clippy::large_enum_variant)]
pub enum Outcome {
    /// The scene, the model behind it (a .bimble file), the load time, and what part of the
    /// file it shows when not all of it.
    Ready {
        scene: Scene,
        model: Option<bimble_core::Model>,
        time: Duration,
        partial: Option<String>,
        /// What to tell about the load (an IFC import's report), shown until closed.
        report: Vec<String>,
    },
    Ask(Box<Ask>),
}

impl Outcome {
    pub fn ready(scene: Scene, model: Option<bimble_core::Model>, time: Duration) -> Outcome {
        Outcome::Ready {
            scene,
            model,
            time,
            partial: None,
            report: Vec::new(),
        }
    }
}

/// A model too large for this machine, waiting for the user.
#[allow(clippy::large_enum_variant)]
pub enum Ask {
    /// Reading the file alone would take more than the budget: nothing read yet.
    Read {
        source: Source,
        cost: Cost,
        budget: u64,
    },
    /// Read; showing all of it would take more than the budget.
    Choose(Choose),
}

pub struct Choose {
    pub parsed: Parsed,
    pub census: Vec<Counted>,
    pub cost: Cost,
    pub budget: u64,
    pub split: Split,
    pub chosen: Vec<String>,
}

impl Choose {
    /// What all of it holds in memory, in triangles.
    pub fn all_triangles(&self) -> usize {
        stored_triangles(&self.census)
    }

    /// Triangles drawn for all of it (every copy counted).
    pub fn drawn_triangles(&self) -> usize {
        self.census.iter().map(|c| c.triangles).sum()
    }

    pub fn chosen_triangles(&self) -> usize {
        chosen_triangles(&self.census, self.split, &self.chosen)
    }

    /// What the chosen part is, for the status bar.
    pub fn label(&self) -> String {
        let n = groups(&self.census, self.split).len();
        let what = match self.split {
            Split::Storey => "storeys",
            Split::Class => "IFC classes",
            Split::Material => "materials",
        };
        if self.chosen.len() == 1 {
            format!("{} (1 of {n} {what})", self.chosen[0])
        } else {
            format!("{} of {n} {what}", self.chosen.len())
        }
    }

    /// Mesh the chosen groups (all of them with `everything`) and free the parsed file.
    /// "Anyway" overrides the memory budget, not the guard against hostile files: the cap is
    /// what the census counted for the choice (each element itself capped while counted).
    pub fn scene(self, everything: bool) -> Outcome {
        let t = Instant::now();
        let partial = (!everything).then(|| self.label());
        let keep: Vec<bool> = self
            .census
            .iter()
            .map(|c| everything || self.chosen.iter().any(|g| g == self.split.key(c)))
            .collect();
        let cap = stored_triangles(
            (self.census.iter().zip(&keep))
                .filter(|(_, k)| **k)
                .map(|(c, _)| c),
        );
        let cap = cap.min(self.parsed.limits().max_triangles);
        let (scene, _) = self.parsed.scene(|i| keep[i], cap);
        drop(self.parsed);
        Outcome::Ready {
            scene,
            model: None,
            time: t.elapsed(),
            partial,
            report: Vec::new(),
        }
    }
}

/// Where an IFC comes from: its bytes (a browser), or a file read a window at a time.
pub enum Source {
    Bytes(Vec<u8>),
    #[cfg_attr(target_arch = "wasm32", allow(dead_code))]
    File(std::path::PathBuf),
}

/// What reading holds of the text at a time when it streams a file: one window.
const STREAM_WINDOW: u64 = 64 << 20;

/// Load an IFC within `budget` (ADR 0033): straight through when it fits (the usual case);
/// otherwise stop and ask, before reading when reading alone would not fit (unless
/// `read_anyway`), after reading when showing all of it would not.
pub fn load_ifc(source: Source, budget: u64, read_anyway: bool) -> Result<Outcome, String> {
    let t = Instant::now();
    let cost = match &source {
        Source::Bytes(b) => Cost {
            file: b.len() as u64,
            displayed: bimble_ifc::read::displayed_bytes(b),
        },
        // A streamed file is never whole in memory: one window of its text at a time.
        Source::File(p) => Cost {
            file: std::fs::metadata(p)
                .map_err(|e| e.to_string())?
                .len()
                .min(STREAM_WINDOW),
            displayed: bimble_ifc::read::displayed_bytes_of_file(p).map_err(|e| e.to_string())?,
        },
    };
    if !read_anyway && cost.read() > budget {
        return Ok(Outcome::Ask(Box::new(Ask::Read {
            source,
            cost,
            budget,
        })));
    }
    let limits = bimble_ifc::read::Limits::for_displayed(cost.displayed);
    let parsed = match source {
        Source::Bytes(b) => bimble_ifc::read::parse_bytes_with(b, limits),
        Source::File(p) => bimble_ifc::read::parse_file_with(&p, limits),
    }
    .map_err(|e| e.to_string())?;
    // Mesh what fits; only a model that does not fit all pays for a census. Never past the
    // guard against hostile expansion.
    let cap = cost.affordable_triangles(budget).min(limits.max_triangles);
    let (scene, not_shown) = parsed.scene(|_| true, cap);
    if not_shown == 0 {
        // Freed before the scene is drawn: it would otherwise overlap the GPU upload.
        drop(parsed);
        return Ok(Outcome::ready(scene, None, t.elapsed()));
    }
    drop(scene);
    let census = parsed.census();
    Ok(Outcome::Ask(Box::new(Ask::Choose(Choose {
        parsed,
        census,
        cost,
        budget,
        split: Split::Storey,
        chosen: Vec::new(),
    }))))
}

/// What a new model may take: `share` of the machine's memory (a setting, 80 % by default; 8 GB
/// assumed where it can't be read), less what Bimble already holds — the model shown and its
/// links stay alive while the next one loads. In a browser, all a tab can address (4 GB).
pub fn memory_budget(share: f64) -> u64 {
    let limit = if cfg!(target_arch = "wasm32") {
        4_000_000_000
    } else {
        (total_memory().unwrap_or(8_000_000_000) as f64 * share) as u64
    };
    limit.saturating_sub(in_use())
}

/// Memory this process holds now.
fn in_use() -> u64 {
    #[cfg(target_arch = "wasm32")]
    {
        (core::arch::wasm32::memory_size(0) * 65_536) as u64
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        memory_stats::memory_stats().map_or(0, |m| m.physical_mem as u64)
    }
}

/// The machine's memory, if it can be read.
#[cfg(target_os = "macos")]
pub fn total_memory() -> Option<u64> {
    let mut v: u64 = 0;
    let mut len = std::mem::size_of::<u64>();
    // SAFETY: hw.memsize is a u64; `v` and `len` describe that buffer exactly.
    let r = unsafe {
        libc::sysctlbyname(
            c"hw.memsize".as_ptr(),
            (&raw mut v).cast(),
            &mut len,
            std::ptr::null_mut(),
            0,
        )
    };
    (r == 0 && v > 0).then_some(v)
}

#[cfg(all(unix, not(target_os = "macos")))]
pub fn total_memory() -> Option<u64> {
    let s = std::fs::read_to_string("/proc/meminfo").ok()?;
    let kb: u64 = s
        .lines()
        .find_map(|l| l.strip_prefix("MemTotal:"))?
        .trim()
        .trim_end_matches("kB")
        .trim()
        .parse()
        .ok()?;
    Some(kb * 1024)
}

#[cfg(windows)]
pub fn total_memory() -> Option<u64> {
    use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
    // SAFETY: a zeroed MEMORYSTATUSEX with dwLength set is what the call expects.
    unsafe {
        let mut m: MEMORYSTATUSEX = std::mem::zeroed();
        m.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
        (GlobalMemoryStatusEx(&mut m) != 0).then_some(m.ullTotalPhys)
    }
}

#[cfg(not(any(unix, windows)))]
pub fn total_memory() -> Option<u64> {
    None
}

// Peak memory, fitted on 17 IFC files of 0.2–872 MB (peak memory footprint of `bimble
// render`, Apple unified memory, so GPU buffers included; `scripts/calibrate_memory.sh`
// measures and fits again). Refitted after shapes were meshed once and drawn instanced
// (ADR 0034): "triangles" are what is stored — own triangles, each shared shape once, one per
// copy. Refitted again after the compact parse (24-byte values, exact-size lists). Within 23 %
// on every file over 15 MB, before the margin.
/// Reading: the text plus ~5 bytes per byte of displayed IFC (an upper bound: `info` peaks,
/// which include meshing, reach 3.8–4.8).
const READ_PER_DISPLAYED: f64 = 5.0;
const BASE: f64 = 495e6;
/// Parsed entities kept while meshing.
const PARSED_PER_DISPLAYED: f64 = 3.38;
/// Mesh, edges and GPU buffers, per stored triangle.
const PER_TRIANGLE: f64 = 219.0;
/// The fit's worst under-estimate was 23 %.
const MARGIN: f64 = 1.25;

/// What a file costs before meshing: `file` bytes, of which `displayed` are parsed.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Cost {
    pub file: u64,
    pub displayed: u64,
}

impl Cost {
    /// Peak while reading the file.
    pub fn read(&self) -> u64 {
        (MARGIN * (self.file as f64 + READ_PER_DISPLAYED * self.displayed as f64)) as u64
    }

    /// Peak to read and show `triangles`.
    pub fn shown(&self, triangles: usize) -> u64 {
        let mesh =
            BASE + PARSED_PER_DISPLAYED * self.displayed as f64 + PER_TRIANGLE * triangles as f64;
        self.read().max((MARGIN * mesh) as u64)
    }

    /// Most triangles that can be shown within `budget`.
    pub fn affordable_triangles(&self, budget: u64) -> usize {
        let left = budget as f64 / MARGIN - BASE - PARSED_PER_DISPLAYED * self.displayed as f64;
        (left / PER_TRIANGLE).max(0.0) as usize
    }
}

/// How a large model is split for choosing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Split {
    Storey,
    Class,
    Material,
}

impl Split {
    pub const ALL: [Split; 3] = [Split::Storey, Split::Class, Split::Material];

    pub fn label(self) -> &'static str {
        match self {
            Split::Storey => "Storey",
            Split::Class => "IFC class",
            Split::Material => "Material",
        }
    }

    /// The group an element belongs to.
    pub fn key(self, c: &Counted) -> &str {
        let s = match self {
            Split::Storey => &c.info.storey,
            Split::Class => &c.info.class,
            Split::Material => &c.info.material,
        };
        if s.is_empty() { "(none)" } else { s }
    }
}

/// One group of a split: its name, elements and triangles.
#[derive(Clone, Debug, PartialEq)]
pub struct Group {
    pub name: String,
    pub elements: usize,
    pub triangles: usize,
}

/// The groups of a split, by name (storeys in file order would be nicer; names sort well
/// enough for levels named by number).
pub fn groups(census: &[Counted], split: Split) -> Vec<Group> {
    let mut m: std::collections::BTreeMap<&str, Vec<&Counted>> = Default::default();
    for c in census {
        m.entry(split.key(c)).or_default().push(c);
    }
    m.into_iter()
        .map(|(name, cs)| Group {
            name: name.to_owned(),
            elements: cs.len(),
            triangles: stored_triangles(cs),
        })
        .collect()
}

/// Triangles of the elements in the chosen groups.
/// What the chosen groups hold in memory, in triangles (`stored_triangles`).
pub fn chosen_triangles(census: &[Counted], split: Split, chosen: &[String]) -> usize {
    stored_triangles(
        census
            .iter()
            .filter(|c| chosen.iter().any(|g| g == split.key(c))),
    )
}

pub fn gb(bytes: u64) -> String {
    format!("{:.1} GB", bytes as f64 / 1e9)
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::MaterialCategory;
    use bimble_geom::ElementInfo;

    fn counted(storey: &str, class: &str, triangles: usize) -> Counted {
        Counted {
            info: ElementInfo {
                global_id: String::new(),
                class: class.into(),
                name: String::new(),
                storey: storey.into(),
                material: String::new(),
                category: MaterialCategory::Other,
            },
            triangles,
            shape: None,
        }
    }

    fn demo_ifc() -> Vec<u8> {
        let m = bimble_core::demo::concrete_frame(&bimble_core::demo::FrameSpec::default());
        bimble_ifc::export(&m, "t.ifc", "t").into_bytes()
    }

    /// A model that fits loads straight through; one that does not asks, with its census;
    /// one whose reading alone would not fit asks before reading.
    #[test]
    fn a_model_is_loaded_or_asked_about_by_its_cost() {
        let ifc = demo_ifc();
        assert!(matches!(
            load_ifc(Source::Bytes(ifc.clone()), 100_000_000_000, false),
            Ok(Outcome::Ready { partial: None, .. })
        ));
        let cost = Cost {
            file: ifc.len() as u64,
            displayed: bimble_ifc::read::displayed_bytes(&ifc),
        };
        // Room to read, and for the base, but not for the triangles.
        let tight = cost.shown(0).max(cost.read());
        let Ok(Outcome::Ask(a)) = load_ifc(Source::Bytes(ifc.clone()), tight, false) else {
            panic!("expected a question")
        };
        let Ask::Choose(mut c) = *a else {
            panic!("expected a choice")
        };
        assert!(c.all_triangles() > 0);
        let first = groups(&c.census, Split::Storey)[0].name.clone();
        c.chosen = vec![first];
        let want = c.chosen_triangles();
        let Outcome::Ready { scene, partial, .. } = c.scene(false) else {
            panic!()
        };
        assert!(partial.is_some_and(|p| p.contains("of")));
        let got: usize = scene.elements.iter().map(|e| e.triangle_count()).sum();
        assert_eq!(got, want);
        assert!(matches!(
            load_ifc(Source::Bytes(ifc), cost.read() - 1, false),
            Ok(Outcome::Ask(a)) if matches!(*a, Ask::Read { .. })
        ));
    }

    /// The fit's own numbers: a 200 MB file (78 MB displayed, 0.38 M triangles stored, 8.8 M
    /// drawn) peaked at 0.88 GB; with the margin the estimate must not be under it.
    #[test]
    fn the_estimate_covers_a_measured_model() {
        let c = Cost {
            file: 200_000_000,
            displayed: 78_000_000,
        };
        let est = c.shown(376_901);
        assert!((880_000_000..1_500_000_000).contains(&est), "{est}");
        // What fits in a budget is what the estimate says fits.
        let n = c.affordable_triangles(est);
        assert!(c.shown(n) <= est && c.shown(n + 1000) > est);
    }

    #[test]
    fn a_storey_costs_its_triangles() {
        let census = [
            counted("Level 1", "IfcWall", 4_000_000),
            counted("Level 1", "IfcSlab", 1_000_000),
            counted("Level 2", "IfcWall", 3_000_000),
            counted("", "IfcBeam", 10),
        ];
        let g = groups(&census, Split::Storey);
        let names: Vec<&str> = g.iter().map(|g| g.name.as_str()).collect();
        assert_eq!(names, ["(none)", "Level 1", "Level 2"]);
        assert_eq!((g[1].elements, g[1].triangles), (2, 5_000_000));
        let two = chosen_triangles(&census, Split::Storey, &["Level 2".into()]);
        assert_eq!(two, 3_000_000);
        let walls = chosen_triangles(&census, Split::Class, &["IfcWall".into()]);
        assert_eq!(walls, 7_000_000);
        let c = Cost {
            file: 100_000_000,
            displayed: 50_000_000,
        };
        assert!(c.shown(two) < c.shown(walls));
    }
}
