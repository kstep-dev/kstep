use std::path::PathBuf;

use anyhow::Result;

/// Fetch a Linux tree into build/<name>/linux and make it build/current
#[derive(clap::Args)]
#[command(after_help = "Examples:
  kstep checkout v6.18                        # kernel.org tarball into build/v6.18
  kstep checkout 6d7e478~1 foo_buggy --git    # worktree of build/master, any commit
  kstep checkout v6.14 --patch linux/sync_wakeup.patch --keep-current")]
pub struct Args {
    /// Linux tag or commit (e.g. v6.14, 6d7e478, 5068d84~1)
    git_ref: String,
    /// Build dir name under build/ (default: the ref, with ~ and ^ as -)
    name: Option<String>,
    /// Add a worktree of build/master instead of unpacking a tarball (git log/diff work)
    #[arg(long)]
    git: bool,
    /// Patch to apply to a fresh tree
    #[arg(long, value_name = "FILE")]
    patch: Option<PathBuf>,
    /// Leave build/current where it is
    #[arg(long)]
    keep_current: bool,
}

pub fn main(a: Args) -> Result<()> {
    let name = match &a.name {
        Some(n) => n.clone(),
        None => a.git_ref.replace(['~', '^'], "-"),
    };
    kstep_host::checkout(
        &a.git_ref,
        &name,
        a.patch.as_deref(),
        !a.git,
        !a.keep_current,
    )
}
