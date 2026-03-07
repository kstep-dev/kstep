#!/usr/bin/env python3

import subprocess
from collections import defaultdict
from pathlib import Path

from .consts import BUILD_DIR, LINUX_ROOT_DIR

COV_RECORD_SIZE = 16  # u32 pid + u32 cmd_id + u64 pc

def cov_parse(cov_file: Path) -> dict[int, dict[int, list[int]]]:
    payload = cov_file.read_bytes()
    if len(payload) % COV_RECORD_SIZE != 0:
        raise ValueError(
            f"signal payload size {len(payload)} is not a multiple of {COV_RECORD_SIZE}"
        )
    records: dict[int, dict[int, list[int]]] = {}
    records = defaultdict(lambda: defaultdict(list))
    # Parse the signal records from the payload
    # Each record is 16 bytes: u32 pid + u32 cmd_id + u64 pc
    for i in range(0, len(payload), COV_RECORD_SIZE):
        cmd = int.from_bytes(payload[i : i + 4], byteorder="little", signed=False)
        pid = int.from_bytes(payload[i +  4 : i +  8], byteorder="little", signed=False)
        pc  = int.from_bytes(payload[i +  8 : i + 16], byteorder="little", signed=False)
        records[cmd][pid].append(pc)

    return records

def symbolize_pcs(pcs: list[int], linux_name: str) -> dict[int, tuple[str, str]]:
    vmlinux_path = BUILD_DIR / linux_name / "vmlinux"
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
        if "?" in fn:
            fn += f":{pc}"
        if "?" in loc:
            loc += f":{pc}"

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
