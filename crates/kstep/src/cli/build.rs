use std::path::PathBuf;

use anyhow::Result;
use kstep::Build;

/// Build the kmod, the user binary and rootfs.cpio (and the kernel when needed)
#[derive(clap::Args)]
#[command(
    long_about = "Build the kmod, the user binary and rootfs.cpio. The kernel is built too when it is \
missing, older than its .config, or asked for with --linux.",
    after_help = "Examples:
  kstep build                         # build/current
  kstep build v6.18                   # kmod + user + rootfs for build/v6.18
  kstep build v6.18 --linux           # reconfigure and rebuild the kernel first
  kstep build --config linux/config.kstep.cov   # kernel with an extra Kconfig fragment"
)]
pub struct Args {
    /// Dir under build/ (default: build/current)
    name: Option<String>,
    /// Reconfigure and rebuild the kernel (run after editing the tree or the config fragments)
    #[arg(long)]
    linux: bool,
    /// Extra Kconfig fragment to merge; implies --linux
    #[arg(long, value_name = "FILE")]
    config: Option<PathBuf>,
}

pub fn main(a: Args) -> Result<()> {
    let b = Build::new(a.name.as_deref())?;
    if a.linux || a.config.is_some() {
        b.build_linux(a.config.as_deref(), true, None)?;
    }
    b.build_kstep()
}
