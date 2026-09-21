//! Configure and build Linux, then the kmod, the static user binary and the initramfs. Native
//! only: the target is the host arch. Dependency tracking is kbuild's, except that every kernel
//! build drops the kmod mirror (kbuild does not rebuild an external module after a reconfig).

use std::fs;
use std::io::Write;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

use crate::cmd::{cmd, run};
use crate::{build_curr_dir, build_dir};

pub const ARCH: &str = std::env::consts::ARCH;

/// The image QEMU boots, relative to the kernel tree.
#[cfg(target_arch = "x86_64")]
pub const KERNEL_IMAGE: &str = "arch/x86/boot/bzImage";
#[cfg(target_arch = "aarch64")]
pub const KERNEL_IMAGE: &str = "arch/arm64/boot/Image";

/// One `build/<name>/` directory.
#[derive(Clone, Debug)]
pub struct Build {
    pub name: String,
}

impl Build {
    /// `build/<name>`, or what `build/current` points at; fails without a kernel tree there.
    pub fn new(name: Option<&str>) -> Result<Build> {
        let name = match name {
            Some(n) => n.to_string(),
            None => {
                let cur = fs::read_link(build_curr_dir())
                    .context("no build/current; run `kstep checkout`")?;
                cur.file_name().unwrap().to_string_lossy().into_owned()
            }
        };
        if !build_dir().join(&name).join("linux").exists() {
            let hint = if name.starts_with('v') {
                format!("kstep checkout {name}")
            } else {
                format!("kstep checkout <ref> {name}")
            };
            bail!("no kernel tree at build/{name}/linux; run `{hint}` first");
        }
        Ok(Build { name })
    }

    pub fn dir(&self) -> PathBuf {
        build_dir().join(&self.name)
    }
    pub fn linux(&self) -> PathBuf {
        self.dir().join("linux")
    }
    pub fn config(&self) -> PathBuf {
        self.linux().join(".config")
    }
    pub fn kernel(&self) -> PathBuf {
        self.dir().join("kernel")
    }
    /// Symlink mirror of `kmod/`: kbuild puts objects next to sources.
    pub fn kmod_dir(&self) -> PathBuf {
        self.dir().join("kmod")
    }
    pub fn kmod(&self) -> PathBuf {
        self.kmod_dir().join("kmod.ko")
    }
    pub fn rootfs(&self) -> PathBuf {
        self.dir().join("rootfs.cpio")
    }
}

/// The static user binary, shared by all builds.
pub fn user_bin() -> PathBuf {
    build_dir().join("user")
}

fn src(sub: &str) -> PathBuf {
    crate::proj_dir().join(sub)
}

fn jobs() -> String {
    format!(
        "-j{}",
        std::thread::available_parallelism().map_or(1, |n| n.get())
    )
}

fn append_once(path: &Path, line: &str) -> Result<()> {
    let text = fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
    if !text.lines().any(|l| l == line) {
        writeln!(fs::OpenOptions::new().append(true).open(path)?, "{line}")?;
    }
    Ok(())
}

/// Link the coverage files into `kernel/sched` and merge the config fragments.
pub fn configure(b: &Build, extra_config: Option<&Path>, log: Option<&Path>) -> Result<()> {
    let sched = b.linux().join("kernel/sched");
    for f in ["cov.c", "Kconfig.kstep", "Makefile.kstep"] {
        let link = sched.join(f);
        if fs::symlink_metadata(&link).is_ok() {
            fs::remove_file(&link)?;
        }
        symlink(src("linux").join(f), &link)?;
    }
    append_once(&sched.join("Makefile"), "include $(src)/Makefile.kstep")?;
    append_once(
        &b.linux().join("init/Kconfig"),
        "source \"kernel/sched/Kconfig.kstep\"",
    )?;

    let mut fragments = vec![
        src("linux/config.kstep"),
        src(&format!("linux/config.kstep.{ARCH}")),
    ];
    if let Some(extra) = extra_config {
        fragments.push(fs::canonicalize(extra)?);
    }
    run(
        cmd("./scripts/kconfig/merge_config.sh", ["-n"])
            .args(fragments)
            .current_dir(b.linux()),
        log,
    )
}

