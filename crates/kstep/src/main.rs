//! The `kstep` binary: checkout, build, run, reproduce, web. The fuzzer is a second binary in
//! this package behind the `fuzz` feature, so this one never compiles LibAFL.

use anyhow::{bail, Result};
use clap::Parser;

mod cli;

#[derive(Parser)]
#[command(name = "kstep", bin_name = "kstep", version, about)]
enum Cli {
    Checkout(cli::checkout::Args),
    Build(cli::build::Args),
    Run(cli::run::Args),
    Gdb(cli::run::GdbArgs),
    Reproduce(cli::reproduce::Args),
    /// Build the website: bug catalog, playground image and the wasm decoder
    Web,
}

fn main() -> Result<()> {
    match Cli::parse() {
        Cli::Checkout(args) => cli::checkout::main(args),
        Cli::Build(args) => cli::build::main(args),
        Cli::Run(args) => cli::run::main(args),
        Cli::Gdb(args) => cli::run::gdb(args),
        Cli::Reproduce(args) => cli::reproduce::main(args),
        Cli::Web => bail!("not ported yet: use website/build.py"),
    }
}
