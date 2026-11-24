//! `bimble view` must open its event loop on the main thread: winit refuses any other thread
//! (it checks the thread before it looks for a display). With no display the viewer fails, but
//! it must fail for lack of a display, not with a panic about the thread.
//! Fixture: the demo frame (synthetic), made by `bimble demo` into a temporary directory.
#![cfg(target_os = "linux")]

use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[test]
fn view_runs_on_the_main_thread() {
    let bin = env!("CARGO_BIN_EXE_bimble");
    let dir = std::env::temp_dir().join(format!("bimble-view-main-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let model = dir.join("frame.bimble");

    let demo = Command::new(bin).arg("demo").arg(&model).output().unwrap();
    assert!(demo.status.success(), "bimble demo failed: {demo:?}");

    let mut child = Command::new(bin)
        .arg("view")
        .arg(&model)
        .env_remove("WAYLAND_DISPLAY")
        .env_remove("DISPLAY")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let start = Instant::now();
    let status = loop {
        if let Some(s) = child.try_wait().unwrap() {
            break s;
        }
        if start.elapsed() > Duration::from_secs(20) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = std::fs::remove_dir_all(&dir);
            panic!("`bimble view` with no display was still running after 20 s; killed it");
        }
        std::thread::sleep(Duration::from_millis(50));
    };
    let out = child.wait_with_output().unwrap();
    let stderr = String::from_utf8_lossy(&out.stderr);
    let _ = std::fs::remove_dir_all(&dir);

    assert!(
        !stderr.contains("outside of the main thread"),
        "the event loop was created off the main thread (status {status}):\n{stderr}"
    );
    assert!(
        !stderr.contains("panicked"),
        "`bimble view` panicked (status {status}):\n{stderr}"
    );
}
