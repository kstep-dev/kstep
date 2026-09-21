use std::path::PathBuf;

use anyhow::Result;

#[derive(clap::Args)]
pub struct Args {
    /// Linux tag or commit (e.g. v6.14, 6d7e478, 5068d84~1)
    #[arg(default_value = "v6.18")]
    git_ref: String,
    /// Build dir name under build/ (default: the ref)
    name: Option<String>,
    /// Download a tarball from kernel.org / GitHub [default]
    #[arg(long, conflicts_with = "git")]
    tar: bool,
    /// Add a worktree of build/master instead (git log/diff work)
    #[arg(long)]
    git: bool,
    /// Patch to apply to a fresh tree
    #[arg(long)]
    patch: Option<PathBuf>,
    /// Leave build/current alone
    #[arg(long)]
    no_current: bool,
}

pub fn main(a: Args) -> Result<()> {
    let name = a.name.as_deref().unwrap_or(&a.git_ref);
    kstep_build::checkout(&a.git_ref, name, a.patch.as_deref(), !a.git, !a.no_current)
}
