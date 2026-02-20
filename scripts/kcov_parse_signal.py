#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

HEADER_SIZE = 12  # u32 pid, u32 cmd_id, u32 vector_len

def parse_records(raw: bytes) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    offset = 0
    total = len(raw)
    # Format: PID, CMD_ID, N, SIG0, SIG1, ..., SIGN-1
    while offset < total:
        if offset + HEADER_SIZE > total:
            raise ValueError(f"truncated header at offset {offset}")
        pid = int.from_bytes(raw[offset : offset + 4], byteorder="little", signed=False)
        cmd_id = int.from_bytes(raw[offset + 4 : offset + 8], byteorder="little", signed=False)
        vec_len = int.from_bytes(raw[offset + 8 : offset + 12], byteorder="little", signed=False)
        offset += HEADER_SIZE
        vec_bytes = vec_len * 8
        if offset + vec_bytes > total:
            raise ValueError(f"truncated vector at offset {offset}, len {vec_len}")
        for i in range(vec_len):
            base = offset + i * 8
            sig = int.from_bytes(raw[base : base + 8], byteorder="little", signed=False)
            out.append((f"{cmd_id}-{pid}", sig))
        offset += vec_bytes
    return out

def aggregate_by_key(records: list[tuple[str, int]]) -> dict[str, list[int]]:
    agg: dict[str, list[int]] = {}
    for key, sig in records:
        agg.setdefault(key, []).append(sig)
    return agg

def parse_signal_file(signal_file: Path, output: Path | None = None) -> Path:
    raw = signal_file.read_bytes()
    records = parse_records(raw)
    agg = aggregate_by_key(records)

    out_path = output or Path(f"{signal_file}.json")
    out_path.write_text(json.dumps(agg, indent=2), encoding="utf-8")
    print(f"Wrote {len(agg)} records to {out_path}")
    return out_path


def main() -> None:
    parser = argparse.ArgumentParser(description="Parse kSTEP signal file and aggregate by cmd_id.")
    parser.add_argument("signal_file", type=Path, help="Path to signal file (binary, 16 bytes per record)")
    parser.add_argument(
        "-o",
        "--out",
        type=Path,
        default=None,
        help="Output JSON path (default: <signal_file>.json)",
    )
    args = parser.parse_args()

    parse_signal_file(args.signal_file, args.out)


if __name__ == "__main__":
    main()
