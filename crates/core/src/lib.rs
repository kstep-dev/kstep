//! kSTEP's host-side library, shared by the `kstep` CLI, the fuzzer, and (through wasm) the
//! website playground. It holds the single implementation of everything that used to exist
//! twice: the QEMU command line, the decoder for the guest's shared region (kmod/shm.h), and the
//! bugs.yaml catalog. Modules arrive one step at a time; see the workspace Cargo.toml.
//!
//! Nothing here depends on clap or LibAFL: the CLI and the fuzzer are clients.

use std::path::{Path, PathBuf};

/// The repository root: the directory holding `bugs.yaml`, `kmod/`, `build/` and `results/`.
/// Found from the running binary's manifest at compile time (this crate lives in `crates/core`),
/// or overridden with `KSTEP_DIR`, so a binary copied elsewhere still finds its checkout.
pub fn proj_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("KSTEP_DIR") {
        return PathBuf::from(dir);
    }
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(2)
        .expect("crates/core has a grandparent")
        .to_path_buf()
}

#[cfg(test)]
mod tests {
    #[test]
    fn proj_dir_holds_the_bug_catalog() {
        assert!(super::proj_dir().join("bugs.yaml").is_file());
    }
}
