//! The `kstep` binary: checkout, build, run, reproduce, web. Fuzzing is the separate
//! `kstep-fuzz` binary so this one never compiles LibAFL.

use anyhow::{bail, Result};
use clap::Parser;

mod build;
mod checkout;
mod run;

#[derive(Parser)]
#[command(name = "kstep", bin_name = "kstep", version, about)]
enum Cli {
    Checkout(checkout::Args),
    Build(build::Args),
    Run(run::Args),
    Gdb(run::GdbArgs),
    /// Check out, build and run a bugs.yaml entry on its buggy and fixed kernels, then plot
    Reproduce,
    /// Build the website: bug catalog, playground image and the wasm decoder
    Web,
}

fn main() -> Result<()> {
    match Cli::parse() {
        Cli::Checkout(args) => checkout::main(args),
        Cli::Build(args) => build::main(args),
        Cli::Run(args) => run::main(args),
        Cli::Gdb(args) => run::gdb(args),
        Cli::Reproduce => bail!("not ported yet: use ./reproduce.py"),
        Cli::Web => bail!("not ported yet: use website/build.py"),
    }
}
