// =============================================================================
// Cargo build script for razobill-vector VRL crate
//
// locates native build directory, instructs rustc to link with librazobill,
// and stages razobill.dll into target/deps on Windows for automatic test runner
// discovery
// =============================================================================

use std::env;
use std::fs;
use std::path::PathBuf;

/// configure linker flags and stage shared libraries for test discovery
/// inv: points rustc link search path to cmake build directory
fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let build_dir = manifest_dir
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("build");

    // instruct cargo to link against native razobill shared library
    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=dylib=razobill_cabi");

    // copy razobill_cabi.dll into target/deps on Windows so cargo test finds it
    if cfg!(target_os = "windows") {
        let dll_src = build_dir.join("razobill_cabi.dll");
        if dll_src.exists() {
            if let Ok(out_dir) = env::var("OUT_DIR") {
                let out_path = PathBuf::from(out_dir);
                // out_path is target/{profile}/build/{crate-hash}/out
                // ancestors: 0=out, 1=crate-hash, 2=build, 3=debug/release
                if let Some(target_profile_dir) = out_path.ancestors().nth(3) {
                    let _ = fs::copy(&dll_src, target_profile_dir.join("razobill_cabi.dll"));
                    let deps_dir = target_profile_dir.join("deps");
                    let _ = fs::create_dir_all(&deps_dir);
                    let _ = fs::copy(&dll_src, deps_dir.join("razobill_cabi.dll"));
                }
            }
        }
    }

    // track library binaries to trigger relink on cmake rebuild
    println!(
        "cargo:rerun-if-changed={}",
        build_dir.join("librazobill_cabi.dll.a").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        build_dir.join("razobill_cabi.dll").display()
    );
}
