use std::path::PathBuf;

use anyhow::Result;
use clap::Subcommand;
use kstep_build::{build, Build};

#[derive(clap::Args)]
pub struct Args {
    /// Dir under build/ (default: build/current)
    #[arg(long = "build", value_name = "NAME")]
    name: Option<String>,
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// kmod + user + rootfs.cpio, and the kernel if missing [default]
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

pub fn main(a: Args) -> Result<()> {
    let b = Build::new(a.name.as_deref())?;
    eprintln!("======= BUILD: {} =======", b.name);
    match a.cmd.unwrap_or(Cmd::Kstep) {
        Cmd::Kstep => build::build_kstep(&b),
        Cmd::Linux { config } => build::build_linux(&b, config.as_deref(), true, None),
        Cmd::Clean { all } => build::clean(&b, all),
    }
}
