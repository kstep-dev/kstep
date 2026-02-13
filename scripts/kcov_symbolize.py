#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

from .consts import LATEST_COV_JSON, update_latest


def parse_kcov_pcs(log_file: Path) -> list[int]:
    bytes = log_file.read_bytes()
    return [
        int.from_bytes(bytes[i : i + 8], byteorder="little")
        for i in range(0, len(bytes), 8)
    ]


def symbolize_pcs(vmlinux: Path, pcs: list[int]) -> list[dict[str, str]]:
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


def kcov_symbolize(cov_file: Path, vmlinux: Path) -> None:
    output_file = Path(f"{cov_file.resolve()}.json")
    if not vmlinux.exists():
        raise RuntimeError(f"Missing vmlinux: {vmlinux}")

    pcs = parse_kcov_pcs(cov_file)
    if not pcs:
        raise RuntimeError("No KCOV PCs found in log")

    entries = symbolize_pcs(vmlinux, pcs)

    dump_pcs(entries, output_file)
    print(f"Wrote {len(entries)} entries to {output_file}")
    update_latest(LATEST_COV_JSON, output_file)
