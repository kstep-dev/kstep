#!/usr/bin/env python3

from pathlib import Path

SIG_SIZE = 8  # u64 signal values
SIG_RECORD_SIZE = 16  # u32 pid + u32 cmd_id + u64 sig

def _parse_raw(payload: bytes):
    if len(payload) % SIG_RECORD_SIZE != 0:
        raise ValueError(
            f"signal payload size {len(payload)} is not a multiple of {SIG_RECORD_SIZE}"
        )
    records: list[dict[str, int]] = []
    # Parse the signal records from the payload
    # Each record is 16 bytes: u32 pid + u32 cmd_id + u64 sig
    for i in range(0, len(payload), SIG_RECORD_SIZE):
        pid = int.from_bytes(payload[i : i + 4], byteorder="little", signed=False)
        cmd = int.from_bytes(payload[i + 4 : i + 8], byteorder="little", signed=False)
        sig = int.from_bytes(payload[i + 8 : i + 16], byteorder="little", signed=False)

        records.append({"cmd_id": cmd, "pid": pid, "sig": sig})

    # Sort the records by cmd_id and pid
    records.sort(key=lambda rec: (rec["cmd_id"], rec["pid"]))
    return records


def cov_parse_signal(signal_file: Path) -> tuple[list[dict[str, int]], list[int]]:
    raw = signal_file.read_bytes()
    records = _parse_raw(raw) # Each record is a dict with cmd_id, pid, sig
    signal_list = [rec["sig"] for rec in records] # List of only signal values
    return records, signal_list
