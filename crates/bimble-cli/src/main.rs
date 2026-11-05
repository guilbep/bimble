//! `bimble` command-line tool. Same commands the viewer and agents will use.

/// Measured on a 50 MB IFC: 10–20 % faster loading, freeing the parsed file 1.7× faster
/// (docs/journal/2026-11-05.md). Set for the binary only; the libraries leave the choice to users.
#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

use bimble_core::{Command, Generator, GlobalId, History, demo, file, version};
mod bench;

use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

#[derive(Parser)]
#[command(
    name = "bimble",
    version,
    about = "Open-source structural BIM. Run without arguments to open the viewer."
)]
struct Cli {
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// Open the 3D viewer (optionally with a .ifc or .bimble file)
    View {
        file: Option<PathBuf>,
        /// Graphic style file (JSON; see `bimble style`)
        #[arg(long)]
        style: Option<PathBuf>,
    },
    /// Storey plan at scale, as PDF or SVG (by the output's extension)
    Plan {
        file: PathBuf,
        /// Output: .pdf or .svg
        out: PathBuf,
        /// Storey name (see `bimble info`)
        #[arg(long)]
        storey: String,
        /// Scale 1:N, N from 1 to 5000
        #[arg(long, default_value_t = 100)]
        scale: u32,
        /// A4, A3, A2, A1 or A0; by default the smallest that fits
        #[arg(long)]
        paper: Option<String>,
        /// Height of the cut plane above the storey level, mm
        #[arg(long, default_value_t = 1200.0, allow_hyphen_values = true)]
        cut: f64,
        /// Graphic style file (colours, hatches, pens on paper; see `bimble style`)
        #[arg(long)]
        style: Option<PathBuf>,
        /// Project name in the title block (default: the file name)
        #[arg(long)]
        project: Option<String>,
    },
    /// Print the complete default graphic style, or check a style file and print it in full
    Style {
        /// Style file to check; without it, the default style is printed
        file: Option<PathBuf>,
    },
    /// Generate the demo concrete frame (2 storeys, 3×2 bays by default)
    Demo {
        out: PathBuf,
        #[arg(long, default_value_t = 3)]
        bays_x: usize,
        #[arg(long, default_value_t = 2)]
        bays_y: usize,
        #[arg(long, default_value_t = 2)]
        storeys: usize,
    },
    /// Summary of a .bimble or .ifc file: counts, materials, triangles, load time
    Info {
        file: PathBuf,
        /// Print one JSON object per element (GlobalId, class, volume, world bounding box in mm)
        #[arg(long)]
        elements_json: bool,
    },
    /// List elements (GlobalId, IFC class, name, generator)
    Ls {
        file: PathBuf,
        /// Filter by IFC class, e.g. IfcBeam
        #[arg(long)]
        class: Option<String>,
    },
    /// Change one generator parameter, e.g. `bimble set f.bimble <GlobalId> height=600 -o g.bimble`
    Set {
        file: PathBuf,
        id: GlobalId,
        /// key=value (numeric)
        assignment: String,
        #[arg(short, long)]
        out: Option<PathBuf>,
    },
    /// Element-level diff between two .bimble files
    Diff {
        a: PathBuf,
        b: PathBuf,
        #[arg(long)]
        json: bool,
    },
    /// Export to IFC4
    ExportIfc { file: PathBuf, out: PathBuf },
    /// Measure rendering speed on this machine (generated models of 1k–50k elements, plus files)
    Bench {
        /// Extra .ifc / .bimble files to benchmark
        files: Vec<PathBuf>,
        #[arg(long, default_value_t = 120)]
        frames: usize,
        #[arg(long, default_value_t = 1920)]
        width: u32,
        #[arg(long, default_value_t = 1080)]
        height: u32,
        /// MSAA samples (1 or 4; the viewer uses 4)
        #[arg(long, default_value_t = 4)]
        samples: u32,
        /// Only the smallest generated model
        #[arg(long)]
        quick: bool,
        /// Machine-readable output
        #[arg(long)]
        json: bool,
    },
    /// Render a .ifc or .bimble file to a PNG, without a window
    Render {
        file: PathBuf,
        out: PathBuf,
        #[arg(long, default_value_t = 1600)]
        width: u32,
        #[arg(long, default_value_t = 1000)]
        height: u32,
        /// Camera direction in degrees around the vertical axis
        #[arg(long, default_value_t = -55.0)]
        azimuth: f32,
        /// Camera elevation in degrees above the horizon
        #[arg(long, default_value_t = 28.0)]
        elevation: f32,
        /// Orthographic projection instead of perspective
        #[arg(long)]
        ortho: bool,
        /// Material families to hide, comma-separated (e.g. earth,glass)
        #[arg(long, value_delimiter = ',')]
        hide: Vec<String>,
        /// MSAA samples (1 or 4)
        #[arg(long, default_value_t = 4)]
        samples: u32,
        /// Graphic style file (JSON; see `bimble style`)
        #[arg(long)]
        style: Option<PathBuf>,
        /// Section box in model coordinates (mm): x0,y0,z0,x1,y1,z1. Only what is inside is
        /// drawn; cut solids show their cut surface.
        #[arg(long, value_delimiter = ',', allow_hyphen_values = true)]
        clip: Option<Vec<f64>>,
    },
}

