import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .consts import CORPUS_DIR
from .gen_input_ops import OP_NAME_TO_TYPE

class SignalCorpus:
    def __init__(self):
        self.seen: set[int] = set()
        CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    def seq_digest(self, seq: Iterable[tuple[int, int, int, int]]) -> str:
        return hashlib.sha256(json.dumps(seq, separators=(",", ":")).encode("utf-8")).hexdigest()

    def analyze_new_signals(
        self,
        seq: Iterable[tuple[int, int, int, int]],
        signal_records: set[int],
        linux_version: str,
    ) -> set[int] | None:
        new = set(signal_records) - self.seen
        if not new:
            print("signal: no new signals found")
            return None

        self.seen.update(new)

        digest = self.seq_digest(seq)
        entry_path = CORPUS_DIR / f"{digest}.json"

        entry = {
            "seq": seq, # input sequence
            "new_signals": sorted(new), # new signals in the signal file
            "linux_version": linux_version,
        }

        print(f"Signal: found {len(new)} new signals, dumping to {entry_path}")
        with open(entry_path, "w", encoding="utf-8") as f:
            json.dump(entry, f, indent=2)

        return new

    def analyze_per_task_signals(
        self,
        seq: Iterable[tuple[int, int, int, int]], # input sequence
        cmd_task_signals: list[dict[str, int]], # cmd_id -> pid -> set of new signals, sorted by cmd_id and pid
        new: set[int], # new signals
    ) -> dict[int, dict] | None:
        op_type_to_name = {v: k for k, v in OP_NAME_TO_TYPE.items()}
        tick_repeat = OP_NAME_TO_TYPE["TICK_REPEAT"]
        cmd_meta: dict[int, dict] = {}
        cmd_id = 0
        for op_type, a, b, c in seq:
            common = {"new_signals": {}}  # pid -> set of new signals
            entries: list[tuple[str, list[int]]] = []
            if op_type == tick_repeat:
                entries.extend((f"TICK_REPEAT_{i}", [0, 0, 0]) for i in range(a))
            else:
                entries.append((op_type_to_name.get(op_type, f"OP_{op_type}"), [a, b, c]))

            for cmd_name, cmd_args in entries:
                cmd_id += 1

                cmd_meta[cmd_id] = {
                    "cmd_id": cmd_id,
                    "cmd_name": cmd_name,
                    "cmd_args": cmd_args,
                    **common,
                }
                
        for rec in cmd_task_signals:
            if rec["cmd_id"] not in cmd_meta:
                raise ValueError(f"command {rec["cmd_id"]} not found in cmd_meta")
            if rec["sig"] in new:
                cmd_meta[rec["cmd_id"]]["new_signals"].setdefault(rec["pid"], list[int]()).append(rec["sig"])
                new.remove(rec["sig"])

        digest = self.seq_digest(seq)
        print(f"Signal: dumping per-task new signals to {CORPUS_DIR / f'{digest}_per_task.json'}")
        with open(CORPUS_DIR / f"{digest}_per_task.json", "w", encoding="utf-8") as f:
            json.dump(cmd_meta, f, indent=2)

        return cmd_meta


GLOBAL_SIGNAL_CORPUS = SignalCorpus()
