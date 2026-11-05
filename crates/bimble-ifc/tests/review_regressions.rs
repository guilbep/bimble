//! Regressions from the 5 Nov 2026 code review. Fixtures are small hand-written IFC files;
//! expected boxes come from IfcOpenShell 0.9 on the same files.

use bimble_ifc::read::{Limits, read_str, read_str_with_limits};
use std::time::{Duration, Instant};

fn fixture(name: &str) -> String {
    std::fs::read_to_string(format!(
        "{}/tests/fixtures/{name}",
        env!("CARGO_MANIFEST_DIR")
    ))
    .unwrap()
}

/// World bounding box (mm) of the element with this name.
fn bbox(scene: &bimble_geom::Scene, name: &str) -> ([f64; 3], [f64; 3]) {
    let e = scene
        .elements
        .iter()
        .find(|e| e.name == name)
        .unwrap_or_else(|| panic!("no element {name}"));
    let mut lo = [f64::MAX; 3];
    let mut hi = [f64::MIN; 3];
    for p in &e.mesh.positions {
        for k in 0..3 {
            let w = p[k] as f64 + scene.origin_mm[k];
            lo[k] = lo[k].min(w);
            hi[k] = hi[k].max(w);
        }
    }
    (lo, hi)
}

fn assert_box(got: ([f64; 3], [f64; 3]), want: ([f64; 3], [f64; 3]), tol: f64) {
    for k in 0..3 {
        assert!(
            (got.0[k] - want.0[k]).abs() < tol && (got.1[k] - want.1[k]).abs() < tol,
            "axis {k}: got {got:?}, want {want:?}"
        );
    }
}

#[test]
fn cyclic_mapped_item_terminates_quickly() {
    // A representation map that contains itself twice. Used to expand exponentially (OOM).
    let t = Instant::now();
    let s = read_str(&fixture("cyclic_map.ifc")).unwrap();
    assert!(
        t.elapsed() < Duration::from_secs(5),
        "took {:?}",
        t.elapsed()
    );
    assert!(
        s.warnings.iter().any(|w| w.contains("contains itself")),
        "{:?}",
        s.warnings
    );
    assert!(s.triangle_count() < 1000);
}

#[test]
fn nested_mapped_items_stop_at_the_budget() {
    // 18 levels of maps, each used twice: 2^18 boxes if fully expanded.
    let limits = Limits {
        max_triangles: 50_000,
        max_item_visits: 1_000_000,
    };
    let s = read_str_with_limits(&fixture("chain18.ifc"), limits).unwrap();
    assert!(s.triangle_count() <= 50_100, "{}", s.triangle_count());
    assert!(
        s.warnings.iter().any(|w| w.contains("too large")),
        "{:?}",
        s.warnings
    );

    let limits = Limits {
        max_triangles: 10_000_000,
        max_item_visits: 1_000,
    };
    let s = read_str_with_limits(&fixture("chain18.ifc"), limits).unwrap();
    assert!(
        s.warnings.iter().any(|w| w.contains("too large")),
        "{:?}",
        s.warnings
    );
}

#[test]
fn transform_operator_without_axis2_can_mirror() {
    // Axis1 = (-1,0,0), Axis2 = $: IfcSecondProjAxis gives y = (0,1,0), a mirror.
    let s = read_str(&fixture("mirror.ifc")).unwrap();
    assert_box(
        bbox(&s, "mapped"),
        ([-1000.0, 0.0, 0.0], [0.0, 500.0, 100.0]),
        0.01,
    );
    assert_box(
        bbox(&s, "derived"),
        ([0.0, 0.0, 0.0], [1000.0, 500.0, 100.0]),
        0.01,
    );
}

#[test]
fn axis2placement_default_x_follows_world_x_for_steep_axes() {
    let s = read_str(&fixture("tilt.ifc")).unwrap();
    assert_box(
        bbox(&s, "tilt"),
        ([-156.102, -50.0, -475.007], [251.104, 50.0, 506.228]),
        0.01,
    );
}
