//! Configure and build Linux, then the kmod, the static user binary and the initramfs. Native
//! only: the target is the host arch. Dependency tracking is kbuild's, except that a kernel build
//! producing a new image drops the kmod mirror (kbuild does not rebuild an external module after
//! a reconfig).

use std::fs;
use std::io::Write;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use kstep_core::qemu::{Boot, Machine, ARCH};

use crate::cmd::{cmd, run};
use crate::{build_dir, proj_dir, ResultDir};

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

fn mtime(p: &Path) -> Option<std::time::SystemTime> {
    fs::metadata(p).and_then(|m| m.modified()).ok()
}

fn append_once(path: &Path, line: &str) -> Result<()> {
    let text = fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
    if !text.lines().any(|l| l == line) {
        writeln!(fs::OpenOptions::new().append(true).open(path)?, "{line}")?;
    }
    Ok(())
}

impl Build {
    /// `build/<name>`, or what `build/current` points at. A missing tree is checked out when
    /// the name says where from: a bug's build (`<bug>_buggy`, `<bug>_fixed` in bugs.yaml) or
    /// a Linux release or commit (`v6.18`, `6.18.2`, `5068d84`). build/current is left alone.
    pub fn new(name: Option<&str>) -> Result<Build> {
        let name = match name {
            Some(n) => n.to_string(),
            None => fs::read_link(build_dir().join("current"))
                .context("no build/current; run `kstep checkout`")?
                .file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned(),
        };
        if !build_dir().join(&name).join("linux").exists() {
            let (git_ref, patch) = match crate::bugs::for_build(&name)? {
                Some(bug) => bug.kernel(&name).map(|k| (k.git_ref, k.patch))?,
                None if is_linux_ref(&name) => (name.clone(), None),
                None => bail!(
                    "no kernel tree at build/{name}/linux, and `{name}` is neither a bug's build \
(bugs.yaml) nor a Linux version; run `kstep checkout <ref> {name}` first"
                ),
            };
            crate::checkout(&git_ref, &name, patch.as_deref(), true, false)?;
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

    /// `make -C <linux> -jN`, the start of every kbuild invocation.
    fn make(&self) -> std::process::Command {
        let mut make = cmd("make", ["-C"]);
        make.arg(self.linux()).arg(jobs());
        make
    }

    /// A native boot of this build: `driver` on `machine`, the console and the JSON stream
    /// under `results`; with `terminal`, the console on the terminal too.
    pub fn boot(
        &self,
        driver: &str,
        machine: Machine,
        results: &ResultDir,
        terminal: bool,
    ) -> Boot {
        Boot {
            arch: ARCH,
            kernel: self.kernel(),
            rootfs: self.rootfs(),
            driver: driver.to_string(),
            machine,
            kernel_log: results.kernel_log(),
            kstep_socket: results.kstep_socket(),
            kstep_log: Some(results.kstep_log()),
            monitor_socket: results.monitor_socket(),
            terminal,
            debug: false,
            ram_file: None,
            snapshot: None,
        }
    }

    /// A snapshot of `boot` at its ready line to resume later boots of this build from
    /// (`Boot::snapshot`): `build/<name>/snap-<cpus>-<mem>.bin`, taken now unless one newer than
    /// the kernel and the initramfs is there, with that boot's kernel log as `.log` next to it
    /// (a resumed machine's own log starts after the ready line). The stream fits this machine
    /// size and QEMU.
    pub fn snapshot(&self, boot: &Boot) -> Result<PathBuf> {
        let m = boot.machine;
        let path = self
            .dir()
            .join(format!("snap-{}-{}.bin", m.num_cpus, m.mem_mb));
        let mtime = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        let fresh = mtime(&path).is_some_and(|t| {
            [self.kernel(), self.rootfs()]
                .iter()
                .all(|f| mtime(f).is_some_and(|k| k < t))
        });
        if !fresh {
            println!(
                "Snapshotting {} at the ready line to {}",
                self.name,
                path.display()
            );
            let mut cold = boot.clone();
            cold.snapshot = None;
            crate::Session::start(&cold, std::time::Duration::from_secs(120))?.snapshot(&path)?;
            fs::copy(&boot.kernel_log, path.with_extension("log"))?;
        }
        Ok(path)
    }

    /// Link the coverage files into `kernel/sched` and merge the config fragments: the common
    /// ones, the bug's if this is a bug's build, and `extra_config`.
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
        let bug_config = crate::bugs::for_build(&self.name)?
            .and_then(|b| b.config)
            .map(|c| proj_dir().join(c));
        for extra in bug_config.as_deref().into_iter().chain(extra_config) {
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
        let mut make = self.make();
        make.args([
            "KBUILD_BUILD_TIMESTAMP=1970-01-01",
            "KBUILD_BUILD_VERSION=1",
            &format!("LOCALVERSION=-{}", self.name),
            "WERROR=0",
            "HOSTCFLAGS=-Wno-error",
            "all",
            "compile_commands.json",
        ]);
        let image = self.linux().join(ARCH.kernel_image());
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
        match (
            mtime(&self.kernel()),
            mtime(&self.config()),
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
            self.make().args([&m, "modules", "compile_commands.json"]),
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

/// A release (`v6.18`, `6.18.2`, `v6.19-rc3`) or a commit hash (7+ hex digits).
fn is_linux_ref(name: &str) -> bool {
    let digits = |s: &str| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit());
    let ver = name.strip_prefix('v').unwrap_or(name);
    let (nums, rc) = ver.split_once("-rc").unwrap_or((ver, "1"));
    let release = nums.contains('.') && nums.split('.').all(digits) && digits(rc);
    let commit = (7..=40).contains(&name.len()) && name.bytes().all(|b| b.is_ascii_hexdigit());
    release || commit
}

#[test]
fn linux_refs() {
    for ok in [
        "v6.18",
        "6.18.2",
        "v7.0",
        "v6.19-rc3",
        "5068d84a",
        "c369299812",
    ] {
        assert!(is_linux_ref(ok), "{ok}");
    }
    for no in [
        "current",
        "foo",
        "sync_wakeup_buggy",
        "v6",
        "6.",
        "v6.18-rc",
        "abc",
    ] {
        assert!(!is_linux_ref(no), "{no}");
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
fn cpio_header_is_newc() {
    let h = cpio_header("user", 5);
    assert_eq!(h.len(), 116); // 6 + 13 * 8 + "user\0", padded to 4
    assert_eq!(&h[14..22], b"000081ed"); // mode 0100755
    assert_eq!(&h[54..62], b"00000005"); // filesize
    assert_eq!(&h[94..102], b"00000005"); // namesize
    assert_eq!(&h[110..115], b"user\0");
}
