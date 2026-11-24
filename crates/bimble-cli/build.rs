//! Windows gives the main thread 1 MB of stack; Linux and macOS give 8 MB. The CLI runs on the
//! main thread (winit needs it for `bimble view`), so give the `bimble` binary 8 MB on Windows too.
//! `rustc-link-arg-bins` applies to binary targets only, not to tests or examples.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let env = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    if target_os == "windows" {
        if env == "msvc" {
            println!("cargo:rustc-link-arg-bins=/STACK:8388608");
        } else {
            println!("cargo:rustc-link-arg-bins=-Wl,--stack,8388608");
        }
    }
}
