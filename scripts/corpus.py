import json
from typing import Optional
from pathlib import Path
from collections import defaultdict
from .consts import CORPUS_DIR
from .gen_input_ops import OP_NAME_TO_TYPE, OP_TYPE_TO_NAME
from .input_seq import InputSeq

from .cov import symbolize_pcs

def pc_hash(pc: int) -> int:
    a = (pc ^ 61) ^ (pc >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2d;
    a = a ^ (a >> 15);
    return a

class SignalCorpus:
    def __init__(self):
        self.seen_signals: set[int] = set()
        self.seen_pcs: dict[int, tuple[str, str]] = {}
        CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    def update_pc_symbolize(self, 
        signal_records: dict[int, dict[int, list[int]]],
        linux_name: str,
    ):
        # new_pcs = {rec[3] for rec in signal_records if rec[3] not in self.seen_pcs}
        new_pcs = set()
        for pid_pcs in signal_records.values():
            for pcs in pid_pcs.values():
                for pc in pcs:
                    if pc not in self.seen_pcs:
                        new_pcs.add(pc)
        pc_symbolize = symbolize_pcs(list(new_pcs), linux_name)
        for pc, (fn, loc) in pc_symbolize.items():
            self.seen_pcs[pc] = (fn, loc)

    # Analyze the new signals for a test, return the new signals if any
    def analyze_new_signals(
        self,
        seq: InputSeq,
        signal_records: dict[int, dict[int, list[int]]],
        linux_name: str,
        output_path: Optional[Path] = None,
    ) -> set[int] | None:
        # signal_list = [rec[4] for rec in signal_records]
        # Check if the records have been seen before or not
        signal_list = []
        
        for pid_pcs in signal_records.values():
            for pcs in pid_pcs.values():
                for i in range(len(pcs)):
                    pc = pcs[i]
                    prev_pc = pcs[i - 1] if i > 0 else 0
                    sig = pc ^ (pc_hash(prev_pc) & 0xfff)
                    signal_list.append(sig)

        new = set(signal_list) - self.seen_signals

        if not new:
            print("signal: no new signals found")
            return None
        self.seen_signals.update(new)

        # Dump the test into the corpus directory if it hits new signals
        data = {
            "seq": seq.to_list(), # input sequence
            "new_signals": sorted(new), # new signals in the signal file
            "linux_name": linux_name,
        }

        if output_path is None:
            output_path = CORPUS_DIR / f"{seq.digest()}.json"
        print(f"Wrote {len(new)} new edges to {output_path}")
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        return new

    # Analyze the per-action (each command each pid's action is an action) signals for a test
    # Save the new signal hit by each task
    # If a task hits a new signal, the following tasks hit the same signal will not be recorded
    def analyze_per_action_signals(
        self,
        seq: InputSeq,
        signal_records: dict[int, dict[int, list[int]]], # each entry is a signal record, cmd_id -> pid -> list of pcs
        new_signals: set[int],
        linux_name: str,
        output_path: Optional[Path] = None,
    ) -> list[dict]:
        tick_repeat = OP_NAME_TO_TYPE["TICK_REPEAT"]
        cmd_meta: list[dict] = []
        cmd_id = 0

        # Initialize the command metadata for each command in the sequence
        # For tick_repeat, create multiple entries for each tick
        for op_type, a, b, c in seq:
            cmd_entries: list[tuple[str, list[int]]] = []
            if op_type == tick_repeat:
                cmd_entries.extend((f"TICK_REPEAT_{i}", [0, 0, 0]) for i in range(a))
            else:
                cmd_entries.append((OP_TYPE_TO_NAME[op_type], [a, b, c]))

            for cmd_name, cmd_args in cmd_entries:
                cmd_meta.append({
                    "cmd_id": cmd_id,
                    "cmd_name": cmd_name,
                    "cmd_args": cmd_args,
                    "new_edges": defaultdict(list), # pid -> [(sig, (fn, loc))]
                })
                cmd_id += 1

        # Add the signal to the new signals for the action if the task hits that new signal
        # Remove the signal from the new signals set if it has been recorded
        # So that the following tasks hit the same signal will not be recorded        
        for cmd_id, pid_pcs in signal_records.items():
            for pid, pcs in pid_pcs.items():
                for i in range(len(pcs)):
                    pc = pcs[i]
                    prev_pc = pcs[i - 1] if i > 0 else 0
                    sig = pc ^ (pc_hash(prev_pc) & 0xfff)

                    if sig in new_signals:
                        if prev_pc != 0:
                            cmd_meta[cmd_id]["new_edges"][pid].append(
                                (f"{self.seen_pcs[prev_pc][0]}:{self.seen_pcs[prev_pc][1]}",
                                f"{self.seen_pcs[pc][0]}:{self.seen_pcs[pc][1]}")
                            )
                        else:
                            cmd_meta[cmd_id]["new_edges"][pid].append(
                                ("", f"{self.seen_pcs[pc][0]}:{self.seen_pcs[pc][1]}")
                            )
                        new_signals.remove(sig)

        # Dump the per-action new signals to the corpus directory
        if output_path is None:
            output_path = CORPUS_DIR / f"{seq.digest()}_per_action.json" 
        print(f"Wrote {len(cmd_meta)} per-action new edges to {output_path}")
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump({"Linux_name": linux_name, "cmd_meta": cmd_meta}, f, indent=2)

        return cmd_meta


GLOBAL_SIGNAL_CORPUS = SignalCorpus()
