#!/usr/bin/env python3

import argparse
import json
import subprocess
from pathlib import Path

from .consts import LATEST_COV, LATEST_COV_JSON, LINUX_CURR_DIR, update_latest


def parse_kcov_pcs(log_file: Path) -> list[int]:
    raw = log_file.read_bytes()
    usable_len = len(raw) - (len(raw) % 8)
    pcs: list[int] = []

    for i in range(0, usable_len, 8):
        pcs.append(int.from_bytes(raw[i : i + 8], byteorder="little"))

    return pcs


def symbolize_pcs(vmlinux: Path, pcs: list[int]) -> list[dict[str, str]]:
    if not pcs:
        return []

    proc = subprocess.run(
        ["addr2line", "-e", str(vmlinux), "-f", "-C"],
        input="".join(f"0x{pc:x}\n" for pc in pcs),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )

    lines = proc.stdout.splitlines()
    
    if len(lines) < 2 * len(pcs):
        raise RuntimeError(
            f"addr2line output is shorter than expected: {len(lines)} lines for {len(pcs)} pcs"
        )

    out: list[dict[str, str]] = []
    for i in range(0, 2 * len(pcs), 2):
        fn = lines[i].strip() or "??"
        loc = lines[i + 1].strip() or "??:0"
        if "kernel/sched" in loc:
            out.append({"fn": fn, "loc": loc})
    return out


def dump_pcs(entries: list[dict[str, str]], output: Path):
    with output.open("w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)


def kcov_symbolize(cov_file: Path, linux_dir: Path) -> None:
    output_file = Path(f"{cov_file.resolve()}.json")
    vmlinux = linux_dir / "vmlinux"
    if not vmlinux.exists():
        raise RuntimeError(f"Missing vmlinux: {vmlinux}")

    pcs = parse_kcov_pcs(cov_file)
    if not pcs:
        raise RuntimeError("No KCOV PCs found in log")

    entries = symbolize_pcs(vmlinux, pcs)

    dump_pcs(entries, output_file)
    print(f"Wrote {len(entries)} entries to {output_file}")
    update_latest(LATEST_COV_JSON, output_file)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("cov_file", type=Path, default=LATEST_COV, nargs="?")
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    args = parser.parse_args()

    kcov_symbolize(cov_file=args.cov_file, linux_dir=args.linux_dir)


if __name__ == "__main__":
    main()