fn run(cli: Cli) -> Result<(), String> {
    let e = |x: file::FileError| x.to_string();
    let Some(cmd) = cli.cmd else {
        return bimble_viewer::run(None, None);
    };
    match cmd {
        Cmd::View { file, style } => bimble_viewer::run(file, style)?,
        Cmd::Plan {
            file: path,
            out,
            storey,
            scale,
            paper,
            cut,
            style,
            project,
        } => {
            use bimble_drawing::plan::{PlanOptions, plan};
            use bimble_drawing::sheet::Paper;
            let paper = match paper {
                None => None,
                Some(p) => Some(
                    Paper::parse(&p)
                        .ok_or_else(|| format!("--paper {p:?}: A4, A3, A2, A1 or A0"))?,
                ),
            };
            let style = match style {
                Some(p) => bimble_render::GraphicStyle::load(&p)?,
                None => bimble_render::GraphicStyle::default(),
            };
            let ext = out
                .extension()
                .and_then(|x| x.to_str())
                .map(str::to_ascii_lowercase);
            if !matches!(ext.as_deref(), Some("pdf" | "svg")) {
                return Err("the output must end in .pdf or .svg".into());
            }
            let scene = load_scene(&path)?;
            let project = project.unwrap_or_else(|| {
                path.file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default()
            });
            let opts = PlanOptions {
                storey,
                cut_above_mm: cut,
                scale,
                paper,
                style,
                project,
                date: bimble_drawing::today(),
            };
            let (sheet, info) = plan(&scene, &opts)?;
            let bytes = if ext.as_deref() == Some("pdf") {
                sheet.to_pdf()
            } else {
                sheet.to_svg().into_bytes()
            };
            file::write_atomic(&out, &bytes).map_err(|e| format!("{}: {e}", out.display()))?;
            println!(
                "plan 1:{} on {}{}: {} cut elements, {} lines below, {} above → {}",
                scale,
                info.paper.name(),
                if info.portrait { " portrait" } else { "" },
                info.cut_elements,
                info.projection_lines,
                info.above_lines,
                out.display()
            );
        }
        Cmd::Style { file } => {
            let s = match file {
                Some(p) => bimble_render::GraphicStyle::load(&p)?,
                None => bimble_render::GraphicStyle::default(),
            };
            print!("{}", s.to_json());
        }
        Cmd::Demo {
            out,
            bays_x,
            bays_y,
            storeys,
        } => {
            let spec = demo::FrameSpec {
                bays_x,
                bays_y,
                storeys,
                ..Default::default()
            };
            let m = demo::concrete_frame(&spec);
            file::save(&m, &out).map_err(e)?;
            println!("{} elements → {}", m.elements.len(), out.display());
        }
        Cmd::Info {
            file: path,
            elements_json,
        } => {
            let t = Instant::now();
            let scene = load_scene(&path)?;
            let load = t.elapsed();
            if elements_json {
                print_elements_json(&scene);
                return Ok(());
            }
            if !is_ifc(&path) {
                let m = file::load(&path).map_err(e)?;
                println!("project   {} [{}]", m.project_name, m.project_id);
                println!("hash      {}", version::model_hash(&m));
            }
            let mut classes: std::collections::BTreeMap<&str, usize> = Default::default();
            let mut cats: std::collections::BTreeMap<&str, usize> = Default::default();
            let mut storeys: std::collections::BTreeSet<&str> = Default::default();
            for el in &scene.elements {
                *classes.entry(&el.class).or_default() += 1;
                *cats.entry(el.category.label()).or_default() += 1;
                storeys.insert(&el.storey);
            }
            println!("elements  {}", scene.elements.len());
            println!("storeys   {}", storeys.len());
            for st in &scene.storeys {
                println!("  {:>10.3} m  {}", st.elevation_mm / 1000.0, st.name);
            }
            println!("classes   {}", fmt_counts(&classes));
            println!("materials {}", fmt_counts(&cats));
            let edges: usize = scene.elements.iter().map(|e| e.mesh.edges.len()).sum();
            println!(
                "meshes    {} triangles, {edges} edges",
                scene.triangle_count()
            );
            println!("loaded in {load:.1?}");
            for w in &scene.warnings {
                println!("warning   {w}");
            }
        }
        Cmd::Ls { file: path, class } => {
            use std::io::Write;
            let m = file::load(&path).map_err(e)?;
            let mut out = std::io::stdout().lock();
            for el in m.elements.values() {
                if class.as_deref().is_some_and(|c| c != el.class.name()) {
                    continue;
                }
                let params = serde_json::to_string(&el.generator).unwrap();
                let line = format!(
                    "{}  {:<10} {:<10} {params}",
                    el.id,
                    el.class.name(),
                    el.name
                );
                // Stop quietly if the reader closed the pipe (e.g. `| head`).
                if writeln!(out, "{line}").is_err() {
                    break;
                }
            }
        }
        Cmd::Set {
            file: path,
            id,
            assignment,
            out,
        } => {
            let mut m = file::load(&path).map_err(e)?;
            let (key, val) = assignment.split_once('=').ok_or("expected key=value")?;
            let val: f64 = val
                .trim()
                .parse()
                .map_err(|_| format!("not a number: {val}"))?;
            let el = m.elements.get(&id).ok_or(format!("no element {id}"))?;
            let mut g = serde_json::to_value(&el.generator).unwrap();
            let shown = g.to_string();
            let slot = g
                .get_mut(key.trim())
                .filter(|v| v.is_number())
                .ok_or_else(|| {
                    format!(
                        "{} has no numeric parameter {key:?} (params: {shown})",
                        el.generator.name()
                    )
                })?;
            *slot = val.into();
            let generator: Generator = serde_json::from_value(g).map_err(|x| x.to_string())?;
            History::default()
                .apply(&mut m, vec![Command::SetGenerator { id, generator }])
                .map_err(|x| x.to_string())?;
            let out = out.unwrap_or(path);
            file::save(&m, &out).map_err(e)?;
            println!("{} → {}", assignment, out.display());
        }
        Cmd::Diff { a, b, json } => {
            let (ma, mb) = (file::load(&a).map_err(e)?, file::load(&b).map_err(e)?);
            let d = version::diff(&ma, &mb);
            if json {
                println!("{}", serde_json::to_string_pretty(&d).unwrap());
            } else if d.is_empty() {
                println!("no changes");
            } else {
                for c in &d {
                    println!("{c}");
                }
            }
        }
        Cmd::Render {
            file: path,
            out,
            width,
            height,
            azimuth,
            elevation,
            ortho,
            hide,
            samples,
            style,
            clip,
        } => {
            use bimble_core::MaterialCategory;
            use bimble_render::{Camera, DrawOptions, GraphicStyle, Headless, Renderer, Target};
            let style = match style {
                Some(p) => GraphicStyle::load(&p)?,
                None => GraphicStyle::default(),
            };
            let hidden: Vec<MaterialCategory> = hide
                .iter()
                .map(|h| {
                    MaterialCategory::ALL
                        .into_iter()
                        .find(|c| c.label().eq_ignore_ascii_case(h.trim()))
                        .ok_or_else(|| format!("unknown material family {h:?}"))
                })
                .collect::<Result<_, _>>()?;
            if !matches!(samples, 1 | 4) {
                return Err("--samples must be 1 or 4".into());
            }
            let t0 = Instant::now();
            let scene = load_scene(&path)?;
            let t_load = t0.elapsed();
            let clip = match clip.as_deref() {
                None => None,
                Some([x0, y0, z0, x1, y1, z1]) => {
                    let (a, b) = ([*x0, *y0, *z0], [*x1, *y1, *z1]);
                    if a.iter().chain(&b).any(|v| !v.is_finite()) {
                        return Err("--clip: values must be numbers".into());
                    }
                    // Model coordinates to scene coordinates (relative to the scene origin).
                    let o = scene.origin_mm;
                    let rel = |p: [f64; 3]| {
                        glam::Vec3::new(
                            (p[0] - o[0]) as f32,
                            (p[1] - o[1]) as f32,
                            (p[2] - o[2]) as f32,
                        )
                    };
                    let (a, b) = (rel(a), rel(b));
                    Some(bimble_render::ClipBox {
                        min: a.min(b),
                        max: a.max(b),
                    })
                }
                Some(_) => return Err("--clip needs six numbers: x0,y0,z0,x1,y1,z1".into()),
            };
            let gpu = Headless::new().map_err(|x| x.to_string())?;
            let max = gpu.max_target_size();
            if width == 0 || height == 0 || width > max || height > max {
                return Err(format!(
                    "--width and --height must be between 1 and {max} on this GPU"
                ));
            }
            let r = Renderer::new(&gpu.device, samples);
            let visible = bimble_render::hidden_by_category(&hidden);
            let gs = r.upload_with(&gpu.device, &scene, &visible, clip.is_some());
            let target = Target::new(&gpu.device, (width, height), samples);
            let mut cam = Camera {
                yaw: azimuth.to_radians(),
                pitch: elevation.to_radians(),
                orthographic: ortho,
                ..Default::default()
            };
            let mut shown = scene.clone();
            shown.elements.retain(|e| !hidden.contains(&e.category));
            let aspect = width as f32 / height as f32;
            match (&clip, shown.bounds()) {
                // Frame what is left: the part of the model inside the box.
                (Some(c), Some((lo, hi))) => {
                    let lo = c.min.max(glam::Vec3::from(lo));
                    let hi = c.max.min(glam::Vec3::from(hi));
                    if lo.cmple(hi).all() {
                        cam.fit_box(lo, hi, aspect);
                    } else {
                        cam.fit(&shown, aspect);
                    }
                }
                _ => cam.fit(&shown, aspect),
            }
            let t1 = Instant::now();
            r.render(
                &gpu.device,
                &gpu.queue,
                &target,
                &gs,
                &cam,
                &DrawOptions {
                    style,
                    clip,
                    ..Default::default()
                },
            );
            let px = gpu.read_rgba(&target).map_err(|x| x.to_string())?;
            let t_render = t1.elapsed();
            bimble_render::write_png(&out, width, height, &px).map_err(|x| x.to_string())?;
            println!(
                "{} elements, {} triangles; load {t_load:.1?}, render+readback {t_render:.1?} on {} ({:?}) → {}",
                shown.elements.len(),
                gs.triangles(),
                gpu.adapter_info.name,
                gpu.adapter_info.backend,
                out.display()
            );
        }
        Cmd::Bench {
            files,
            frames,
            width,
            height,
            samples,
            quick,
            json,
        } => {
            if !matches!(samples, 1 | 4) {
                return Err("--samples must be 1 or 4".into());
            }
            bench::run(bench::BenchArgs {
                files,
                frames,
                width,
                height,
                samples,
                quick,
                json,
            })?;
        }
        Cmd::ExportIfc { file: path, out } => {
            let m = file::load(&path).map_err(e)?;
            let name = out
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or("model.ifc");
            let ifc = bimble_ifc::export(&m, name, &now_iso());
            std::fs::write(&out, ifc).map_err(|x| x.to_string())?;
            println!("{} elements → {}", m.elements.len(), out.display());
        }
    }
    Ok(())
}

