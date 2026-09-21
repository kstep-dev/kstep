//! The `kstep` binary: checkout, build, run, reproduce, viz. The fuzzer is its own crate
//! (crates/fuzz) on top of this package's library, so this one never compiles LibAFL.

use anyhow::Result;
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
    Viz(cli::viz::Args),
}

fn main() -> Result<()> {
    match Cli::parse() {
        Cli::Checkout(args) => cli::checkout::main(args),
        Cli::Build(args) => cli::build::main(args),
        Cli::Run(args) => cli::run::main(args),
        Cli::Gdb(args) => cli::run::gdb(args),
        Cli::Reproduce(args) => cli::reproduce::main(args),
        Cli::Viz(args) => cli::viz::main(args),
    }
}
