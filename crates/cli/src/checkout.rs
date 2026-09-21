//! `kstep checkout`: what `checkout.py` did.

use std::path::PathBuf;

use anyhow::Result;
use clap::Args;

#[derive(Args)]
pub struct CheckoutArgs {
    /// Linux tag or commit (e.g. v6.14, 6d7e478, 5068d84~1)
    #[arg(default_value = "v6.18")]
    git_ref: String,
    /// Name of the build dir under build/ (default: the ref)
    name: Option<String>,
    /// Download a tarball from kernel.org / GitHub (fast, one-shot) [default]
    #[arg(long, conflicts_with = "git")]
    tar: bool,
    /// Add a worktree of build/master (multi-version dev; git log/diff work)
    #[arg(long)]
    git: bool,
    /// Patch to apply to a fresh tree
    #[arg(long)]
    patch: Option<PathBuf>,
    /// Leave build/current alone
    #[arg(long = "no-current")]
    no_current: bool,
}

pub fn main(args: CheckoutArgs) -> Result<()> {
    let name = args.name.as_deref().unwrap_or(&args.git_ref);
    kstep_build::checkout(
        &args.git_ref,
        name,
        args.patch.as_deref(),
        !args.git,
        !args.no_current,
    )
}
