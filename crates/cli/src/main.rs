//! The `kstep` binary: checkout, build, run, reproduce, web. Fuzzing is the separate
//! `kstep-fuzz` binary so this one never compiles LibAFL.

use anyhow::{bail, Result};
use clap::Parser;

mod build;
mod checkout;

#[derive(Parser)]
#[command(name = "kstep", version, about)]
enum Cli {
    /// Fetch a Linux tree into build/<name>/linux (tarball or worktree)
    Checkout(checkout::Args),
    /// Build the kernel, the kmod, the user binary and rootfs.cpio
    Build(build::Args),
    /// Boot a driver under QEMU and record its output under results/
    Run,
    /// Check out, build and run a bugs.yaml entry on its buggy and fixed kernels, then plot
    Reproduce,
    /// Build the website: bug catalog, playground image and the wasm decoder
    Web,
}

fn main() -> Result<()> {
    match Cli::parse() {
        Cli::Checkout(args) => checkout::main(args),
        Cli::Build(args) => build::main(args),
        Cli::Run => bail!("not ported yet: use ./run.py"),
        Cli::Reproduce => bail!("not ported yet: use ./reproduce.py"),
        Cli::Web => bail!("not ported yet: use website/build.py"),
    }
}
