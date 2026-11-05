//! Performance budgets: deterministic geometry counts for fixed models.
//!
//! Wall-clock times are too noisy on shared CI runners to fail a build. What drives frame time and
//! memory on old machines is the amount of geometry, and that is exactly reproducible. This test
//! compares triangle/edge counts and GPU buffer bytes with `tests/budgets.json`.
//!
//! When a change is intended (e.g. finer circles), regenerate the file and commit it with the
//! change, so the PR diff shows the cost:
//!
//!     BIMBLE_UPDATE_BUDGETS=1 cargo test -p bimble-ifc --test budgets
//!
//! Timing trends are tracked separately with `bimble bench` (see docs/adr/0011).

use bimble_core::demo::{FrameSpec, concrete_frame};
use bimble_geom::Scene;
use std::collections::BTreeMap;

/// Bytes the renderer uploads per face vertex and per edge vertex (see bimble-render).
const FACE_VERTEX_BYTES: usize = 28;
const LINE_VERTEX_BYTES: usize = 20;

fn fixture(name: &str) -> String {
    std::fs::read_to_string(format!(
        "{}/tests/fixtures/{name}",
        env!("CARGO_MANIFEST_DIR")
    ))
    .unwrap()
}

fn measure(scene: &Scene) -> serde_json::Value {
    let tris: usize = scene.elements.iter().map(|e| e.mesh.triangle_count()).sum();
    let edges: usize = scene.elements.iter().map(|e| e.mesh.edges.len()).sum();
    serde_json::json!({
        "elements": scene.elements.len(),
        "triangles": tris,
        "edges": edges,
        "gpu_bytes": tris * 3 * FACE_VERTEX_BYTES + edges * 2 * LINE_VERTEX_BYTES,
    })
}

fn frame(bx: usize, storeys: usize) -> Scene {
    Scene::from_model(&concrete_frame(&FrameSpec {
        bays_x: bx,
        bays_y: bx,
        storeys,
        ..Default::default()
    }))
}

#[test]
fn geometry_budgets_match() {
    let mut got: BTreeMap<String, serde_json::Value> = BTreeMap::new();
    got.insert("frame 3x3x2".into(), measure(&frame(3, 2)));
    got.insert("frame 10x10x3".into(), measure(&frame(10, 3)));
    got.insert("frame 40x40x10".into(), measure(&frame(40, 10)));
    for f in [
        "booleans.ifc",
        "curves.ifc",
        "mirror.ifc",
        "tilt.ifc",
        "cyclic_map.ifc",
    ] {
        let s = bimble_ifc::read::read_str(&fixture(f)).unwrap();
        got.insert(f.into(), measure(&s));
    }

    let path = format!("{}/tests/budgets.json", env!("CARGO_MANIFEST_DIR"));
    let got_json = serde_json::to_string_pretty(&got).unwrap() + "\n";
    if std::env::var("BIMBLE_UPDATE_BUDGETS").is_ok() {
        std::fs::write(&path, &got_json).unwrap();
        return;
    }
    let want: BTreeMap<String, serde_json::Value> =
        serde_json::from_str(&std::fs::read_to_string(&path).expect("tests/budgets.json missing"))
            .unwrap();
    let mut diffs = Vec::new();
    for (k, g) in &got {
        match want.get(k) {
            None => diffs.push(format!("{k}: new model, not in budgets.json")),
            Some(w) if w != g => diffs.push(format!("{k}:\n  budget {w}\n  now    {g}")),
            _ => {}
        }
    }
    assert!(
        diffs.is_empty(),
        "geometry budgets changed:\n{}\n\nIf intended, run BIMBLE_UPDATE_BUDGETS=1 cargo test -p bimble-ifc --test budgets \
         and commit tests/budgets.json with the change.",
        diffs.join("\n")
    );
}
