#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

def parse_kcov_pcs(log_file: Path) -> list[int]:
    lines = log_file.read_text(encoding="utf-8", errors="ignore").splitlines()
    pcs: list[int] = []

    for line in lines:
        # a JSON line with "fork_kcov_pcs": ["ffffffff...", ..., "END"]
        if line.startswith("{") and '"fork_kcov_pcs"' in line:
            obj = json.loads(line)
            raw_pcs = obj.get("fork_kcov_pcs")
            for pc in raw_pcs:
                if pc.startswith("END"):
                    break
                pcs.append(int(pc, 16))
            continue

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


def kcov_symbolize(kstep_out_file: Path, linux_dir: Path, output_file: Path) -> None:
    vmlinux = linux_dir / "vmlinux"
    if not vmlinux.exists():
        raise RuntimeError(f"Missing vmlinux: {vmlinux}")

    pcs = parse_kcov_pcs(kstep_out_file)
    if not pcs:
        raise RuntimeError("No KCOV PCs found in log")

    entries = symbolize_pcs(vmlinux, pcs)

    if output_file is not None:
        dump_pcs(entries, output_file)
        print(f"Wrote {len(entries)} entries to {output_file}")
    
    return

