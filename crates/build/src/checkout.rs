//! `build/<name>/linux` from a kernel.org/GitHub tarball or a worktree of `build/master`.

use std::fs;
use std::os::unix::fs::symlink;
use std::path::Path;

use anyhow::{Context, Result};

use crate::cmd::{cmd, run};
use crate::{build_curr_dir, build_dir};

pub const LINUX_MASTER_URL: &str = "https://github.com/gregkh/linux.git";

/// Releases (`v6.14`, `6.14.2`) come from kernel.org's CDN, commits from GitHub.
pub fn download_url(git_ref: &str) -> String {
    if git_ref.contains('.') {
        let ver = git_ref.strip_prefix('v').unwrap_or(git_ref);
        let major = ver.split('.').next().unwrap();
        format!("https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{ver}.tar.xz")
    } else {
        format!("https://github.com/torvalds/linux/archive/{git_ref}.tar.gz")
    }
}

fn add_worktree(git_ref: &str, linux_dir: &Path) -> Result<()> {
    let master = build_dir().join("master");
    if !master.exists() {
        run(
            cmd("git", ["clone", "--filter=blob:none", LINUX_MASTER_URL]).arg(&master),
            None,
        )?;
    }
    run(cmd("git", ["fetch"]).current_dir(&master), None)?;
    run(
        cmd("git", ["worktree", "prune", "-v"]).current_dir(&master),
        None,
    )?;
    run(
        cmd("git", ["worktree", "add"])
            .arg(linux_dir)
            .arg(git_ref)
            .current_dir(&master),
        None,
    )
}

pub fn set_current_build(name: &str) -> Result<()> {
    let current = build_curr_dir();
    fs::create_dir_all(build_dir())?;
    if fs::symlink_metadata(&current).is_ok() {
        fs::remove_file(&current)?;
    }
    symlink(build_dir().join(name), &current)
        .with_context(|| format!("symlink {}", current.display()))
}

pub fn download(url: &str, output: &Path) -> Result<()> {
    if output.exists() {
        return Ok(());
    }
    eprintln!("Downloading {url}");
    run(cmd("wget", ["--no-verbose", url, "-O"]).arg(output), None)
}

pub fn decompress(tarball: &Path, output_dir: &Path) -> Result<()> {
    if output_dir.exists() {
        return Ok(());
    }
    fs::create_dir_all(output_dir)?;
    run(
        cmd("tar", ["-xf"])
            .arg(tarball)
            .arg("-C")
            .arg(output_dir)
            .arg("--strip-components=1"),
        None,
    )
}

pub fn patch_linux(linux_dir: &Path, patch: &Path) -> Result<()> {
    let patch = fs::File::open(patch).with_context(|| format!("open {}", patch.display()))?;
    run(
        cmd("patch", ["-p1", "--forward", "--batch"])
            .stdin(patch)
            .current_dir(linux_dir),
        None,
    )
}

pub fn checkout(
    git_ref: &str,
    name: &str,
    patch: Option<&Path>,
    tarball: bool,
    set_current: bool,
) -> Result<()> {
    let dir = build_dir().join(name);
    let linux_dir = dir.join("linux");
    if linux_dir.exists() {
        eprintln!("Reusing build/{name}/linux");
    } else {
        fs::create_dir_all(&dir)?;
        if tarball {
            let tarball_path = dir.join(format!("{git_ref}.tar.xz"));
            download(&download_url(git_ref), &tarball_path)?;
            decompress(&tarball_path, &linux_dir)?;
        } else {
            add_worktree(git_ref, &linux_dir)?;
        }
        if let Some(patch) = patch {
            patch_linux(&linux_dir, patch)?;
        }
    }
    if set_current {
        set_current_build(name)?;
        eprintln!("Build `build/{name}` (Linux {git_ref}) is now current");
    } else {
        eprintln!("Build `build/{name}` (Linux {git_ref}) checked out");
    }
    Ok(())
}

#[test]
fn urls_match_checkout_py() {
    assert_eq!(
        download_url("v6.14"),
        "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.14.tar.xz"
    );
    assert_eq!(
        download_url("5.15.2"),
        "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.15.2.tar.xz"
    );
    assert_eq!(
        download_url("c3692998"),
        "https://github.com/torvalds/linux/archive/c3692998.tar.gz"
    );
}
