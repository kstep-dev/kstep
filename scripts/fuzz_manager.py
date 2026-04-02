"""Fuzzer manager: owns the SignalCorpus and seed pool, spawns workers,
schedules work, and processes coverage results."""

from __future__ import annotations

import json
import logging
import multiprocessing as mp
import os
import queue
import random
import shutil
import signal
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

from run import Driver
from scripts.corpus import SignalCorpus
from scripts.cov import cov_parse
from scripts.fuzz_common import SeedPool, WorkItem, WorkResult, worker_paths
from scripts.fuzz_worker import worker_main
from scripts.fuzz_mutate import pick_pivot
from scripts.input_seq import InputSeq
from scripts.consts import FUZZ_ERROR_DIR, FUZZ_SUCCESS_DIR, FUZZ_CORPUS_DIR

# ──────────────────────────────────────────────────────────────────────────────
# Manager
# ──────────────────────────────────────────────────────────────────────────────

class FuzzManager:
    # Initialize manager-owned queues, counters, corpus state, and worker settings.
    def __init__(
        self,
        n_workers: int,
        driver: Driver,
        linux_name: str,
        steps: int,
        fresh_ratio: float,
        mutate_ratio: float = 0.35,
        pin_cpus: Optional[str] = None,
        demo: bool = False,
    ) -> None:
        self.n_workers = n_workers
        self.driver = driver
        self.linux_name = linux_name
        self.steps = steps
        self.fresh_ratio = fresh_ratio
        self.mutate_ratio = mutate_ratio
        self.pin_cpus = pin_cpus
        self.demo = demo

        self.task_queue: "mp.Queue[Optional[WorkItem]]" = mp.Queue(maxsize=n_workers * 4)
        self.result_queue: "mp.Queue[WorkResult]" = mp.Queue()
        self.shutdown_event = mp.Event()
        self.qemu_cpu_lists: list[Optional[str]] = [None] * n_workers
        self.procs: list[mp.Process] = []

        self.corpus = SignalCorpus()
        self.pool = SeedPool()
        self.rng = random.Random()

        self.t_start = time.monotonic()
        self.last_stat_t = self.t_start
        self.total_execs = 0
        self.total_fresh = 0
        self.total_replay = 0
        self.total_mutate = 0
        self.total_errors = 0
        self.total_new_signals = 0
        self.interval_signals = 0
        self.errors_by_cat: dict[str, int] = {}
        self.target_execs = n_workers if demo else None

    # Save one worker result and its artifacts into the success or error archive.
    def _save_test(self, result: WorkResult) -> Path:
        """Persist a error input and its console log, routed by error category."""
        if result.error:
            assert result.error_category is not None
            category = result.error_category
            category_dir = FUZZ_ERROR_DIR / category
            category_dir.mkdir(parents=True, exist_ok=True)

            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            target_dir = category_dir / f"w{result.worker_id}_{ts}"
        else:
            FUZZ_SUCCESS_DIR.mkdir(parents=True, exist_ok=True)
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            target_dir = FUZZ_SUCCESS_DIR / f"w{result.worker_id}_{ts}"

        target_dir.mkdir()

        ops_data = {
            "linux_name": self.linux_name,
            "seed_id": result.seed_id,
            "ops": result.ops,
        }
        (target_dir / "ops.json").write_text(json.dumps(ops_data, indent=2))

        paths = worker_paths(result.worker_id)

        # The worker may fail before all artifacts are produced, so keep crash
        # preservation best-effort and copy only the files that exist.
        if paths.log_file and paths.log_file.exists():
            shutil.copy2(paths.log_file, target_dir / "worker.log")
        if paths.debug_log_file and paths.debug_log_file.exists():
            shutil.copy2(paths.debug_log_file, target_dir / "worker.debug.log")
        if paths.output_file and paths.output_file.exists():
            shutil.copy2(paths.output_file, target_dir / "worker.jsonl")

        return target_dir

    # Choose the next work item from fresh, mutate, or replay modes.
    def _next_work(self) -> WorkItem:
        r = self.rng.random()
        # Fresh mode
        if len(self.pool) == 0 or r < self.fresh_ratio:
            return WorkItem(mode="fresh", steps=self.steps)
        # Mutate mode
        seed = self.pool.pick_seed()
        assert seed is not None  # pool is non-empty (checked above)
        if r < self.fresh_ratio + self.mutate_ratio:
            pivot_idx = pick_pivot(seed.productive_cmd_ids, self.rng)
            if pivot_idx is not None:
                return WorkItem(
                    mode = "mutate",
                    steps = self.steps,
                    ops = seed.ops,
                    seed_id = seed.seed_id,
                    pivot_idx = pivot_idx,
                )
            # Seeds without productive commands cannot drive tick-insertion
            # mutation, so fall back to a plain replay instead of failing.
            logging.warning("Selected pivot idx is None")
        # replay mode
        return WorkItem(mode="replay", steps=0, ops=seed.ops, seed_id=seed.seed_id)

    # Partition host CPUs between QEMU instances and the Python manager when requested.
    def _configure_cpu_pinning(self) -> None:
        if not self.pin_cpus:
            return

        selected_cpus = _parse_cpu_list(self.pin_cpus)
        qemu_total = self.n_workers * self.driver.num_cpus
        if qemu_total >= len(selected_cpus):
            raise RuntimeError(
                f"pin_cpus: need {qemu_total} CPUs for QEMU plus at least 1 CPU for Python, only {len(selected_cpus)} provided in {self.pin_cpus!r}"
            )

        for wid in range(self.n_workers):
            start = wid * self.driver.num_cpus
            end = start + self.driver.num_cpus
            self.qemu_cpu_lists[wid] = _format_cpu_list(selected_cpus[start:end])

        python_cpus = selected_cpus[qemu_total:]
        os.sched_setaffinity(0, set(python_cpus))
        logging.info(
            f"CPU pinning: selected={selected_cpus} QEMU={self.qemu_cpu_lists} Python={python_cpus}"
        )

    # Turn Ctrl-C into a cooperative manager shutdown signal.
    def _on_sigint(self, _sig, _frame) -> None:
        if self.shutdown_event.is_set():
            return
        logging.info("[fuzz] Caught Ctrl-C – shutting down…")
        self.shutdown_event.set()

    # Start all worker processes and remember their process handles.
    def _spawn_workers(self) -> None:
        for wid in range(self.n_workers):
            proc = mp.Process(
                target=worker_main,
                args=(
                    wid,
                    self.task_queue,
                    self.result_queue,
                    self.driver,
                    self.linux_name,
                    self.qemu_cpu_lists[wid],
                ),
                daemon=True,
            )
            proc.start()
            self.procs.append(proc)
            logging.info(f"Spawned worker {wid} (pid={proc.pid})")

    # Enqueue one work item unless shutdown has already stopped normal scheduling.
    def _put_task(self, item: Optional[WorkItem]) -> bool:
        # `None` is the poison pill used during shutdown, so it must still be
        # enqueueable after `shutdown_event` is set.
        if item is None or not self.shutdown_event.is_set():
            try:
                self.task_queue.put(item, timeout=0.5)
                return True
            except queue.Full:
                raise RuntimeError("Shutdown: task_queue is full")
            except (BrokenPipeError, EOFError, OSError):
                raise RuntimeError("Shutdown: error when pushing task into queue")
        return False

    # Update aggregate execution counters for the finished result's mode.
    def _record_result_mode(self, result: WorkResult) -> None:
        self.total_execs += 1
        if result.mode == "replay":
            self.total_replay += 1
        elif result.mode == "mutate":
            self.total_mutate += 1
        else:
            self.total_fresh += 1

    # Archive and count one failed execution.
    def _handle_error_result(self, result: WorkResult) -> None:
        self.total_errors += 1
        assert result.error_category is not None
        cat = result.error_category
        self.errors_by_cat[cat] = self.errors_by_cat.get(cat, 0) + 1
        error_dir = self._save_test(result)
        logging.error(
            f"[error] worker={result.worker_id}  "
            f"category={cat}  "
            f"msg={result.error!r}  "
            f"saved={error_dir}"
        )

    # Parse coverage from one successful execution and add new seeds to the pool.
    def _handle_success_result(self, result: WorkResult) -> None:
        self._save_test(result)
        paths = worker_paths(result.worker_id)

        # Successful executions can still produce no coverage, for example when
        # QEMU exits early or the cov stream stays empty.
        if not (paths.cov_file.exists() and paths.cov_file.stat().st_size > 0):
            logging.warning("Success test does not have cov file")
            return

        signal_records = cov_parse(paths.cov_file)
        self.corpus.update_pc_symbolize(signal_records, self.linux_name)

        seq = InputSeq()
        for op_tuple in result.ops:
            seq.append(op_tuple)

        seed_id = self.pool.next_seed_id()

        FUZZ_CORPUS_DIR.mkdir(parents=True, exist_ok=True)

        new_signal_info = self.corpus.analyze_new_signals(
            seq=seq,
            signal_records=signal_records,
            linux_name=self.linux_name,
            output_path=FUZZ_CORPUS_DIR / f"{seed_id}.json",
        )
        if not new_signal_info:
            return
        new_signals, distinct_signals = new_signal_info

        cmd_meta = self.corpus.analyze_per_action_signals(
            seq=seq,
            signal_records=signal_records,
            new_signals=set(new_signals),
            linux_name=self.linux_name,
            output_path=FUZZ_CORPUS_DIR / f"{seed_id}_per_action.json",
        )

        productive_cmd_ids = [
            entry["cmd_id"] for entry in cmd_meta if entry["new_edges"]
        ]

        n_new = len(new_signals)
        self.total_new_signals += n_new
        self.interval_signals += n_new
        seed = self.pool.add(result.ops, distinct_signals, productive_cmd_ids)

        logging.info(
            f"[+] seed #{seed.seed_id}: +{n_new} new  "
            f"distinct={distinct_signals}  "
            f"productive_cmd={len(productive_cmd_ids)} "
            f"total={len(self.corpus.seen_signals)}  corpus={len(self.pool)}"
        )

    # Derive an error category from the prefix of an error string.
    def _classify_error(self, exc: str) -> str:
        category, sep, _ = str(exc).partition(":")
        if sep:
            return category.strip().lower()
        return "other"

    # Route one worker result through success or error handling.
    def _process_result(self, result: Optional[WorkResult]) -> None:
        assert result is not None

        self._record_result_mode(result)
        if result.error:
            result.error_category = self._classify_error(result.error)
            self._handle_error_result(result)
        else:
            self._handle_success_result(result)

    # Consume worker results, feed new work, and stop when shutdown is requested.
    def _result_loop(self) -> None:
        while not self.shutdown_event.is_set():
            try:
                result: WorkResult = self.result_queue.get(timeout=0.5)
            except Exception:
                # Timeouts are expected; use them to periodically emit stats
                # while still remaining responsive to shutdown.
                self._maybe_log_periodic_stats()
                continue

            # Consume result
            self._process_result(result)
            if self.target_execs is not None and self.total_execs >= self.target_execs:
                logging.info(
                    f"[fuzz] Demo mode complete: collected {self.total_execs}/{self.target_execs} results"
                )
                self.shutdown_event.set()
                return
            # Produce task
            self._put_task(self._next_work())
            self._maybe_log_periodic_stats()

    # Emit periodic high-level fuzzing stats without blocking the result loop.
    def _maybe_log_periodic_stats(self) -> None:
        now = time.monotonic()
        if now - self.last_stat_t < 10:
            return

        elapsed = now - self.t_start
        cat_str = "  ".join(f"{k}={v}" for k, v in sorted(self.errors_by_cat.items()))
        logging.info(
            f"[{elapsed:6.0f}s]  "
            f"execs={self.total_execs} ({self.total_execs / max(elapsed, 1):.2f}/s)  "
            f"fresh={self.total_fresh}  replay={self.total_replay}  mutate={self.total_mutate}  "
            f"signals={len(self.corpus.seen_signals)} (+{self.interval_signals} this interval)  "
            f"new_total={self.total_new_signals}  "
            f"corpus={len(self.pool)}  "
            f"errors={self.total_errors}"
            + (f"  [{cat_str}]" if cat_str else "")
        )
        self.interval_signals = 0
        self.last_stat_t = now

    # Ask workers to exit and forcibly terminate any process that stays alive.
    def _shutdown_workers(self) -> None:
        logging.info("[fuzz] Sending poison pills to workers…")
        for _ in self.procs:
            self._put_task(None)
        for proc in self.procs:
            proc.join(timeout=10)
            if proc.is_alive():
                logging.warning(f"Worker {proc.pid} didn't exit, terminating…")
                proc.terminate()
                proc.join(timeout=5)

    # Close multiprocessing queues once worker traffic is finished.
    def _close_queues(self) -> None:
        self.task_queue.close()
        self.task_queue.cancel_join_thread()
        self.result_queue.close()
        self.result_queue.cancel_join_thread()

    # Print one final summary after the fuzzing session ends.
    def _log_final_stats(self) -> None:
        elapsed = time.monotonic() - self.t_start
        logging.info(
            f"[fuzz] Done – "
            f"execs={self.total_execs}  "
            f"signals={len(self.corpus.seen_signals)} ({self.total_new_signals} new)  "
            f"corpus={len(self.pool)}  "
            f"wall={elapsed:.0f}s"
        )

    # Run the manager lifecycle from startup through teardown.
    def run(self) -> None:
        # Keep the high-level lifecycle linear: configure once, run the main
        # loop, then always tear workers and queues down in the finally block.
        self._configure_cpu_pinning()
        self._spawn_workers()
        prev_handler = signal.signal(signal.SIGINT, self._on_sigint)
        try:
            for _ in range(self.n_workers):
                put_result = self._put_task(self._next_work())
                if not put_result:
                    break
            self._result_loop()
        finally:
            signal.signal(signal.SIGINT, prev_handler)
            self._shutdown_workers()
            self._close_queues()
            self._log_final_stats()

