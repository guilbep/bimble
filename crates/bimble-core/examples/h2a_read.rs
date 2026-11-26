//! Ticket H2a (C core): Rust reads a history folder the C core wrote.
//! `cargo run -p bimble-core --example h2a_read -- <folder> [<model_at dir>]`
//! Prints HEAD, the branches, the log of each branch (`<hash> <time> <author> <parents|-> <message>`,
//! as ccore/golden/history/log.txt), the object check of every commit's tree (`model_at` loads and
//! validates, mesh references resolved) and, with a second argument, whether each commit's model
//! is byte for byte `<dir>/model_at_<n>.json` (n in log order, oldest first, on the first branch).
use bimble_core::repo::{Head, Repo};
use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let r = match Repo::open_dir(Path::new(&args[1])) {
        Ok(r) => r,
        Err(e) => return println!("open error {e}"),
    };
    match r.head() {
        Ok(Head::Branch(b)) => println!("HEAD ref: {b}"),
        Ok(Head::Detached(h)) => println!("HEAD {h}"),
        Err(e) => println!("HEAD error {e}"),
    }
    let branches = r.branches().expect("branches");
    for (name, h) in &branches {
        println!("{name} {h}");
    }
    let mut revs: Vec<String> = branches.iter().map(|(n, _)| n.clone()).collect();
    if let Ok(Head::Detached(h)) = r.head() {
        revs.push(h);
    }
    for rev in &revs {
        println!("log {rev}");
        match r.log(rev, 100) {
            Ok(log) => {
                for (h, c) in log {
                    let parents = if c.parents.is_empty() { "-".to_string() } else { c.parents.join(",") };
                    println!("{h} {} {} {parents} {}", c.time, c.author, c.message);
                    match r.model_at(&h) {
                        Ok(m) => println!("  model_at ok: {} elements", m.elements.len()),
                        Err(e) => println!("  model_at error {e}"),
                    }
                }
            }
            Err(e) => println!("  log error {e}"),
        }
    }
    if let Some(dir) = args.get(2) {
        for (rev, first) in [("main", 1), ("variant", 1)] {
            let Ok(log) = r.log(rev, 100) else { continue };
            let n = log.len();
            for (i, (h, _)) in log.iter().enumerate() {
                let k = n - i + first - 1 + if rev == "variant" && i == 0 { 1 } else { 0 };
                let want = std::fs::read_to_string(Path::new(dir).join(format!("model_at_{k}.json")));
                let got = r.model_at(h).map(|m| bimble_core::file::to_string(&m));
                println!(
                    "model_at {rev} {} = model_at_{k}.json: {}",
                    &h[..12],
                    match (got, want) {
                        (Ok(g), Ok(w)) => if g == w { "equal" } else { "DIFFER" },
                        _ => "unreadable",
                    }
                );
            }
        }
    }
}
