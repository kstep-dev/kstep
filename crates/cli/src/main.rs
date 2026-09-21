//! The `kstep` binary. Each subcommand replaces one of the Python entry points; the Python stays
//! in place until its Rust counterpart is verified, then is deleted.
//!
//!   kstep checkout   ./checkout.py   fetch a kernel tree into build/<name>/linux
//!   kstep build      ./make.py       configure and build the kernel, the kmod, user and rootfs
//!   kstep run        ./run.py        boot a driver under QEMU
//!   kstep reproduce  ./reproduce.py  buggy and fixed kernels of a bugs.yaml entry, then plot
//!   kstep web        website/build.py
//!
//! Fuzzing is the separate `kstep-fuzz` binary so that building this one never compiles LibAFL.

use anyhow::{bail, Result};
use clap::{Parser, Subcommand};

mod build;
mod checkout;

#[derive(Parser)]
#[command(name = "kstep", version, about, long_about = None)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Fetch a Linux tree into build/<name>/linux (tarball or worktree)
    Checkout(checkout::CheckoutArgs),
    /// Build the kernel, the kmod, the user binary and rootfs.cpio
    Build(build::BuildArgs),
    /// Boot a driver under QEMU and record its output under results/
    Run,
    /// Check out, build and run a bugs.yaml entry on its buggy and fixed kernels, then plot
    Reproduce,
    /// Build the website: bug catalog, playground image and the wasm decoder
    Web,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let name = match cli.cmd {
        Cmd::Checkout(args) => return checkout::main(args),
        Cmd::Build(args) => return build::main(args),
        Cmd::Run => "run",
        Cmd::Reproduce => "reproduce",
        Cmd::Web => "web",
    };
    bail!(
        "`kstep {name}` is not ported yet; use the Python script it replaces (see {})",
        kstep_core::proj_dir().join("README.md").display()
    )
}
