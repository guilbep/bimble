//! `bimble` command-line tool. Same commands the viewer and agents will use.

/// Measured on a 50 MB IFC: 10–20 % faster loading, freeing the parsed file 1.7× faster
/// (docs/journal/2026-11-05.md). Set for the binary only; the libraries leave the choice to users.
#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

use bimble_core::{Command, Generator, GlobalId, History, demo, file, version};
mod bench;
mod codec_bench;

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
enum HistoryCmd {
    /// Start a history for a .bimble model
    Init { model: PathBuf },
    /// Record the model as it is now
    Commit {
        model: PathBuf,
        #[arg(short, long)]
        message: String,
        /// Default: $BIMBLE_AUTHOR, else $USER
        #[arg(long)]
        author: Option<String>,
    },
    /// What changed since the last commit
    Status { model: PathBuf },
    /// Commits, newest first
    Log {
        model: PathBuf,
        /// Branch, commit (or prefix) or HEAD~n
        #[arg(default_value = "HEAD")]
        rev: String,
        #[arg(short = 'n', long, default_value_t = 20)]
        limit: usize,
    },
    /// Changes between two revisions; with one, between it and the model as it is now
    Diff {
        model: PathBuf,
        a: String,
        b: Option<String>,
        #[arg(long)]
        json: bool,
    },
    /// List branches, or create one at HEAD (or at --at)
    Branch {
        model: PathBuf,
        name: Option<String>,
        #[arg(long)]
        at: Option<String>,
    },
    /// Switch to a branch or commit: the model file is replaced by its state there
    Switch {
        model: PathBuf,
        rev: String,
        /// Even when the model has changes not committed (they are lost)
        #[arg(long)]
        force: bool,
    },
    /// Merge a branch or commit into the current branch (object by object, by GlobalId)
    Merge {
        model: PathBuf,
        rev: String,
        /// When both sides changed the same object: keep ours or theirs instead of stopping
        #[arg(long, value_parser = ["ours", "theirs"])]
        prefer: Option<String>,
        #[arg(long)]
        author: Option<String>,
    },
    /// Show the shared folder this history synchronizes with, or set it
    Remote {
        model: PathBuf,
        dir: Option<PathBuf>,
    },
    /// A new local copy of a shared history (its folder) as a model file
    Clone {
        remote: PathBuf,
        model: PathBuf,
        #[arg(long, default_value = "main")]
        branch: String,
    },
    /// Send the current branch to the remote (refused unless it only adds to it)
    Push {
        model: PathBuf,
        /// Your e-mail. Default: $BIMBLE_AUTHOR, else $USER
        #[arg(long)]
        author: Option<String>,
    },
    /// Fetch, merge and push; give back your borrowed elements
    Sync {
        model: PathBuf,
        /// When both changed the same field: keep ours or theirs instead of stopping
        #[arg(long, value_parser = ["ours", "theirs"])]
        prefer: Option<String>,
        #[arg(long)]
        author: Option<String>,
    },
    /// Borrow elements (GlobalIds) before changing them, so others are warned
    Borrow {
        model: PathBuf,
        ids: Vec<String>,
        #[arg(long)]
        author: Option<String>,
    },
}

