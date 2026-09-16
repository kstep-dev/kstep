#!/usr/bin/env -S uv run --script

import argparse
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from checkout import Linux, checkout
from make import Build, build_kstep, build_linux
from run import Driver, run_qemu
from scripts import (
    PROJ_DIR,
    ResultDir,
    TermColor,
    system,
)


BUGS_FILE = PROJ_DIR / "bugs.yaml"


@dataclass(frozen=True)
class Bug:
    """One bugs.yaml entry: how to build, run, reproduce and fuzz a bug (fields documented there)."""
    name: str
    # Commit-based: buggy = fix~1, fixed = fix
    fix: str | None = None
    # Patch-based: buggy = ref, fixed = ref + patch
    ref: str | None = None
    patch: str | None = None
    # Extra kernel config fragment to merge
    config: Path | None = None
    extra: bool = False
    plot_format: str | None = None
    # The machine (run.py Driver fields)
    num_cpus: int = 2
    mem_mb: int = 128  # kSTEP itself needs ~20 MB
    # Fuzzing cli material (checks, verbs, setup), see bugs.yaml
    fuzz: dict = field(default_factory=dict)

    @property
    def machine(self) -> dict:
        return {k: getattr(self, k) for k in ("num_cpus", "mem_mb")}

    @property
    def driver(self) -> Driver:
        return Driver(name=self.name, **self.machine)

    @property
    def linux(self) -> list[Linux]:
        if self.fix:
            return [
                Linux(name="buggy", ref=f"{self.fix}~1", config=self.config),
                Linux(name="fixed", ref=self.fix, config=self.config),
            ]
        elif self.ref and self.patch:
            patch_path = PROJ_DIR / "linux" / self.patch
            return [
                Linux(name="buggy", ref=self.ref, config=self.config),
                Linux(
                    name="fixed",
                    ref=self.ref,
                    patch=patch_path,
                    config=self.config,
                ),
            ]
        else:
            raise ValueError(
                f"Bug '{self.name}': specify either 'fix' or 'ref'+'patch'"
            )

    def print_facts(self):
        """One `key value` line per fact, for the fuzzer."""
        print("name", self.name)
        for k, v in self.machine.items():
            if v is not None:
                print(k, v)
        for k in ("checks", "verbs", "setup"):
            for line in self.fuzz.get(k, []):
                print(k.rstrip("s") if k != "setup" else k, line)


def load_bugs() -> dict[str, Bug]:
    return {name: Bug(name=name, **(entry or {})) for name, entry in yaml.safe_load(BUGS_FILE.read_text()).items()}


ALL_BUGS = load_bugs()
BUGS = [b for b in ALL_BUGS.values() if not b.extra]
BUGS_EXTRA = [b for b in ALL_BUGS.values() if b.extra]


def log_step(prefix: str, message: str):
    print(f"{TermColor.GREEN}{prefix}{TermColor.RESET}: {message}", flush=True)


def plot_data(python_script: str, driver: str):
    log_step(driver, "Plotting data")
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py {driver}")


def reproduce(bug: "Bug", linux: Linux):
    kernel = f"{bug.name}_{linux.name}"

    log_step(kernel, "Checkout Linux")
    checkout(linux.ref, kernel=kernel, patch=linux.patch, tarball=True, set_current=False)
    b = Build(kernel)
    log = b.dir / "build.log"
    log_step(kernel, f"Build Linux (log: {log})")
    build_linux(b, extra_config=linux.config, log=log)
    log_step(kernel, "Build kSTEP")
    build_kstep(b)

    result_dir = ResultDir.create(f"repro_{bug.name}/{linux.name}")
    log_step(kernel, "Run kSTEP")
    log_step(kernel, f"QEMU Log: {result_dir.log}")
    log_step(kernel, f"kSTEP Output: {result_dir.output}")
    run_qemu(kernel=kernel, driver=bug.driver, result_dir=result_dir, headless=True)


def main(bug: Bug, runs: list[str]):
    print("=" * 80, flush=True)
    print(f" {bug.name} ".center(80, "="), flush=True)
    print("=" * 80, flush=True)

    linux_map = {linux.name: linux for linux in bug.linux}
    for r in runs:
        if r == "plot":
            continue
        linux = linux_map.get(r, Linux(name=r, ref=r))
        reproduce(bug, linux)

    if "plot" in runs:
        if bug.plot_format:
            plot_data(bug.plot_format, bug.name)
        else:
            print(f"Plot format not specified for bug '{bug.name}'")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "name",
        type=str,
        default="all",
        choices=["all", "extra", *ALL_BUGS],
        help="The name of the bug to reproduce, or 'all' to reproduce all BUGS.",
    )
    parser.add_argument(
        "--run",
        type=str,
        default=["buggy", "fixed", "plot"],
        nargs="+",
    )
    parser.add_argument("--print", action="store_true", help="print the bug's facts from bugs.yaml and exit (the fuzzer reads them)")
    args = parser.parse_args()

    if args.print:
        ALL_BUGS[args.name].print_facts()
        raise SystemExit

    if args.name == "all":
        selected_bugs = BUGS
    elif args.name == "extra":
        selected_bugs = BUGS_EXTRA
    else:
        selected_bugs = [ALL_BUGS[args.name]]

    print(f"running {len(selected_bugs)} bug(s)")
    for bug in selected_bugs:
        main(bug=bug, runs=args.run)
