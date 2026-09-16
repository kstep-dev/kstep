# The in-guest fuzzer (archived)

kSTEP's first fuzzer. It ran **inside the guest**: `kmod/fuzz/` was an executor loaded with the
kmod, which decoded an input sequence into scheduler operations, ran them, checked invariants and
dumped an edge map over a virtio console port; `scripts/` was the host side that generated and
mutated inputs, collected coverage and managed workers, driven by `fuzz.py`.

It was superseded by [`fuzzer/`](../../../fuzzer), a LibAFL client of the `cli` driver
(`./fuzz.sh <bug>`). The guest now carries no fuzzer-specific code at all: programs are cli lines
over the jsonl socket, invariants are the rules in [`kmod/checkers/`](../../../kmod/checkers)
(enabled by the `check` verb), and coverage is read straight out of the guest's RAM file from the
shared region in [`kmod/shm.h`](../../../kmod/shm.h).

Kept for reference only -- nothing here is built, imported or run, and the modules it imports
(`scripts.utils` aside) no longer exist at their original paths.

| here | was |
| --- | --- |
| `kmod/` | `kmod/fuzz/` -- in-guest executor, op handlers, coverage, sanity checks |
| `scripts/` | `scripts/` -- input generation and mutation, coverage parsing, worker/corpus management |
| `fuzz.py` | `fuzz.py` -- the entry point |
| `run_test.py` | `docs/archive/run_test.py` -- single-input replay against the executor |
