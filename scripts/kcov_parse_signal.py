#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

SIG_SIZE = 8  # u64 signal values
SIG_RECORD_SIZE = 16  # u32 pid + u32 cmd_id + u64 sig

def _parse_raw(payload: bytes):
    if len(payload) % SIG_RECORD_SIZE != 0:
        raise ValueError(
            f"signal payload size {len(payload)} is not a multiple of {SIG_RECORD_SIZE}"
        )
    records: list[dict[str, int]] = []
    for offset in range(0, len(payload), SIG_RECORD_SIZE):
        pid = int.from_bytes(payload[offset : offset + 4], byteorder="little", signed=False)
        cmd_id = int.from_bytes(payload[offset + 4 : offset + 8], byteorder="little", signed=False)
        sig = int.from_bytes(payload[offset + 8 : offset + 16], byteorder="little", signed=False)
        records.append({"cmd_id": cmd_id, "pid": pid, "sig": sig})
    records.sort(key=lambda rec: (rec["cmd_id"], rec["pid"]))
    return records


def parse_signal_file(signal_file: Path, output: Path | None = None, dump: bool = False) -> tuple[list[dict[str, int]], list[int]]:
    raw = signal_file.read_bytes()
    records = _parse_raw(raw)
    records_signals = [rec["sig"] for rec in records]

    out_path = output or Path(f"{signal_file}.json")
    if dump:
        out_path.write_text(json.dumps(records, indent=2), encoding="utf-8")
        print(f"Wrote {len(records)} records to {out_path}")
    return records, records_signals

def main() -> None:
    parser = argparse.ArgumentParser(description="Parse kSTEP signal file.")
    parser.add_argument("signal_file", type=Path, help="Path to signal file (binary, 8 bytes per record)")
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
