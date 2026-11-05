//! `bimble` command-line tool. Same commands the viewer and agents will use.

use bimble_core::{Command, Generator, GlobalId, History, demo, file, version};
use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

#[derive(Parser)]
#[command(name = "bimble", version, about = "Open-source structural BIM")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
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
    /// Summary of a .bimble file: counts, hash, mesh size and meshing time
    Info { file: PathBuf },
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
}

fn run(cli: Cli) -> Result<(), String> {
    let e = |x: file::FileError| x.to_string();
    match cli.cmd {
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
        Cmd::Info { file: path } => {
            let m = file::load(&path).map_err(e)?;
            println!("project   {} [{}]", m.project_name, m.project_id);
            println!("hash      {}", version::model_hash(&m));
            println!("storeys   {}", m.storeys.len());
            println!("materials {}", m.materials.len());
            println!("elements  {}", m.elements.len());
            let t = Instant::now();
            let (mut tris, mut edges) = (0, 0);
            for el in m.elements.values() {
                let mesh =
                    bimble_geom::element_mesh(el).map_err(|x| format!("{}: {x:?}", el.name))?;
                tris += mesh.triangle_count();
                edges += mesh.edges.len();
            }
            println!(
                "meshes    {tris} triangles, {edges} edges, built in {:.1?}",
                t.elapsed()
            );
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
