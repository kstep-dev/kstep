#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

from .consts import LATEST_COV_JSON, LINUX_BUILD_DIR, LINUX_ROOT_DIR, update_latest

COV_RECORD_SIZE = 24  # u32 pid + u32 cmd_id + u64 sig + u64 pc

def _parse_raw(payload: bytes) -> list[tuple[int, int, int, int]]:
    if len(payload) % COV_RECORD_SIZE != 0:
        raise ValueError(
            f"signal payload size {len(payload)} is not a multiple of {COV_RECORD_SIZE}"
        )
    records: list[tuple[int, int, int, int]] = []
    # Parse the signal records from the payload
    # Each record is 32 bytes: u32 pid + u32 cmd_id + u64 sig + u64 pc
    for i in range(0, len(payload), COV_RECORD_SIZE):
        cmd = int.from_bytes(payload[i : i + 4], byteorder="little", signed=False)
        pid = int.from_bytes(payload[i +  4 : i +  8], byteorder="little", signed=False)
        pc  = int.from_bytes(payload[i +  8 : i + 16], byteorder="little", signed=False)
        sig = int.from_bytes(payload[i + 16 : i + 24], byteorder="little", signed=False)

        records.append((cmd, pid, pc, sig))

    # Sort the records by cmd_id and pid
    records.sort(key=lambda rec: (rec[0], rec[1]))
    return records


def cov_parse(cov_file: Path) -> list[tuple[int, int, int, int]]:
    raw = cov_file.read_bytes()
    records = _parse_raw(raw) # Each record is a dict with cmd_id, pid, sig
    return records

def symbolize_pcs(pcs: list[int], linux_name: str) -> dict[int, tuple[str, str]]:
    vmlinux_path = LINUX_BUILD_DIR / f"{linux_name}.vmlinux"
    if not vmlinux_path.exists():
        raise RuntimeError(f"Missing vmlinux: {vmlinux_path}")

    proc = subprocess.run(
        ["addr2line", "-e", str(vmlinux_path), "-f", "-C"],
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

    out: dict[int, tuple[str, str]] = {}
    for i in range(0, 2 * len(pcs), 2):
        pc = pcs[i // 2]
        fn = lines[i].strip()
        loc = lines[i + 1].strip()
        assert "?" not in fn and "?" not in loc, f"fn contains ?: {fn}, loc contains ?: {loc}"

        # Keep only the path relative to the linux source directory.
        loc_path, sep, loc_suffix = loc.partition(":")
        linux_prefix = f"{LINUX_ROOT_DIR}/{linux_name}/"
        if linux_prefix in loc_path:
            loc_path = loc_path.rsplit(linux_prefix, 1)[1]
        else:
            raise RuntimeError(f"Linux prefix not found in loc: {loc}")
        loc = f"{loc_path}{sep}{loc_suffix}" if sep else loc_path
        
        out[pc] = (fn, loc)
    return out


def dump_pcs(records: list[tuple[int, int, int, int]], pc_symbolize: dict[int, tuple[str, str]], linux_name: str, output: Path):
    entries = []
    for cmd_id, pid, pc, _ in records:
        entries.append({
            "cmd_id": cmd_id,
            "pid": pid,
            "fn": pc_symbolize[pc][0],
            "loc": pc_symbolize[pc][1],
        })
    with output.open("w", encoding="utf-8") as f:
        json.dump({"Linux_name": linux_name, "entries": entries}, f, indent=2)


def cov_symbolize(cov_file, linux_name: str) -> None:
    output_file = Path(f"{cov_file.resolve()}.json")

    cov_entries = cov_parse(cov_file)

    if not cov_entries:
        raise RuntimeError("No KCOV PCs found in log")

    pc_symbolize = symbolize_pcs(list(set([rec[2] for rec in cov_entries])), linux_name)

    dump_pcs(cov_entries, pc_symbolize, linux_name, output_file)
    print(f"Wrote {len(cov_entries)} entries to {output_file}")

    update_latest(LATEST_COV_JSON, output_file)
