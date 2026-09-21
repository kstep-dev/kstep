//! Kernel checkout and build (the old checkout.py and make.py). Host tooling, never in the
//! wasm build, hence not in kstep-core.

pub mod build;
pub mod checkout;
pub mod cmd;

pub use build::Build;
pub use checkout::checkout;

use std::path::PathBuf;

pub fn build_dir() -> PathBuf {
    kstep_core::proj_dir().join("build")
}

/// The `build/current` symlink, set by `kstep checkout`.
pub fn build_curr_dir() -> PathBuf {
    build_dir().join("current")
}
