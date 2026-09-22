use std::path::PathBuf;

use anyhow::Result;
use kstep::Build;

/// Build the kmod, the user binary and rootfs.cpio (and the kernel when needed)
#[derive(clap::Args)]
#[command(
    long_about = "Build the kmod, the user binary and rootfs.cpio. The kernel is built too when it is \
missing, older than its .config, or asked for with --linux. A bug's build (<bug>_buggy, <bug>_fixed) \
or a Linux version is checked out first if it is missing; `lts` builds every supported LTS release.",
    after_help = "Examples:
  kstep build                         # build/current
  kstep build v6.18                   # kmod + user + rootfs for build/v6.18 (checked out if missing)
  kstep build freeze_buggy            # the bug's kernel from bugs.yaml, checked out if missing
  kstep build lts                     # every supported LTS release (v5.15 ... v6.18)
  kstep build v6.18 --linux           # reconfigure and rebuild the kernel first
  kstep build --config linux/config.kstep.cov   # kernel with an extra Kconfig fragment"
)]
pub struct Args {
    /// Dir under build/ (default: build/current), or `lts` for every supported LTS release
    name: Option<String>,
    /// Reconfigure and rebuild the kernel (run after editing the tree or the config fragments)
    #[arg(long)]
    linux: bool,
    /// Extra Kconfig fragment to merge; implies --linux
    #[arg(long, value_name = "FILE")]
    config: Option<PathBuf>,
}

pub fn main(a: Args) -> Result<()> {
    let names: Vec<Option<&str>> = match a.name.as_deref() {
        Some("lts") => kstep::checkout::LTS.iter().map(|v| Some(*v)).collect(),
        name => vec![name],
    };
    for name in names {
        let b = Build::new(name)?;
        if a.linux || a.config.is_some() {
            b.build_linux(a.config.as_deref(), true, None)?;
        }
        b.build_kstep()?;
    }
    Ok(())
}
