//! Configure and build Linux, then the kmod, the static user binary and the initramfs. Native
//! only: the target is the host arch. Dependency tracking is kbuild's, except that a kernel build
//! producing a new image drops the kmod mirror (kbuild does not rebuild an external module after
//! a reconfig).

use std::fs;
use std::io::Write;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use kstep_core::qemu::ARCH;

use crate::cmd::{cmd, run};
use crate::{build_dir, proj_dir};

/// One `build/<name>/` directory: a kernel tree and what kSTEP builds against it.
#[derive(Clone, Debug)]
pub struct Build {
    pub name: String,
}

/// The static user binary, shared by all builds.
pub fn user_bin() -> PathBuf {
    build_dir().join("user")
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

impl Build {
    /// `build/<name>`, or what `build/current` points at; fails without a kernel tree there.
    pub fn new(name: Option<&str>) -> Result<Build> {
        let name = match name {
            Some(n) => n.to_string(),
            None => {
                let cur = fs::read_link(build_dir().join("current"))
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
    pub fn kernel(&self) -> PathBuf {
        self.dir().join("kernel")
    }
    pub fn rootfs(&self) -> PathBuf {
        self.dir().join("rootfs.cpio")
    }
    fn config(&self) -> PathBuf {
        self.linux().join(".config")
    }
    /// Symlink mirror of `kmod/`: kbuild puts objects next to sources.
    fn kmod_dir(&self) -> PathBuf {
        self.dir().join("kmod")
    }

    /// Link the coverage files into `kernel/sched` and merge the config fragments.
    fn configure(&self, extra_config: Option<&Path>, log: Option<&Path>) -> Result<()> {
        let sched = self.linux().join("kernel/sched");
        for f in ["cov.c", "Kconfig.kstep", "Makefile.kstep"] {
            let link = sched.join(f);
            if fs::symlink_metadata(&link).is_ok() {
                fs::remove_file(&link)?;
            }
            symlink(proj_dir().join("linux").join(f), &link)?;
        }
        append_once(&sched.join("Makefile"), "include $(src)/Makefile.kstep")?;
        append_once(
            &self.linux().join("init/Kconfig"),
            "source \"kernel/sched/Kconfig.kstep\"",
        )?;

        let linux = proj_dir().join("linux");
        let mut fragments = vec![
            linux.join("config.kstep"),
            linux.join(format!("config.kstep.{}", ARCH.name())),
        ];
        if let Some(extra) = extra_config {
            fragments.push(fs::canonicalize(extra)?);
        }
        run(
            cmd("./scripts/kconfig/merge_config.sh", ["-n"])
                .args(fragments)
                .current_dir(self.linux()),
            log,
        )
    }

    /// Configure (unless told not to and a .config exists) and build the kernel; the image and
    /// vmlinux are copied out, and if the image changed the kmod mirror is dropped.
    pub fn build_linux(
        &self,
        extra_config: Option<&Path>,
        reconfigure: bool,
        log: Option<&Path>,
    ) -> Result<()> {
        if reconfigure || !self.config().exists() {
            self.configure(extra_config, log)?;
        }
        let mut make = cmd("make", ["-C"]);
        make.arg(self.linux()).arg(jobs()).args([
            "KBUILD_BUILD_TIMESTAMP=1970-01-01",
            "KBUILD_BUILD_VERSION=1",
            &format!("LOCALVERSION=-{}", self.name),
            "WERROR=0",
            "HOSTCFLAGS=-Wno-error",
            "all",
            "compile_commands.json",
        ]);
        let image = self.linux().join(ARCH.kernel_image());
        let mtime = |p: &Path| fs::metadata(p).and_then(|m| m.modified()).ok();
        let before = mtime(&image);
        run(&mut make, log)?;
        // fs::copy gives a fresh mtime, which kernel_stale compares with the .config's
        fs::copy(&image, self.kernel())?;
        fs::copy(self.linux().join("vmlinux"), self.dir().join("vmlinux"))?;
        // A new kernel invalidates the module's objects, which kbuild would not notice; a make
        // that had nothing to do leaves them.
        if mtime(&image) != before {
            let _ = fs::remove_dir_all(self.kmod_dir());
        }
        Ok(())
    }

    /// Never built, or the .config changed since.
    fn kernel_stale(&self) -> bool {
        let mtime = |p: PathBuf| fs::metadata(p).and_then(|m| m.modified()).ok();
        match (
            mtime(self.kernel()),
            mtime(self.config()),
            self.linux().join("Module.symvers").exists(),
        ) {
            (Some(kernel), Some(config), true) => kernel < config,
            _ => true,
        }
    }

    fn build_kmod(&self) -> Result<()> {
        let dir = self.kmod_dir();
        mirror(&proj_dir().join("kmod"), &dir)?;
        let m = format!("M={}", dir.display());
        run(
            cmd("make", ["-C"]).arg(self.linux()).arg(jobs()).args([
                &m,
                "modules",
                "compile_commands.json",
            ]),
            None,
        )
    }

    /// rootfs.cpio: kmod.ko and user, as a newc archive of root-owned 0755 files (reproducible).
    fn build_rootfs(&self) -> Result<()> {
        let mut out = Vec::new();
        for (name, path) in [
            ("kmod.ko", self.kmod_dir().join("kmod.ko")),
            ("user", user_bin()),
        ] {
            let mut data = fs::read(&path).with_context(|| format!("read {}", path.display()))?;
            out.extend(cpio_header(name, data.len()));
            pad4(&mut data);
            out.extend(data);
        }
        out.extend(cpio_header("TRAILER!!!", 0));
        Ok(fs::write(self.rootfs(), out)?)
    }

    /// What `kstep build` does: the kernel if it is stale (keeping its .config), then the rest.
    pub fn build_kstep(&self) -> Result<()> {
        if self.kernel_stale() {
            eprintln!(
                "build/{}: kernel missing or older than its .config, building Linux first",
                self.name
            );
            self.build_linux(None, false, None)?;
        }
        let flags = [
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-std=c99",
            "-static",
            "-o",
        ];
        run(
            cmd("gcc", flags)
                .arg(user_bin())
                .arg(proj_dir().join("user/user.c")),
            None,
        )?;
        self.build_kmod()?;
        self.build_rootfs()
    }
}

/// `dst` gets a symlink to every file under `src`; stale symlinks in `dst` are dropped first.
fn mirror(src: &Path, dst: &Path) -> Result<()> {
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(dst)? {
        let p = entry?.path();
        let ft = fs::symlink_metadata(&p)?.file_type();
        if ft.is_symlink() {
            fs::remove_file(&p)?;
        } else if ft.is_dir() {
            mirror(&src.join(p.file_name().unwrap()), &p)?;
        }
    }
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let target = dst.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            mirror(&entry.path(), &target)?;
        } else if fs::symlink_metadata(&target).is_err() {
            symlink(entry.path(), &target)?;
        }
    }
    Ok(())
}

fn pad4(data: &mut Vec<u8>) {
    data.resize(data.len().next_multiple_of(4), 0);
}

fn cpio_header(name: &str, size: usize) -> Vec<u8> {
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

#[test]
fn cpio_header_matches_make_py() {
    let h = cpio_header("user", 5);
    assert_eq!(h.len(), 116); // 6 + 13 * 8 + "user\0", padded to 4
    assert_eq!(&h[14..22], b"000081ed"); // mode 0100755
    assert_eq!(&h[54..62], b"00000005"); // filesize
    assert_eq!(&h[94..102], b"00000005"); // namesize
    assert_eq!(&h[110..115], b"user\0");
}
