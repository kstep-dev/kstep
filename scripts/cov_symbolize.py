#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

from .consts import LATEST_COV_JSON, update_latest


def cov_parse_pcs(log_file: Path) -> list[tuple[int, int, int]]:
    bytes = log_file.read_bytes()
    return [
        (
            int.from_bytes(bytes[i : i + 4], byteorder="little"),
            int.from_bytes(bytes[i + 4 : i + 8], byteorder="little"),
            int.from_bytes(bytes[i + 8 : i + 16], byteorder="little"),
        )
        for i in range(0, len(bytes), 16)
    ]


def symbolize_pcs(vmlinux: Path, cov_entries: list[tuple[int, int, int]]) -> list[dict[str, str]]:
    proc = subprocess.run(
        ["addr2line", "-e", str(vmlinux), "-f", "-C"],
        input="".join(f"0x{entry[2]:x}\n" for entry in cov_entries),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )

    lines = proc.stdout.splitlines()

    if len(lines) < 2 * len(cov_entries):
        raise RuntimeError(
            f"addr2line output is shorter than expected: {len(lines)} lines for {len(cov_entries)} cov entries"
        )

    out: list[dict[str, str]] = []
    for i in range(0, 2 * len(cov_entries), 2):
        fn = lines[i].strip()
        loc = lines[i + 1].strip()
        pid = cov_entries[i // 2][0]
        cmd_id = cov_entries[i // 2][1]
        assert "?" not in fn and "?" not in loc, f"fn contains ?: {fn}, loc contains ?: {loc}"
        out.append(
            {
                "fn": fn,
                "loc": loc,
                "pid": str(pid),
                "cmd_id": str(cmd_id),
            }
        )
    return out


def dump_pcs(entries: list[dict[str, str]], output: Path):
    with output.open("w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)


def cov_symbolize(cov_file: Path, vmlinux: Path) -> None:
    output_file = Path(f"{cov_file.resolve()}.json")
    if not vmlinux.exists():
        raise RuntimeError(f"Missing vmlinux: {vmlinux}")

    cov_entries = cov_parse_pcs(cov_file)

    if not cov_entries:
        raise RuntimeError("No KCOV PCs found in log")

    entries = symbolize_pcs(vmlinux, cov_entries)

    dump_pcs(entries, output_file)
    print(f"Wrote {len(entries)} entries to {output_file}")
    update_latest(LATEST_COV_JSON, output_file)