# Create a manager instance and run it with the provided fuzzing parameters.
def run_manager(
    n_workers: int,
    driver: Driver,
    linux_name: str,
    steps: int,
    fresh_ratio: float,
    mutate_ratio: float = 0.35,
    pin_cpus: Optional[str] = None,
    demo: bool = False,
) -> None:
    manager = FuzzManager(
        n_workers=n_workers,
        driver=driver,
        linux_name=linux_name,
        steps=steps,
        fresh_ratio=fresh_ratio,
        mutate_ratio=mutate_ratio,
        pin_cpus=pin_cpus,
        demo=demo,
    )
    manager.run()


def _parse_cpu_list(cpu_list: str) -> list[int]:
    cpus: list[int] = []
    for part in cpu_list.split(","):
        token = part.strip()
        if not token:
            continue
        if "-" in token:
            start_str, end_str = token.split("-", 1)
            start = int(start_str)
            end = int(end_str)
            if start > end:
                raise RuntimeError(f"pin_cpus: invalid CPU range {token!r}")
            cpus.extend(range(start, end + 1))
        else:
            cpus.append(int(token))

    cpus = sorted(set(cpus))
    if not cpus:
        raise RuntimeError("pin_cpus: CPU set is empty")
    return cpus


def _format_cpu_list(cpus: list[int]) -> str:
    if not cpus:
        raise RuntimeError("pin_cpus: empty CPU allocation")

    ranges: list[str] = []
    start = prev = cpus[0]
    for cpu in cpus[1:]:
        if cpu == prev + 1:
            prev = cpu
            continue
        ranges.append(f"{start}-{prev}" if start != prev else str(start))
        start = prev = cpu
    ranges.append(f"{start}-{prev}" if start != prev else str(start))
    return ",".join(ranges)
