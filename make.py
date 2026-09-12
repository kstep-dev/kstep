#!/usr/bin/env python3
"""Build kSTEP: the Linux kernel, the kmod, the user binary, and the initramfs.

    ./make.py [kstep] [--build B] [--arch A]               # kmod + user + rootfs.cpio (default)
    ./make.py linux   [--build B] [--arch A] [--config F]  # configure + build the kernel
    ./make.py clean   [--build B] [--all]

A build dir is bound to one target arch, recorded in build/<B>/arch when the kernel is first
configured; `--arch` is only needed on the first build. There is no dependency tracking:
kbuild does its own, and every other step takes seconds.
"""

import argparse
import logging
import os
import shutil
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

from scripts import BUILD_CURR_DIR, BUILD_DIR, PROJ_DIR, system

HOST_ARCH = os.uname().machine


class Arch(StrEnum):
    X86_64 = "x86_64"
    AARCH64 = "aarch64"

    @property
    def kbuild(self) -> str:
        return {Arch.X86_64: "x86_64", Arch.AARCH64: "arm64"}[self]

    @property
    def image(self) -> str:
        return {Arch.X86_64: "arch/x86/boot/bzImage", Arch.AARCH64: "arch/arm64/boot/Image"}[self]

    @property
    def cross(self) -> str:
        return "" if self == HOST_ARCH else f"{self}-linux-gnu-"

    @property
    def qemu(self) -> str:
        return f"qemu-system-{self}"


@dataclass(frozen=True)
class Build:
    name: str | None = None  # dir under build/; defaults to build/current
    arch: Arch | None = None  # defaults to the dir's stamp, else x86_64
    log: Path | None = None  # append command output here instead of the console

    def __post_init__(self):
        name = self.name or BUILD_CURR_DIR.resolve().name
        if not (BUILD_DIR / name / "linux").exists():
            raise SystemExit(f"No kernel tree at build/{name}/linux; run ./checkout.py")
        stamp = BUILD_DIR / name / "arch"
        stamped = Arch(stamp.read_text().strip()) if stamp.exists() else None
        arch = self.arch or stamped or Arch.X86_64
        if stamped and arch != stamped:
            raise SystemExit(f"build/{name} was built for {stamped}, not {arch}; use another build name")
        object.__setattr__(self, "name", name)
        object.__setattr__(self, "arch", arch)

    @property
    def dir(self) -> Path:
        return BUILD_DIR / self.name

    @property
    def linux(self) -> Path:
        return self.dir / "linux"

    @property
    def kmod(self) -> Path:
        return self.dir / "kmod" / "kmod.ko"

    @property
    def rootfs(self) -> Path:
        return self.dir / "rootfs.cpio"

    @property
    def user(self) -> Path:
        return BUILD_DIR / f"user.{self.arch}"

    @property
    def kbuild_env(self) -> str:
        return f"ARCH={self.arch.kbuild} CROSS_COMPILE={self.arch.cross}"

    def run(self, cmd: str, cwd: Path = PROJ_DIR):
        system(cmd, cwd=cwd, log=self.log)

    def kbuild(self, args: str):
        self.run(f"make -j{os.cpu_count()} {self.kbuild_env} {args}", cwd=self.linux)


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

    fragments = [KSTEP_CONFIG, Path(f"{KSTEP_CONFIG}.{b.arch}")]
    if extra_config:
        fragments.append(extra_config.resolve())
    b.run(f"{b.kbuild_env} ./scripts/kconfig/merge_config.sh -n {' '.join(map(str, fragments))}", cwd=b.linux)
    (b.dir / "arch").write_text(f"{b.arch}\n")


def build_linux(b: Build, extra_config: Path | None = None):
    configure(b, extra_config)
    b.kbuild(
        f"KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION=1 "
        f"LOCALVERSION=-{b.name} WERROR=0 HOSTCFLAGS=-Wno-error all compile_commands.json"
    )
    shutil.copy2(b.linux / b.arch.image, b.dir / "kernel")
    shutil.copy2(b.linux / "vmlinux", b.dir / "vmlinux")


def build_user(b: Build):
    src = PROJ_DIR / "user" / "user.c"
    b.run(f"{b.arch.cross}gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o {b.user} {src}")


def build_kmod(b: Build):
    # A symlink mirror of kmod/ so kbuild's objects stay in build/
    mirror = b.dir / "kmod"
    # kbuild does not rebuild an external module's objects after the kernel is reconfigured, and
    # objects compiled under the old config (inlined preempt counting, struct layouts) then link
    # against a kernel that disagrees. autoconf.h is rewritten on every reconfiguration: drop the
    # objects that predate it.
    autoconf = b.linux / "include" / "generated" / "autoconf.h"
    objects = list(mirror.rglob("*.o"))
    if objects and min(o.stat().st_mtime for o in objects) < autoconf.stat().st_mtime:
        shutil.rmtree(mirror)
    mirror.mkdir(exist_ok=True)
    for p in mirror.rglob("*"):
        if p.is_symlink():
            p.unlink()
    b.run(f"cp -rs {PROJ_DIR / 'kmod'}/* {mirror}")
    b.kbuild(f"M={mirror} modules compile_commands.json")


def build_rootfs(b: Build):
    staging = b.dir / "rootfs"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir()
    shutil.copy2(b.kmod, staging / "kmod.ko")
    shutil.copy2(b.user, staging / "user")
    for p in [staging, *staging.iterdir()]:
        os.utime(p, (0, 0))
    b.run(f"find . | sort | cpio -o --format=newc --reproducible --quiet > {b.rootfs}", cwd=staging)


def build_kstep(b: Build):
    if not (b.linux / "Module.symvers").exists():
        build_linux(b)
    build_user(b)
    build_kmod(b)
    build_rootfs(b)


def clean(b: Build, full: bool = False):
    for p in (b.dir / "kmod", b.dir / "rootfs"):
        shutil.rmtree(p, ignore_errors=True)
    b.rootfs.unlink(missing_ok=True)
    if full:
        b.user.unlink(missing_ok=True)
        b.kbuild("clean")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", dest="name", help="dir under build/ (default: build/current)")
    parser.add_argument("--arch", type=Arch, choices=Arch, help="target arch (default: the build dir's, else x86_64)")
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

    b = Build(args.name, args.arch)
    logging.info(f"======= BUILD: {b.name} ({b.arch}) =======")
    if args.cmd == "linux":
        build_linux(b, args.config)
    elif args.cmd == "kstep":
        build_kstep(b)
    elif args.cmd == "clean":
        clean(b, args.all)
