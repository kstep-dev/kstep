//! `kstep build`: what `make.py` did.

use std::path::PathBuf;

use anyhow::Result;
use clap::{Args, Subcommand};
use kstep_build::{build, Build};

#[derive(Args)]
pub struct BuildArgs {
    /// dir under build/ (default: build/current)
    #[arg(long = "build", value_name = "NAME")]
    name: Option<String>,
    #[command(subcommand)]
    cmd: Option<BuildCmd>,
}

#[derive(Subcommand)]
enum BuildCmd {
    /// Build kmod, user, and rootfs.cpio (and the kernel if missing) [default]
    Kstep,
    /// Configure and build the kernel
    Linux {
        /// Extra Kconfig fragment to merge (e.g. linux/config.kstep.cov)
        #[arg(long)]
        config: Option<PathBuf>,
    },
    /// Remove kmod and rootfs outputs
    Clean {
        /// Also the user binary and kbuild objects
        #[arg(long)]
        all: bool,
    },
}

pub fn main(args: BuildArgs) -> Result<()> {
    let b = Build::new(args.name.as_deref())?;
    eprintln!("======= BUILD: {} =======", b.name);
    match args.cmd.unwrap_or(BuildCmd::Kstep) {
        BuildCmd::Kstep => build::build_kstep(&b),
        BuildCmd::Linux { config } => build::build_linux(&b, config.as_deref(), true, None),
        BuildCmd::Clean { all } => build::clean(&b, all),
    }
}