#[derive(Subcommand)]
enum Cmd {
    /// Open the 3D viewer (optionally with a .ifc or .bimble file)
    View {
        file: Option<PathBuf>,
        /// Graphic style file (JSON; see `bimble style`)
        #[arg(long)]
        style: Option<PathBuf>,
        /// Another model to show as a reference (e.g. the architect's IFC), to compare
        #[arg(long)]
        link: Option<PathBuf>,
    },
    /// A storey plan or any view (ceiling plan, section) at scale, as PDF, SVG or PNG (by extension)
    Plan {
        file: PathBuf,
        /// Output: .pdf, .svg or .png
        out: PathBuf,
        /// Resolution of a .png output, dots per inch
        #[arg(long, default_value_t = 150.0)]
        dpi: f64,
        /// Storey name (see `bimble info`)
        #[arg(long, required_unless_present = "view")]
        storey: Option<String>,
        /// Draw a view instead (a plan with its view range, a ceiling plan, a section) by name,
        /// e.g. "Ceiling Level 1"; `bimble views` lists them
        #[arg(long, conflicts_with_all = ["storey", "cut"])]
        view: Option<String>,
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
        /// Plan template: title block, legend, texts, what is shown (see `bimble plan-template`)
        #[arg(long)]
        template: Option<PathBuf>,
        /// A title block value, KEY=VALUE, used as {KEY} in the template (repeatable)
        #[arg(long = "field", value_name = "KEY=VALUE")]
        fields: Vec<String>,
    },
    /// Print the default plan template, or check a template file and print it in full
    PlanTemplate {
        /// Template to check; without it, the default template is printed
        file: Option<PathBuf>,
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
    /// List the views of a model (plans, ceiling plans, sections), for `plan --view`
    Views { file: PathBuf },
    /// List the sheets of a model (number, name, paper, the views placed on them)
    Sheets { file: PathBuf },
    /// Project information (ADR 0030): print it, or set entries with KEY=VALUE (an empty VALUE
    /// removes the entry). Title blocks show each value where they say {KEY}
    Project {
        file: PathBuf,
        /// KEY=VALUE, e.g. client="City of X" project_number=P-001 (repeatable)
        set: Vec<String>,
    },
    /// List the schedules (columns, beams, slabs, walls, footings by material) and their counts
    Schedules { file: PathBuf },
    /// A schedule as a table: each element measured (section, length, area, volume), grouped
    /// by level with subtotals; printed, or written as CSV for a spreadsheet
    Schedule {
        file: PathBuf,
        /// Schedule name, e.g. "Concrete columns" (see `bimble schedules`)
        name: String,
        /// Write CSV here instead of printing a table
        #[arg(long)]
        csv: Option<PathBuf>,
    },
    /// Draw a sheet (its views at their scales, title block, legend) as PDF, SVG or PNG
    Sheet {
        file: PathBuf,
        /// Sheet number, e.g. S-101 (see `bimble sheets`)
        number: String,
        /// Output: .pdf, .svg or .png
        out: PathBuf,
        /// Resolution of a .png output, dots per inch
        #[arg(long, default_value_t = 150.0)]
        dpi: f64,
        /// Graphic style file (see `bimble style`)
        #[arg(long)]
        style: Option<PathBuf>,
        /// Plan template: title block, legend, texts (see `bimble plan-template`)
        #[arg(long)]
        template: Option<PathBuf>,
        /// Project name in the title block (default: the file name)
        #[arg(long)]
        project: Option<String>,
    },
    /// Add a sheet with views laid out side by side, e.g.
    /// `bimble sheet-new m.bimble --name Plans --place "Plan Level 1:100" --place "Section 1:50"`.
    /// Saved in the .bimble file, or in the side file of an IFC
    SheetNew {
        file: PathBuf,
        #[arg(long)]
        name: String,
        /// Sheet number; by default the next free one (S-101, S-102…)
        #[arg(long)]
        number: Option<String>,
        /// A4, A3, A2, A1 or A0
        #[arg(long, default_value = "A1")]
        paper: String,
        #[arg(long)]
        portrait: bool,
        /// A view and its scale, VIEW:N (repeatable, placed left to right)
        #[arg(long = "place", value_name = "VIEW:N")]
        place: Vec<String>,
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
    /// Convert an IFC file into an editable .bimble model (ADR 0032)
    ImportIfc { file: PathBuf, out: PathBuf },
    /// Model history: commits, branches, log, diff and merge (kept in <model>-history/)
    History {
        #[command(subcommand)]
        cmd: HistoryCmd,
    },
    /// Geometry codec sizes and speeds on files (ADR 0036 #8; maintenance)
    #[command(hide = true)]
    CodecBench { files: Vec<PathBuf> },
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
        /// With occlusion culling (the viewer turns it on when the GPU is slow)
        #[arg(long)]
        occlusion: bool,
        /// Draw far cells' simplified stand-ins when their error spans at most this many
        /// pixels (0: never)
        #[arg(long, default_value_t = 0.0)]
        proxies: f32,
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
        return bimble_viewer::run(None, None, None);
    };
    match cmd {
        Cmd::View { file, style, link } => bimble_viewer::run(file, style, link)?,
        Cmd::Plan {
            file: path,
            out,
            storey,
            scale,
            paper,
            cut,
            style,
            project,
            dpi,
            template,
            fields,
            view,
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
            if !matches!(ext.as_deref(), Some("pdf" | "svg" | "png")) {
                return Err("the output must end in .pdf, .svg or .png".into());
            }
            if !(10.0..=1200.0).contains(&dpi) {
                return Err("--dpi must be between 10 and 1200".into());
            }
            let scene = load_scene(&path)?;
            let project = project.unwrap_or_else(|| {
                path.file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default()
            });
            let view = match view {
                Some(name) => Some(find_view(&path, &scene, &name)?),
                None => None,
            };
            let section_marks = bimble_drawing::plan::section_marks(
                &model_views(&path, &scene).unwrap_or_default(),
            );
            let opts = PlanOptions {
                section_marks,
                storey: storey.unwrap_or_default(),
                cut_above_mm: cut,
                view,
                bare: false,
                scale,
                paper,
                style,
                project,
                date: bimble_drawing::today(),
                template: {
                    let mut t = match template {
                        Some(p) => bimble_drawing::template::PlanTemplate::load(&p)?,
                        None => Default::default(),
                    };
                    // The project's information, then --field on top (ADR 0030).
                    t = t.with_project(&model_project(&path)?);
                    for f in fields {
                        let (k, v) = f
                            .split_once('=')
                            .ok_or_else(|| format!("--field {f:?}: expected KEY=VALUE"))?;
                        t.fields.insert(k.trim().to_owned(), v.replace("\\n", "\n"));
                    }
                    t
                },
            };
            let (sheet, info) = plan(&scene, &opts)?;
            match ext.as_deref() {
                Some("png") => {
                    let r = sheet.rasterize(dpi / 25.4)?;
                    bimble_render::write_png(&out, r.width, r.height, &r.rgba)
                        .map_err(|e| format!("{}: {e}", out.display()))?;
                }
                _ => {
                    let bytes = if ext.as_deref() == Some("pdf") {
                        sheet.to_pdf()
                    } else {
                        sheet.to_svg().into_bytes()
                    };
                    file::write_atomic(&out, &bytes)
                        .map_err(|e| format!("{}: {e}", out.display()))?;
                }
            }
            println!(
                "plan 1:{} on {}{}: {} cut elements, {} seen, {} lines below, {} above, {} axes{}{} → {}",
                scale,
                info.paper.name(),
                if info.portrait { " portrait" } else { "" },
                info.cut_elements,
                info.seen_elements,
                info.projection_lines,
                info.above_lines,
                info.axes,
                if info.axes > 0 && !info.model_axes {
                    " (from the columns)"
                } else {
                    ""
                },
                if info.hidden_elements > 0 {
                    format!(", {} hidden by class", info.hidden_elements)
                } else {
                    String::new()
                },
                out.display()
            );
        }
        Cmd::PlanTemplate { file } => {
            let t = match file {
                Some(p) => bimble_drawing::template::PlanTemplate::load(&p)?,
                None => Default::default(),
            };
            println!("{}", t.to_json());
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
            let edges: usize = scene.elements.iter().map(|e| e.edge_count()).sum();
            println!(
                "meshes    {} triangles, {edges} edges",
                scene.triangle_count()
            );
            println!(
                "stored    {} triangles ({} elements are copies of shared shapes)",
                scene.stored_triangles(),
                scene.instance_count()
            );
            println!("loaded in {load:.1?}");
            for w in &scene.warnings {
                println!("warning   {w}");
            }
        }
        Cmd::Sheets { file: path } => {
            let scene = load_scene(&path)?;
            let views = model_views(&path, &scene)?;
            let sheets = model_sheets(&path)?;
            if sheets.is_empty() {
                println!("no sheets (bimble sheet-new adds one)");
            }
            for s in sheets {
                let placed: Vec<String> = s
                    .placed
                    .iter()
                    .map(|p| {
                        let name = views
                            .iter()
                            .find(|v| v.id == p.view)
                            .map_or("(missing view)", |v| v.name.as_str());
                        format!("{name} 1:{}", p.scale)
                    })
                    .collect();
                println!(
                    "{:<10} {:<30} {}{}  {}",
                    s.number,
                    s.name,
                    s.paper,
                    if s.portrait { " portrait" } else { "" },
                    placed.join(", ")
                );
            }
        }
        Cmd::Sheet {
            file: path,
            number,
            out,
            dpi,
            style,
            template,
            project,
        } => {
            let ext = out
                .extension()
                .and_then(|x| x.to_str())
                .map(str::to_ascii_lowercase);
            if !matches!(ext.as_deref(), Some("pdf" | "svg" | "png")) {
                return Err("the output must end in .pdf, .svg or .png".into());
            }
            if !(10.0..=1200.0).contains(&dpi) {
                return Err("--dpi must be between 10 and 1200".into());
            }
            let scene = load_scene(&path)?;
            let views = model_views(&path, &scene)?;
            let sheets = model_sheets(&path)?;
            let def = sheets
                .iter()
                .find(|s| s.number.eq_ignore_ascii_case(&number))
                .ok_or_else(|| {
                    let n: Vec<&str> = sheets.iter().map(|s| s.number.as_str()).collect();
                    format!("no sheet {number:?}. Sheets: {}", n.join(", "))
                })?;
            let o = bimble_drawing::sheetdraw::SheetOptions {
                style: match style {
                    Some(p) => bimble_render::GraphicStyle::load(&p)?,
                    None => Default::default(),
                },
                template: match template {
                    Some(p) => bimble_drawing::template::PlanTemplate::load(&p)?,
                    None => bimble_drawing::template::PlanTemplate::default(),
                }
                .with_project(&model_project(&path)?),
                project: project.unwrap_or_else(|| {
                    path.file_stem()
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_default()
                }),
                date: bimble_drawing::today(),
            };
            let (sheet, info) = bimble_drawing::sheetdraw::draw(&scene, def, &views, &o)?;
            match ext.as_deref() {
                Some("png") => {
                    let r = sheet.rasterize(dpi / 25.4)?;
                    bimble_render::write_png(&out, r.width, r.height, &r.rgba)
                        .map_err(|e| format!("{}: {e}", out.display()))?;
                }
                _ => {
                    let bytes = if ext.as_deref() == Some("pdf") {
                        sheet.to_pdf()
                    } else {
                        sheet.to_svg().into_bytes()
                    };
                    file::write_atomic(&out, &bytes)
                        .map_err(|e| format!("{}: {e}", out.display()))?;
                }
            }
            println!(
                "sheet {} on {}: {} views{} → {}",
                def.number,
                def.paper,
                info.views_drawn,
                if info.problems.is_empty() {
                    String::new()
                } else {
                    format!(" ({})", info.problems.join("; "))
                },
                out.display()
            );
        }
        Cmd::SheetNew {
            file: path,
            name,
            number,
            paper,
            portrait,
            place,
        } => {
            let scene = load_scene(&path)?;
            let sheets = model_sheets(&path)?;
            let number = number.unwrap_or_else(|| {
                bimble_core::sheet::next_number(sheets.iter().map(|s| s.number.as_str()))
            });
            let mut def = bimble_core::sheet::Sheet::new(number, name);
            def.paper = paper.to_ascii_uppercase();
            def.portrait = portrait;
            let (w, h) = def
                .size_mm()
                .ok_or_else(|| format!("--paper {paper}: A4, A3, A2, A1 or A0"))?;
            // Each view's drawing size at its scale, then left to right, centred in the space
            // above the title block.
            let template = bimble_drawing::template::PlanTemplate::default();
            let mut sizes = Vec::new();
            for p in &place {
                let (vname, n) = p
                    .rsplit_once(':')
                    .ok_or_else(|| format!("--place {p:?}: expected VIEW:N"))?;
                let scale: u32 = n
                    .trim()
                    .parse()
                    .map_err(|_| format!("--place {p:?}: scale N must be a number"))?;
                let v = find_view(&path, &scene, vname.trim())?;
                let (drawn, _) = bimble_drawing::plan::plan(
                    &scene,
                    &bimble_drawing::plan::PlanOptions {
                        scale,
                        view: Some(v.clone()),
                        bare: true,
                        template: template.clone(),
                        ..Default::default()
                    },
                )?;
                sizes.push((v.id, scale, drawn.width_mm, drawn.height_mm));
            }
            let margin = 10.0;
            let usable_h = h - 2.0 * margin - template.title_block.height_mm;
            let total_w: f64 = sizes.iter().map(|s| s.2).sum();
            let gap = ((w - 2.0 * margin - total_w) / (sizes.len() as f64 + 1.0)).max(0.0);
            let mut x = margin + gap;
            for (view, scale, sw, sh) in sizes {
                if sh > usable_h || x + sw > w - margin {
                    eprintln!(
                        "warning: views don't all fit on {paper}: move them or use bigger paper"
                    );
                }
                def.placed.push(bimble_core::sheet::Placed {
                    id: GlobalId::new(),
                    view,
                    scale,
                    at_mm: [
                        (x + sw / 2.0).clamp(0.0, w),
                        (margin + usable_h / 2.0).clamp(0.0, h),
                    ],
                });
                x += sw + gap;
            }
            let number = def.number.clone();
            save_sheet(&path, def)?;
            println!("sheet {number} added to {}", path.display());
        }
        Cmd::Project { file: path, set } => {
            let mut p = model_project(&path)?;
            if set.is_empty() {
                for (k, v) in &p {
                    println!("{k:<20} {v}");
                }
                return Ok(());
            }
            for kv in &set {
                let (k, v) = kv
                    .split_once('=')
                    .ok_or_else(|| format!("{kv:?}: expected KEY=VALUE"))?;
                let k = bimble_core::project::key_of(k);
                if v.trim().is_empty() {
                    p.remove(&k);
                } else {
                    p.insert(k, v.to_string());
                }
            }
            bimble_core::project::validate(&p)?;
            if is_ifc(&path) {
                let f = read_side(&path)?;
                let f = bimble_core::view::ViewsFile::new(f.views, f.sheets)
                    .with_project(p)
                    .with_schedules(f.schedules);
                let side = bimble_core::view::side_file(&path);
                file::write_atomic(&side, f.to_json().as_bytes())
                    .map_err(|e| format!("{}: {e}", side.display()))?;
                println!("project information → {}", side.display());
            } else {
                let mut m = file::load(&path).map_err(|e| format!("{}: {e}", path.display()))?;
                bimble_core::Command::SetProject { project: p }
                    .apply(&mut m)
                    .map_err(|e| e.to_string())?;
                file::save(&m, &path).map_err(|e| format!("{}: {e}", path.display()))?;
                println!("project information → {}", path.display());
            }
        }
        Cmd::Schedules { file: path } => {
            let scene = load_scene(&path)?;
            for d in bimble_core::schedule::effective(&model_schedules(&path)?) {
                let t = bimble_drawing::schedule::schedule(&scene, &d);
                println!("{:<20} {:>6}", d.name, t.count);
            }
        }
        Cmd::Schedule {
            file: path,
            name,
            csv,
        } => {
            let scene = load_scene(&path)?;
            let defs = bimble_core::schedule::effective(&model_schedules(&path)?);
            let d = defs
                .iter()
                .find(|d| d.name.eq_ignore_ascii_case(name.trim()))
                .ok_or_else(|| {
                    let names: Vec<&str> = defs.iter().map(|d| d.name.as_str()).collect();
                    format!("no schedule {name:?}. Schedules: {}", names.join(", "))
                })?;
            let t = bimble_drawing::schedule::schedule(&scene, d);
            match csv {
                Some(out) => {
                    std::fs::write(&out, bimble_drawing::schedule::to_csv(&t))
                        .map_err(|e| format!("{}: {e}", out.display()))?;
                    println!("{}: {} rows → {}", t.name, t.count, out.display());
                }
                None => print!("{}", bimble_drawing::schedule::to_text(&t)),
            }
        }
        Cmd::Views { file: path } => {
            use bimble_core::view::{Look, ViewKind};
            let scene = load_scene(&path)?;
            let levels = scene.levels();
            for v in model_views(&path, &scene)? {
                let what = match &v.kind {
                    ViewKind::ThreeD => "3D".to_string(),
                    ViewKind::Plan {
                        storey,
                        look,
                        range,
                    } => {
                        let r = bimble_core::view::resolve(range, *look, storey, &levels);
                        let kind = match look {
                            Look::Down => "plan",
                            Look::Up => "ceiling plan",
                        };
                        match r {
                            Ok(r) => {
                                let (lo, hi) = r.visible(*look);
                                let m = |z: f64| {
                                    if z.is_finite() {
                                        format!("{:+.3} m", z / 1000.0)
                                    } else {
                                        "unlimited".into()
                                    }
                                };
                                format!("{kind}, cut {}, shows {} to {}", m(r.cut), m(lo), m(hi))
                            }
                            Err(e) => format!("{kind} (invalid: {e})"),
                        }
                    }
                    ViewKind::Section {
                        origin,
                        facing,
                        depth_mm,
                    } => format!(
                        "section looking {} through {:.0}, {:.0}{}",
                        facing.label(),
                        origin[0],
                        origin[1],
                        if *depth_mm > 0.0 {
                            format!(", depth {:.0} mm", depth_mm)
                        } else {
                            String::new()
                        }
                    ),
                };
                let refs = if v.references.is_empty() {
                    String::new()
                } else {
                    format!(", {} reference plan(s)", v.references.len())
                };
                println!("{:<28} {what}{refs}", v.name);
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
        Cmd::History { cmd } => history(cmd)?,
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
            let gs = r.upload_with(&gpu.device, &gpu.queue, &scene, &visible, clip.is_some());
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
        Cmd::CodecBench { files } => codec_bench::run(files)?,
        Cmd::Bench {
            files,
            frames,
            width,
            height,
            samples,
            quick,
            occlusion,
            proxies,
            json,
        } => {
            if !matches!(samples, 1 | 4) {
                return Err("--samples must be 1 or 4".into());
            }
            bench::run(bench::BenchArgs {
                occlusion,
                proxies,
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
        Cmd::ImportIfc { file: path, out } => {
            let text =
                std::fs::read_to_string(&path).map_err(|x| format!("{}: {x}", path.display()))?;
            let (mut m, rep) = bimble_ifc::import::import_str(&text)?;
            m.project_name = path
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("Imported model")
                .to_owned();
            m.validate()
                .map_err(|x| format!("imported model invalid: {x}"))?;
            file::save(&m, &out).map_err(e)?;
            println!("→ {}", out.display());
            for l in rep.lines() {
                println!("{l}");
            }
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
fn history(cmd: HistoryCmd) -> Result<(), String> {
    use bimble_core::remote;
    use bimble_core::repo::{Head, Merged, Prefer, Repo};
    // Only a push may create the remote: a mistyped or unreachable path must not become a new,
    // empty shared history that a sync then "synchronizes" with.
    let open_remote = |model: &std::path::Path, create: bool| -> Result<(Repo, Repo), String> {
        let r = Repo::open(model).map_err(|x| x.to_string())?;
        let dir = remote::remote_of(&r)
            .map_err(|x| x.to_string())?
            .ok_or("no remote: set one with `bimble history remote <model> <folder>`")?;
        let share = if create {
            remote::open_or_init(&dir)
        } else {
            Repo::open_dir(&dir)
        }
        .map_err(|x| format!("remote not reachable: {} ({x})", dir.display()))?;
        Ok((r, share))
    };
    let e = |x: bimble_core::repo::RepoError| x.to_string();
    let fe = |x: file::FileError| x.to_string();
    let author_of = |a: Option<String>| {
        a.or_else(|| std::env::var("BIMBLE_AUTHOR").ok())
            .or_else(|| std::env::var("USER").ok())
            .or_else(|| std::env::var("USERNAME").ok())
            .unwrap_or_else(|| "unknown".into())
    };
    let short = |h: &str| h[..12].to_owned();
    let print_changes = |d: &[version::Change], json: bool| {
        if json {
            println!("{}", serde_json::to_string_pretty(d).unwrap());
        } else if d.is_empty() {
            println!("no changes");
        } else {
            for c in d {
                println!("{c}");
            }
        }
    };
    let now = bimble_core::date::now_secs;
    match cmd {
        HistoryCmd::Init { model } => {
            // The model must be a readable .bimble file: the history is of that model.
            file::load(&model).map_err(fe)?;
            let r = Repo::init(&model).map_err(e)?;
            println!("history started in {} (branch main)", r.dir().display());
        }
        HistoryCmd::Commit {
            model,
            message,
            author,
        } => {
            let r = Repo::open(&model).map_err(e)?;
            let m = file::load(&model).map_err(fe)?;
            let changes = r.status(&m).map_err(e)?.len();
            let h = r
                .commit(&m, &message, &author_of(author), now())
                .map_err(e)?;
            println!("{} {message} ({changes} change(s))", short(&h));
        }
        HistoryCmd::Status { model } => {
            let r = Repo::open(&model).map_err(e)?;
            let m = file::load(&model).map_err(fe)?;
            match r.head().map_err(e)? {
                Head::Branch(b) => println!("on branch {b}"),
                Head::Detached(h) => println!("at commit {} (no branch)", short(&h)),
            }
            print_changes(&r.status(&m).map_err(e)?, false);
        }
        HistoryCmd::Log { model, rev, limit } => {
            let r = Repo::open(&model).map_err(e)?;
            if r.head_commit().map_err(e)?.is_none() && rev == "HEAD" {
                println!("no commits yet");
                return Ok(());
            }
            let branches = r.branches().map_err(e)?;
            for (h, c) in r.log(&rev, limit).map_err(e)? {
                let names: Vec<&str> = branches
                    .iter()
                    .filter(|(_, b)| *b == h)
                    .map(|(n, _)| n.as_str())
                    .collect();
                let tags = if names.is_empty() {
                    String::new()
                } else {
                    format!(" ({})", names.join(", "))
                };
                let merge = if c.parents.len() > 1 { " [merge]" } else { "" };
                println!(
                    "{}{tags}{merge}  {}  {}\n    {}",
                    short(&h),
                    bimble_core::date::format_utc(c.time),
                    c.author,
                    c.message.replace('\n', "\n    ")
                );
            }
        }
        HistoryCmd::Diff { model, a, b, json } => {
            let r = Repo::open(&model).map_err(e)?;
            let ma = r.model_at(&a).map_err(e)?;
            let mb = match b {
                Some(b) => r.model_at(&b).map_err(e)?,
                None => file::load(&model).map_err(fe)?,
            };
            print_changes(&version::diff(&ma, &mb), json);
        }
        HistoryCmd::Branch { model, name, at } => {
            let r = Repo::open(&model).map_err(e)?;
            match name {
                Some(n) => {
                    let h = r.create_branch(&n, at.as_deref()).map_err(e)?;
                    println!("branch {n} at {}", short(&h));
                }
                None => {
                    let cur = r.head().map_err(e)?;
                    for (n, h) in r.branches().map_err(e)? {
                        let mark = if cur == Head::Branch(n.clone()) {
                            "*"
                        } else {
                            " "
                        };
                        println!("{mark} {n}  {}", short(&h));
                    }
                }
            }
        }
        HistoryCmd::Switch { model, rev, force } => {
            let r = Repo::open(&model).map_err(e)?;
            let working = file::load(&model).map_err(fe)?;
            let pending = r.status(&working).map_err(e)?;
            if !pending.is_empty() && !force {
                return Err(format!(
                    "{} change(s) not committed; commit them or use --force to drop them",
                    pending.len()
                ));
            }
            let m = r.switch(&rev).map_err(e)?;
            file::save(&m, &model).map_err(fe)?;
            println!("switched to {rev}: {} elements", m.elements.len());
        }
        HistoryCmd::Merge {
            model,
            rev,
            prefer,
            author,
        } => {
            let r = Repo::open(&model).map_err(e)?;
            let working = file::load(&model).map_err(fe)?;
            if !r.status(&working).map_err(e)?.is_empty() {
                return Err("the model has changes not committed; commit them first".into());
            }
            let prefer = match prefer.as_deref() {
                Some("ours") => Prefer::Ours,
                Some("theirs") => Prefer::Theirs,
                _ => Prefer::Neither,
            };
            match r
                .merge(&rev, prefer, &author_of(author), now())
                .map_err(e)?
            {
                Ok((how, m)) => {
                    file::save(&m, &model).map_err(fe)?;
                    match how {
                        Merged::UpToDate => println!("already up to date"),
                        Merged::FastForward(h) => println!("fast-forward to {}", short(&h)),
                        Merged::Commit(h) => println!("merged {rev}: {}", short(&h)),
                    }
                }
                Err(conflicts) => {
                    let list: Vec<String> = conflicts.iter().map(|c| format!("  {c}")).collect();
                    return Err(format!(
                        "{} conflict(s), nothing merged (use --prefer ours|theirs):\n{}",
                        conflicts.len(),
                        list.join("\n")
                    ));
                }
            }
        }
        HistoryCmd::Remote { model, dir } => {
            let r = Repo::open(&model).map_err(e)?;
            match dir {
                Some(d) => {
                    remote::set_remote(&r, &d).map_err(e)?;
                    println!("remote: {}", d.display());
                }
                None => match remote::remote_of(&r).map_err(e)? {
                    Some(d) => println!("{}", d.display()),
                    None => println!("no remote"),
                },
            }
        }
        HistoryCmd::Clone {
            remote: dir,
            model,
            branch,
        } => {
            if model.exists() {
                return Err(format!("{} already exists", model.display()));
            }
            let m = remote::clone(&dir, &model, &branch).map_err(e)?;
            file::save(&m, &model).map_err(fe)?;
            println!("cloned {branch}: {} elements", m.elements.len());
        }
        HistoryCmd::Push { model, author } => {
            let (r, share) = open_remote(&model, true)?;
            let branch = match r.head().map_err(e)? {
                Head::Branch(b) => b,
                Head::Detached(_) => return Err("not on a branch".into()),
            };
            let h = remote::push(&r, &share, &branch, &author_of(author), now()).map_err(e)?;
            println!("pushed {branch}: {}", short(&h));
        }
        HistoryCmd::Sync {
            model,
            prefer,
            author,
        } => {
            let (r, share) = open_remote(&model, false)?;
            let working = file::load(&model).map_err(fe)?;
            if !r.status(&working).map_err(e)?.is_empty() {
                return Err("the model has changes not committed; commit them first".into());
            }
            let prefer = match prefer.as_deref() {
                Some("ours") => Prefer::Ours,
                Some("theirs") => Prefer::Theirs,
                _ => Prefer::Neither,
            };
            match remote::sync(&r, &share, &author_of(author), now(), prefer).map_err(e)? {
                Ok((how, m)) => {
                    file::save(&m, &model).map_err(fe)?;
                    match how {
                        Merged::UpToDate => println!("synchronized: nothing new from others"),
                        Merged::FastForward(h) => println!("synchronized: now at {}", short(&h)),
                        Merged::Commit(h) => println!("synchronized: merged {}", short(&h)),
                    }
                }
                Err(conflicts) => {
                    let list: Vec<String> = conflicts.iter().map(|c| format!("  {c}")).collect();
                    return Err(format!(
                        "{} conflict(s), nothing pushed (use --prefer ours|theirs; an element \
                         borrowed by someone else waits until they synchronize):\n{}",
                        conflicts.len(),
                        list.join("\n")
                    ));
                }
            }
        }
        HistoryCmd::Borrow { model, ids, author } => {
            let (_, share) = open_remote(&model, false)?;
            let ids = ids
                .iter()
                .map(|s| s.parse::<GlobalId>())
                .collect::<Result<Vec<_>, _>>()?;
            let taken = remote::borrow(&share, &author_of(author), &ids, now()).map_err(e)?;
            if taken.is_empty() {
                println!("borrowed {} element(s)", ids.len());
            } else {
                let list: Vec<String> = taken
                    .iter()
                    .map(|(id, who)| format!("  {id} belongs to {who}"))
                    .collect();
                return Err(format!(
                    "nothing borrowed; ask them to synchronize:\n{}",
                    list.join("\n")
                ));
            }
        }
    }
    Ok(())
}

/// The views of a model: stored in a .bimble, in the side file of an IFC, over the defaults.
/// The schedules a model keeps (ADR 0029): in a .bimble, or in an IFC's side file.
fn model_schedules(
    path: &std::path::Path,
) -> Result<Vec<bimble_core::schedule::ScheduleDef>, String> {
    if is_ifc(path) {
        let side = bimble_core::view::side_file(path);
        if !side.exists() {
            return Ok(Vec::new());
        }
        let bytes = std::fs::read(&side).map_err(|e| format!("{}: {e}", side.display()))?;
        Ok(bimble_core::view::ViewsFile::from_bytes(&bytes)
            .map_err(|e| format!("{}: {e}", side.display()))?
            .schedules)
    } else {
        Ok(bimble_core::file::load(path)
            .map_err(|e| format!("{}: {e}", path.display()))?
            .schedules)
    }
}

/// The project information of a model (ADR 0030): in a .bimble, or in an IFC's side file.
fn model_project(
    path: &std::path::Path,
) -> Result<std::collections::BTreeMap<String, String>, String> {
    let is_ifc = path
        .extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"));
    if is_ifc {
        let side = bimble_core::view::side_file(path);
        if !side.exists() {
            return Ok(Default::default());
        }
        let bytes = std::fs::read(&side).map_err(|e| format!("{}: {e}", side.display()))?;
        Ok(bimble_core::view::ViewsFile::from_bytes(&bytes)
            .map_err(|e| format!("{}: {e}", side.display()))?
            .project)
    } else {
        Ok(bimble_core::file::load(path)
            .map_err(|e| format!("{}: {e}", path.display()))?
            .project)
    }
}

fn model_views(
    path: &std::path::Path,
    scene: &bimble_geom::Scene,
) -> Result<Vec<bimble_core::view::View>, String> {
    use bimble_core::view::{ViewsFile, effective, side_file};
    let is_ifc = path
        .extension()
        .and_then(|x| x.to_str())
        .is_some_and(|x| x.eq_ignore_ascii_case("ifc"));
    let stored = if is_ifc {
        let side = side_file(path);
        if side.exists() {
            let bytes = std::fs::read(&side).map_err(|e| format!("{}: {e}", side.display()))?;
            ViewsFile::from_bytes(&bytes)
                .map_err(|e| format!("{}: {e}", side.display()))?
                .views
        } else {
            Default::default()
        }
    } else {
        bimble_core::file::load(path)
            .map_err(|e| format!("{}: {e}", path.display()))?
            .views
    };
    Ok(effective(&stored, &scene.levels()))
}

/// The sheets of a model: in a .bimble, or in the side file of an IFC.
fn model_sheets(path: &std::path::Path) -> Result<Vec<bimble_core::sheet::Sheet>, String> {
    let mut v: Vec<_> = if is_ifc(path) {
        read_side(path)?.sheets.into_values().collect()
    } else {
        bimble_core::file::load(path)
            .map_err(|e| format!("{}: {e}", path.display()))?
            .sheets
            .into_values()
            .collect()
    };
    v.sort_by(|a, b| a.number.cmp(&b.number));
    Ok(v)
}

/// The side file of an IFC (empty when there is none).
fn read_side(path: &std::path::Path) -> Result<bimble_core::view::ViewsFile, String> {
    use bimble_core::view::{ViewsFile, side_file};
    let side = side_file(path);
    if !side.exists() {
        return Ok(ViewsFile::new(Default::default(), Default::default()));
    }
    let bytes = std::fs::read(&side).map_err(|e| format!("{}: {e}", side.display()))?;
    ViewsFile::from_bytes(&bytes).map_err(|e| format!("{}: {e}", side.display()))
}

/// Add a sheet: a Command in a .bimble model (saved), else in the IFC's side file.
fn save_sheet(path: &std::path::Path, def: bimble_core::sheet::Sheet) -> Result<(), String> {
    if is_ifc(path) {
        let scene = load_scene(path)?;
        let ids: std::collections::BTreeSet<GlobalId> =
            model_views(path, &scene)?.iter().map(|v| v.id).collect();
        def.validate(|v| ids.contains(&v))?;
        let mut f = read_side(path)?;
        if f.sheets.values().any(|s| s.number == def.number) {
            return Err(format!("sheet number {} is already used", def.number));
        }
        f.sheets.insert(def.id, def);
        // Project information kept as it was (ADR 0030).
        let f = bimble_core::view::ViewsFile::new(f.views, f.sheets)
            .with_project(f.project)
            .with_schedules(f.schedules);
        let side = bimble_core::view::side_file(path);
        file::write_atomic(&side, f.to_json().as_bytes())
            .map_err(|e| format!("{}: {e}", side.display()))
    } else {
        let mut m = file::load(path).map_err(|e| format!("{}: {e}", path.display()))?;
        bimble_core::Command::AddSheet(def)
            .apply(&mut m)
            .map_err(|e| e.to_string())?;
        file::save(&m, path).map_err(|e| format!("{}: {e}", path.display()))
    }
}

/// A view by name (any letter case).
fn find_view(
    path: &std::path::Path,
    scene: &bimble_geom::Scene,
    name: &str,
) -> Result<bimble_core::view::View, String> {
    let views = model_views(path, scene)?;
    views
        .iter()
        .find(|v| v.name == name)
        .or_else(|| views.iter().find(|v| v.name.eq_ignore_ascii_case(name)))
        .cloned()
        .ok_or_else(|| {
            let names: Vec<&str> = views
                .iter()
                .filter(|v| v.is_2d())
                .map(|v| v.name.as_str())
                .collect();
            format!("no view {name:?}. Plans and sections: {}", names.join(", "))
        })
}

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
        let world = el.world_mesh();
        for p in &world.positions {
            for k in 0..3 {
                let w = p[k] as f64 + o[k];
                lo[k] = lo[k].min(w);
                hi[k] = hi[k].max(w);
            }
        }
        let j = serde_json::json!({
            "global_id": el.global_id, "class": el.class, "name": el.name, "storey": el.storey,
            "material": el.material, "category": el.category.label(),
            "triangles": el.triangle_count(), "volume_mm3": world.volume(),
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
