import json
from .consts import CORPUS_DIR
from .gen_input_ops import OP_NAME_TO_TYPE, OP_TYPE_TO_NAME
from .input_seq import InputSeq
from pathlib import Path

from .cov import symbolize_pcs

class SignalCorpus:
    def __init__(self):
        self.seen_signals: set[int] = set()
        self.seen_pcs: dict[int, tuple[str, str]] = {}
        CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    def update_pc_symbolize(self, 
        signal_records: list[tuple[int, int, int, int]],
        linux_name: str,
    ):
        new_pcs = {rec[2] for rec in signal_records if rec[2] not in self.seen_pcs}
        pc_symbolize = symbolize_pcs(list(new_pcs), linux_name)
        for pc, (fn, loc) in pc_symbolize.items():
            self.seen_pcs[pc] = (fn, loc)

    # Analyze the new signals for a test, return the new signals if any
    def analyze_new_signals(
        self,
        seq: InputSeq,
        signal_records: list[tuple[int, int, int, int]],
        linux_version: str,
    ) -> set[int] | None:
        signal_list = [rec[3] for rec in signal_records]
        # Check if the records have been seen before or not
        new = set(signal_list) - self.seen_signals
        if not new:
            print("signal: no new signals found")
            return None
        self.seen_signals.update(new)

        # Dump the test into the corpus directory if it hits new signals
        save_path = CORPUS_DIR / f"{seq.digest()}.json"
        data = {
            "seq": seq.to_list(), # input sequence
            "new_signals": sorted(new), # new signals in the signal file
            "linux_version": linux_version,
        }
        print(f"Signal: found {len(new)} new signals, dumping to {save_path}")
        with open(save_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        return new

    # Analyze the per-action (each command each pid's action is an action) signals for a test
    # Save the new signal hit by each task
    # If a task hits a new signal, the following tasks hit the same signal will not be recorded
    def analyze_per_action_signals(
        self,
        seq: InputSeq,
        signal_records: list[tuple[int, int, int, int]], # each entry is a signal record, cmd_id -> pid -> sig -> pc
        new_signals: set[int],
        linux_name: str,
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
                    "new_signals": {}, # pid -> [(sig, (fn, loc))]
                })
                cmd_id += 1
                
        for rec in signal_records:
            cmd_id = rec[0]
            pid = rec[1]
            pc = rec[2]
            sig = rec[3]
            if sig in new_signals:
                # Add the signal to the new signals for the action if the task hits that new signal
                # Remove the signal from the new signals set if it has been recorded
                # So that the following tasks hit the same signal will not be recorded
                new_signals_by_action = cmd_meta[cmd_id]["new_signals"]
                if pid not in new_signals_by_action:
                    new_signals_by_action[pid] = []
                new_signals_by_action[pid].append((sig, self.seen_pcs[pc]))
                new_signals.remove(sig)

        # Dump the per-action new signals to the corpus directory
        save_path = CORPUS_DIR / f"{seq.digest()}_per_action.json"  
        print(f"Signal: dumping per-action new signals to {save_path}")
        with open(save_path, "w", encoding="utf-8") as f:
            json.dump({"Linux_name": linux_name, "cmd_meta": cmd_meta}, f, indent=2)

        return cmd_meta


GLOBAL_SIGNAL_CORPUS = SignalCorpus()
