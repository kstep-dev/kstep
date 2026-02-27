import json
from .consts import CORPUS_DIR
from .gen_input_ops import OP_NAME_TO_TYPE, OP_TYPE_TO_NAME
from .input_seq import InputSeq

class SignalCorpus:
    def __init__(self):
        self.seen: set[int] = set()
        CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    # Analyze the new signals for a test, return the new signals if any
    def analyze_new_signals(
        self,
        seq: InputSeq,
        signal_records: set[int],
        linux_version: str,
    ) -> set[int] | None:
        # Check if the records have been seen before or not
        new = set(signal_records) - self.seen
        if not new:
            print("signal: no new signals found")
            return None
        self.seen.update(new)

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
        signal_records: list[dict[str, int]], # each entry is a signal record, cmd_id -> pid -> sig
        new_signals: set[int],
    ) -> dict[int, dict]:
        tick_repeat = OP_NAME_TO_TYPE["TICK_REPEAT"]
        cmd_meta: dict[int, dict] = {}
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
                cmd_id += 1
                cmd_meta[cmd_id] = {
                    "cmd_id": cmd_id,
                    "cmd_name": cmd_name,
                    "cmd_args": cmd_args,
                    "new_signals": {}, # pid -> set of new signals
                }
                
        for rec in signal_records:
            if rec["cmd_id"] not in cmd_meta:
                raise ValueError(f"command {rec["cmd_id"]} not found in cmd_meta")
            if rec["sig"] in new_signals:
                # Add the signal to the new signals for the action if the task hits that new signal
                # Remove the signal from the new signals set if it has been recorded
                # So that the following tasks hit the same signal will not be recorded
                new_signals_by_action = cmd_meta[rec["cmd_id"]]["new_signals"]
                if rec["pid"] not in new_signals_by_action:
                    new_signals_by_action[rec["pid"]] = []
                new_signals_by_action[rec["pid"]].append(rec["sig"])
                new_signals.remove(rec["sig"])

        # Dump the per-action new signals to the corpus directory
        save_path = CORPUS_DIR / f"{seq.digest()}_per_action.json"  
        print(f"Signal: dumping per-action new signals to {save_path}")
        with open(save_path, "w", encoding="utf-8") as f:
            json.dump(cmd_meta, f, indent=2)

        return cmd_meta


GLOBAL_SIGNAL_CORPUS = SignalCorpus()
