#!/usr/bin/env python3
"""Build kSTEP: the Linux kernel, the kmod, the user binary, and the initramfs.

    ./make.py [kstep] [--build B]               # kmod + user + rootfs.cpio (default)
    ./make.py linux   [--build B] [--config F]  # configure + build the kernel
    ./make.py clean   [--build B] [--all]

Everything is built for the host's arch (x86_64 or aarch64), natively. Dependency tracking is
kbuild's: `kstep` builds the kernel only if it is missing or older than its .config, and every
kernel build rebuilds the kmod from scratch. Edits to the config fragments or inside
build/<B>/linux go through `make.py linux`.
"""

import argparse
import logging
import os
import shutil
from dataclasses import dataclass
from pathlib import Path

from scripts import BUILD_CURR_DIR, BUILD_DIR, PROJ_DIR, system

# The target is the host: no cross-compiling, and one QEMU machine per arch (see run.py).
ARCH = os.uname().machine
KERNEL_IMAGE = {"x86_64": "arch/x86/boot/bzImage", "aarch64": "arch/arm64/boot/Image"}[ARCH]


@dataclass(frozen=True)
class Build:
    name: str | None = None  # dir under build/; defaults to build/current
    log: Path | None = None  # append command output here instead of the console

    def __post_init__(self):
        name = self.name or BUILD_CURR_DIR.resolve().name
        if not (BUILD_DIR / name / "linux").exists():
            raise SystemExit(f"No kernel tree at build/{name}/linux; run ./checkout.py")
        object.__setattr__(self, "name", name)

    @property
    def dir(self) -> Path:
        return BUILD_DIR / self.name

    @property
    def linux(self) -> Path:
        return self.dir / "linux"

    @property
    def config(self) -> Path:
        return self.linux / ".config"

    @property
    def kernel(self) -> Path:
        return self.dir / "kernel"

    @property
    def kmod_dir(self) -> Path:  # symlink mirror of kmod/; kbuild puts objects next to sources
        return self.dir / "kmod"

    @property
    def kmod(self) -> Path:
        return self.kmod_dir / "kmod.ko"

    @property
    def rootfs(self) -> Path:
        return self.dir / "rootfs.cpio"

    @property
    def user(self) -> Path:
        return BUILD_DIR / "user"

    def run(self, cmd: str, cwd: Path = PROJ_DIR):
        system(cmd, cwd=cwd, log=self.log)

    def kbuild(self, args: str):
        self.run(f"make -j{os.cpu_count()} {args}", cwd=self.linux)


KSTEP_CONFIG = PROJ_DIR / "linux" / "config.kstep"
COV_FILES = [PROJ_DIR / "linux" / f for f in ("cov.c", "Kconfig.kstep", "Makefile.kstep")]


def append_once(path: Path, line: str):
    if line not in path.read_text().splitlines():
        with path.open("a") as f:
            f.write(line + "\n")


def configure(b: Build, extra_config: Path | None = None):
    sched = b.linux / "kernel" / "sched"
    for f in COV_FILES:
        link = sched / f.name
        link.unlink(missing_ok=True)
        link.symlink_to(f)
    append_once(sched / "Makefile", "include $(src)/Makefile.kstep")
    append_once(b.linux / "init" / "Kconfig", 'source "kernel/sched/Kconfig.kstep"')

    fragments = [KSTEP_CONFIG, Path(f"{KSTEP_CONFIG}.{ARCH}")]
    if extra_config:
        fragments.append(extra_config.resolve())
    b.run(f"./scripts/kconfig/merge_config.sh -n {' '.join(map(str, fragments))}", cwd=b.linux)


def build_linux(b: Build, extra_config: Path | None = None, *, reconfigure: bool = True):
    if reconfigure or not b.config.exists():
        configure(b, extra_config)
    b.kbuild(
        f"KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION=1 "
        f"LOCALVERSION=-{b.name} WERROR=0 HOSTCFLAGS=-Wno-error all compile_commands.json"
    )
    # copy, not copy2: kernel_stale() needs the image's mtime
    shutil.copy(b.linux / KERNEL_IMAGE, b.kernel)
    shutil.copy(b.linux / "vmlinux", b.dir / "vmlinux")
    # kbuild does not rebuild an external module after a kernel reconfig
    shutil.rmtree(b.kmod_dir, ignore_errors=True)


# Never built, or .config changed since (hand edit, interrupted `make.py linux`)
def kernel_stale(b: Build) -> bool:
    if not all(p.exists() for p in (b.kernel, b.config, b.linux / "Module.symvers")):
        return True
    return b.kernel.stat().st_mtime < b.config.stat().st_mtime


def build_user(b: Build):
    b.run(f"gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o {b.user} {PROJ_DIR / 'user' / 'user.c'}")


def build_kmod(b: Build):
    b.kmod_dir.mkdir(exist_ok=True)
    for p in b.kmod_dir.rglob("*"):
        if p.is_symlink():
            p.unlink()
    b.run(f"cp -rs {PROJ_DIR / 'kmod'}/* {b.kmod_dir}")
    b.kbuild(f"M={b.kmod_dir} modules compile_commands.json")


def pad4(data: bytes) -> bytes:
    return data + b"\0" * (-len(data) % 4)


def cpio_header(name: str, size: int) -> bytes:
    """A newc-format header, as the kernel unpacks it (Documentation/driver-api/early-userspace/
    buffer-format.rst). Every file is a root-owned 0755 regular file: the archive is reproducible."""
    # ino, mode, uid, gid, nlink, mtime, filesize, devmajor, devminor, rdevmajor, rdevminor, namesize, check
    header = "070701" + "%08x" * 13 % (0, 0o100755, 0, 0, 1, 0, size, 0, 0, 0, 0, len(name) + 1, 0)
    return pad4((header + name + "\0").encode())


def build_rootfs(b: Build):
    with b.rootfs.open("wb") as f:
        for name, path in [("kmod.ko", b.kmod), ("user", b.user)]:
            data = path.read_bytes()
            f.write(cpio_header(name, len(data)) + pad4(data))
        f.write(cpio_header("TRAILER!!!", 0))


def build_kstep(b: Build):
    if kernel_stale(b):
        build_linux(b, reconfigure=False)  # keep the existing .config
    build_user(b)
    build_kmod(b)
    build_rootfs(b)


def clean(b: Build, full: bool = False):
    shutil.rmtree(b.kmod_dir, ignore_errors=True)
    b.rootfs.unlink(missing_ok=True)
    if full:
        b.user.unlink(missing_ok=True)
        b.kbuild("clean")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", dest="name", help="dir under build/ (default: build/current)")
    sub = parser.add_subparsers(dest="cmd")
    parser.set_defaults(cmd="kstep")
    sub.add_parser("kstep", help="build kmod, user, and rootfs.cpio (and the kernel if missing)")
    sub.add_parser("linux", help="configure and build the kernel").add_argument(
        "--config", type=Path, help="extra Kconfig fragment to merge (e.g. linux/config.kstep.cov)"
    )
    sub.add_parser("clean", help="remove kmod and rootfs outputs").add_argument(
        "--all", action="store_true", help="also the user binary and kbuild objects"
    )
    args = parser.parse_args()

    b = Build(args.name)
    logging.info(f"======= BUILD: {b.name} =======")
    if args.cmd == "linux":
        build_linux(b, args.config)
    elif args.cmd == "kstep":
        build_kstep(b)
    elif args.cmd == "clean":
        clean(b, args.all)