pub fn build_linux(
    b: &Build,
    extra_config: Option<&Path>,
    reconfigure: bool,
    log: Option<&Path>,
) -> Result<()> {
    if reconfigure || !b.config().exists() {
        configure(b, extra_config, log)?;
    }
    let mut make = cmd("make", ["-C"]);
    make.arg(b.linux()).arg(jobs()).args([
        "KBUILD_BUILD_TIMESTAMP=1970-01-01",
        "KBUILD_BUILD_VERSION=1",
        &format!("LOCALVERSION=-{}", b.name),
        "WERROR=0",
        "HOSTCFLAGS=-Wno-error",
        "all",
        "compile_commands.json",
    ]);
    run(&mut make, log)?;
    // fs::copy gives a fresh mtime, which kernel_stale compares with the .config's
    fs::copy(b.linux().join(KERNEL_IMAGE), b.kernel())?;
    fs::copy(b.linux().join("vmlinux"), b.dir().join("vmlinux"))?;
    let _ = fs::remove_dir_all(b.kmod_dir());
    Ok(())
}

/// Never built, or the .config changed since.
pub fn kernel_stale(b: &Build) -> bool {
    let mtime = |p: PathBuf| fs::metadata(p).and_then(|m| m.modified()).ok();
    match (
        mtime(b.kernel()),
        mtime(b.config()),
        b.linux().join("Module.symvers").exists(),
    ) {
        (Some(kernel), Some(config), true) => kernel < config,
        _ => true,
    }
}

pub fn build_user() -> Result<()> {
    let flags = [
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-std=c99",
        "-static",
        "-o",
    ];
    run(
        cmd("gcc", flags).arg(user_bin()).arg(src("user/user.c")),
        None,
    )
}

pub fn build_kmod(b: &Build) -> Result<()> {
    let dir = b.kmod_dir();
    fs::create_dir_all(&dir)?;
    remove_symlinks(&dir)?;
    let mut cp = cmd("cp", ["-rs"]);
    for entry in fs::read_dir(src("kmod"))? {
        cp.arg(entry?.path());
    }
    run(cp.arg(&dir), None)?;
    let m = format!("M={}", dir.display());
    run(
        cmd("make", ["-C"]).arg(b.linux()).arg(jobs()).args([
            &m,
            "modules",
            "compile_commands.json",
        ]),
        None,
    )
}

fn remove_symlinks(dir: &Path) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let p = entry?.path();
        let ft = fs::symlink_metadata(&p)?.file_type();
        if ft.is_symlink() {
            fs::remove_file(&p)?;
        } else if ft.is_dir() {
            remove_symlinks(&p)?;
        }
    }
    Ok(())
}

fn pad4(data: &mut Vec<u8>) {
    data.resize(data.len().next_multiple_of(4), 0);
}

/// A newc header: every file a root-owned 0755 regular file, so the archive is reproducible.
pub fn cpio_header(name: &str, size: usize) -> Vec<u8> {
    // ino, mode, uid, gid, nlink, mtime, filesize, devmajor, devminor, rdevmajor, rdevminor, namesize, check
    let fields = [0, 0o100755, 0, 0, 1, 0, size, 0, 0, 0, 0, name.len() + 1, 0];
    let mut out = b"070701".to_vec();
    for f in fields {
        out.extend(format!("{f:08x}").into_bytes());
    }
    out.extend(name.as_bytes());
    out.push(0);
    pad4(&mut out);
    out
}

pub fn build_rootfs(b: &Build) -> Result<()> {
    let mut out = Vec::new();
    for (name, path) in [("kmod.ko", b.kmod()), ("user", user_bin())] {
        let mut data = fs::read(&path).with_context(|| format!("read {}", path.display()))?;
        out.extend(cpio_header(name, data.len()));
        pad4(&mut data);
        out.extend(data);
    }
    out.extend(cpio_header("TRAILER!!!", 0));
    Ok(fs::write(b.rootfs(), out)?)
}

pub fn build_kstep(b: &Build) -> Result<()> {
    if kernel_stale(b) {
        eprintln!(
            "build/{}: kernel missing or older than its .config, building Linux first",
            b.name
        );
        build_linux(b, None, false, None)?; // keep the existing .config
    }
    build_user()?;
    build_kmod(b)?;
    build_rootfs(b)
}

#[test]
fn cpio_header_matches_make_py() {
    let h = cpio_header("user", 5);
    assert_eq!(h.len(), 116); // 6 + 13 * 8 + "user\0", padded to 4
    assert_eq!(&h[14..22], b"000081ed"); // mode 0100755
    assert_eq!(&h[54..62], b"00000005"); // filesize
    assert_eq!(&h[94..102], b"00000005"); // namesize
    assert_eq!(&h[110..115], b"user\0");
}