fn is_ifc(path: &std::path::Path) -> bool {
    path.extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"))
}

/// Load a .ifc or .bimble file as a render-ready scene.
fn load_scene(path: &std::path::Path) -> Result<bimble_geom::Scene, String> {
    bimble_viewer::load_scene(path)
}

fn fmt_counts(m: &std::collections::BTreeMap<&str, usize>) -> String {
    let mut v: Vec<_> = m.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
    v.iter()
        .map(|(k, n)| format!("{k} {n}"))
        .collect::<Vec<_>>()
        .join(", ")
}

fn print_elements_json(scene: &bimble_geom::Scene) {
    use std::io::Write;
    let mut out = std::io::stdout().lock();
    let o = scene.origin_mm;
    for el in &scene.elements {
        let mut lo = [f64::MAX; 3];
        let mut hi = [f64::MIN; 3];
        for p in &el.mesh.positions {
            for k in 0..3 {
                let w = p[k] as f64 + o[k];
                lo[k] = lo[k].min(w);
                hi[k] = hi[k].max(w);
            }
        }
        let j = serde_json::json!({
            "global_id": el.global_id, "class": el.class, "name": el.name, "storey": el.storey,
            "material": el.material, "category": el.category.label(),
            "triangles": el.mesh.triangle_count(), "volume_mm3": el.mesh.volume(),
            "bbox_min_mm": lo, "bbox_max_mm": hi,
        });
        if writeln!(out, "{j}").is_err() {
            break;
        }
    }
}

/// UTC timestamp without pulling in a date crate.
fn now_iso() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let (days, rem) = ((secs / 86400) as i64, secs % 86400);
    // Civil-from-days (Howard Hinnant's algorithm).
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(mo <= 2);
    format!(
        "{y:04}-{mo:02}-{d:02}T{:02}:{:02}:{:02}",
        rem / 3600,
        rem % 3600 / 60,
        rem % 60
    )
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(msg) => {
            eprintln!("error: {msg}");
            ExitCode::FAILURE
        }
    }
}
