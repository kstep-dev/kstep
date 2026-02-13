#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

def parse_kcov_pcs(log_file: Path) -> tuple[list[int], list[int]]:
    lines = log_file.read_text(encoding="utf-8", errors="ignore").splitlines()
    user_pcs: list[int] = []
    kmod_pcs: list[int] = []

    for line in lines:
        if line.startswith("{") and any(key in line for key in ["user_kcov_pcs", "kmod_kcov_pcs"]):
            obj = json.loads(line)
            for pc in obj.get("user_kcov_pcs", []):
                if pc.startswith("END"):
                    break
                user_pcs.append(int(pc, 16))
            for pc in obj.get("kmod_kcov_pcs", []):
                if pc.startswith("END"):
                    break
                kmod_pcs.append(int(pc, 16))

    return user_pcs, kmod_pcs

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

    user_pcs, kmod_pcs = parse_kcov_pcs(kstep_out_file)

    user_entries = symbolize_pcs(vmlinux, user_pcs)
    kmod_entries = symbolize_pcs(vmlinux, kmod_pcs)

    if output_file is not None:
        user_output_file = Path(f"{output_file}.user")
        kmod_output_file = Path(f"{output_file}.module")
        dump_pcs(user_entries, user_output_file)
        dump_pcs(kmod_entries, kmod_output_file)
        print(f"Wrote {len(user_entries)} entries to {user_output_file}")
        print(f"Wrote {len(kmod_entries)} entries to {kmod_output_file}")
    
    return
