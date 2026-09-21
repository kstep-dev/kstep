//! Shared by the `kstep` CLI, the fuzzer and (through wasm) the website: the shm decoder, the
//! QEMU command line and the bugs.yaml catalog. No clap, no LibAFL.

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

#[test]
fn proj_dir_holds_the_bug_catalog() {
    assert!(proj_dir().join("bugs.yaml").is_file());
}
