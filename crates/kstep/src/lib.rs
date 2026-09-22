//! What the `kstep` binary and the fuzzer share: kernel checkout and build, bugs.yaml, and the
//! results/ convention. Nothing here is built for wasm; that part is kstep-core.

pub mod bugs;
pub mod build;
pub mod checkout;
pub mod cmd;
pub mod results;
pub mod session;

pub use build::Build;
pub use checkout::checkout;
pub use results::ResultDir;
pub use session::Session;

use std::path::{Path, PathBuf};

/// The repo root (`KSTEP_DIR` overrides the compiled-in location).
pub fn proj_dir() -> PathBuf {
    match std::env::var_os("KSTEP_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(2)
            .unwrap()
            .to_path_buf(),
    }
}

pub fn build_dir() -> PathBuf {
    proj_dir().join("build")
}

#[test]
fn proj_dir_holds_the_bug_catalog() {
    assert!(proj_dir().join("bugs.yaml").is_file());
}
