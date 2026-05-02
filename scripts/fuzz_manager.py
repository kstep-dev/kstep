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
from scripts.fuzz_common import (
    MutationPivot,
    Seed,
    SeedPool,
    WorkItem,
    WorkResult,
    worker_dir,
)
from scripts.fuzz_mutate import bottleneck_signal
from scripts.fuzz_worker import worker_main
from scripts.input_seq import InputSeq
from scripts.utils import FUZZ_CORPUS_DIR, FUZZ_ERROR_DIR, FUZZ_SUCCESS_DIR, fuzz_mode_dir

# ──────────────────────────────────────────────────────────────────────────────
# Manager
# ──────────────────────────────────────────────────────────────────────────────

class FuzzManager:
    # Initialize manager-owned queues, counters, corpus state, and worker settings.
    def __init__(
        self,
        n_workers: int,
        driver: Driver,
        name: str,
        steps: int,
        fresh_ratio: float,
        mutate_ratio: float = 0.35,
        special_mutate_ratio: float = 0.2,
        pivot_rarity_alpha: float = 1.0,
        cross_scheduler: bool = False,
        enable_kthreads: bool = False,
        enable_task_freeze: bool = True,
        pin_cpus: Optional[str] = None,
        ci_mode: bool = False,
    ) -> None:
        self.n_workers = n_workers
        self.driver = driver
        self.name = name
        self.steps = steps
        self.fresh_ratio = fresh_ratio
        self.mutate_ratio = mutate_ratio
        self.special_mutate_ratio = special_mutate_ratio
        self.pivot_rarity_alpha = pivot_rarity_alpha
        self.cross_scheduler = cross_scheduler
        self.enable_kthreads = enable_kthreads
        self.enable_task_freeze = enable_task_freeze
        self.pin_cpus = pin_cpus
        self.ci_mode = ci_mode

        self.task_queue: "mp.Queue[Optional[WorkItem]]" = mp.Queue(maxsize=n_workers * 4)
        self.result_queue: "mp.Queue[WorkResult]" = mp.Queue()
        self.shutdown_event = mp.Event()
        self.qemu_cpu_lists: list[Optional[str]] = [None] * n_workers
        self.procs: list[mp.Process] = []

        self.corpus = SignalCorpus()
        self.pool = SeedPool()
        self.special_pool = SeedPool()
        self.rng = random.Random()
        self.seed_counter = 0

        self.t_start = time.monotonic()
        self.last_stat_t = self.t_start
        self.total_execs = 0
        self.total_fresh = 0
        self.total_replay = 0
        self.total_mutate = 0
        self.total_errors = 0
        self.total_new_signals = 0
        self.total_special_seeds = 0
        self.seen_modes: set[str] = set()
        self.interval_signals = 0
        self.errors_by_cat: dict[str, int] = {}
        self.ci_plan = ["fresh", "replay", "mutate"] if ci_mode else []
        self.target_execs = len(self.ci_plan) if ci_mode else None

        if self.ci_mode and self.n_workers != 1:
            raise RuntimeError("ci_mode requires exactly one worker")

    def _allocate_seed_id(self) -> int:
        seed_id = self.seed_counter
        self.seed_counter += 1
        return seed_id

    # Save one worker result and its artifacts into the configured archive layout.
    def _save_test(self, result: WorkResult) -> Path:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        if self.ci_mode:
            mode_dir = fuzz_mode_dir(result.mode)
            mode_dir.mkdir(parents=True, exist_ok=True)
            target_dir = mode_dir
        elif result.error:
            assert result.error_category is not None
            category_dir = FUZZ_ERROR_DIR / result.error_category
            category_dir.mkdir(parents=True, exist_ok=True)
            target_dir = category_dir / f"w{result.worker_id}_{ts}"
        else:
            FUZZ_SUCCESS_DIR.mkdir(parents=True, exist_ok=True)
            target_dir = FUZZ_SUCCESS_DIR / f"w{result.worker_id}_{ts}"

        target_dir.mkdir(exist_ok=True)

        ops_data = {
            "name": self.name,
            "mode": result.mode,
            "seed_id": result.seed_id,
            "ops": result.ops,
            "error": result.error,
            "error_category": result.error_category,
        }
        (target_dir / "ops.json").write_text(json.dumps(ops_data, indent=2))

        wdir = worker_dir(result.worker_id)

        # The worker may fail before all artifacts are produced, so keep crash
        # preservation best-effort and copy only the files that exist.
        if wdir.log.exists():
            shutil.copy2(wdir.log, target_dir / "worker.log")
        if wdir.debug_log.exists():
            shutil.copy2(wdir.debug_log, target_dir / "worker.debug.log")
        if wdir.output.exists():
            shutil.copy2(wdir.output, target_dir / "worker.jsonl")

        return target_dir

    def _pick_replay_seed(self) -> Optional[Seed]:
        seed = self.pool.pick_seed(self.rng)
        if seed is None:
            seed = self.special_pool.pick_seed(self.rng)
        return seed

    def _pick_mutation_target(self) -> Optional[tuple[str, Seed, MutationPivot]]:
        prefer_special = (
            len(self.special_pool) > 0
            and self.rng.random() < self.special_mutate_ratio
        )
        pool_order = [
            ("special", self.special_pool),
            ("coverage", self.pool),
        ] if prefer_special else [
            ("coverage", self.pool),
            ("special", self.special_pool),
        ]

        for pool_name, seed_pool in pool_order:
            picked = seed_pool.pick_mutation_target(
                signal_test_counts=self.corpus.signal_test_counts,
                rng=self.rng,
                rarity_alpha=self.pivot_rarity_alpha,
            )
            if picked is not None:
                seed, pivot = picked
                return pool_name, seed, pivot
        return None

    def _make_mutation_work(
        self,
        pool_name: str,
        seed: Seed,
        pivot: MutationPivot,
    ) -> WorkItem:
        bottleneck = bottleneck_signal(
            pivot.signal_ids,
            self.corpus.signal_test_counts,
            self.pivot_rarity_alpha,
        )
        bottleneck_freq = 0.0
        if bottleneck is not None:
            bottleneck_freq = (
                self.corpus.signal_test_counts.get(bottleneck, 0)
                / max(self.corpus.total_covered_tests, 1)
            )
        logging.info(
            f"[mutate] pool={pool_name} picked seed=#{seed.seed_id} pivot={pivot.cmd_id} "
            f"kind={pivot.kind} "
            f"bottleneck_signal={bottleneck} "
            f"bottleneck_signal_freq={bottleneck_freq:.6f} "
            f"score={pivot.weight(self.corpus.signal_test_counts, self.pivot_rarity_alpha):.4f}"
        )
        return WorkItem(
            mode="mutate",
            steps=self.steps,
            ops=seed.ops,
            seed_id=seed.seed_id,
            pivot_idx=pivot.cmd_id,
        )

    def _next_ci_work(self) -> WorkItem:
        mode_idx = self.total_execs
        if mode_idx >= len(self.ci_plan):
            raise RuntimeError("ci_mode scheduled more work than expected")

        planned_mode = self.ci_plan[mode_idx]
        if planned_mode == "fresh":
            return WorkItem(mode="fresh", steps=self.steps)

        if planned_mode == "replay":
            seed = self._pick_replay_seed()
            if seed is None:
                raise RuntimeError("ci_mode: no seed available for replay")
            return WorkItem(mode="replay", steps=0, ops=seed.ops, seed_id=seed.seed_id)

        target = self._pick_mutation_target()
        if target is not None:
            return self._make_mutation_work(*target)
        
        raise RuntimeError("ci_mode: no pivot available for mutation")


    # Choose the next work item from fresh, mutate, or replay modes.
    def _next_work(self) -> WorkItem:
        if self.ci_mode:
            return self._next_ci_work()

        r = self.rng.random()
        if (len(self.pool) == 0 and len(self.special_pool) == 0) or r < self.fresh_ratio:
            return WorkItem(mode="fresh", steps=self.steps)

        if r < self.fresh_ratio + self.mutate_ratio:
            target = self._pick_mutation_target()
            if target is not None:
                return self._make_mutation_work(*target)
            logging.warning("No productive pivots available for mutate; falling back to replay")

        seed = self._pick_replay_seed()
        assert seed is not None  # pool is non-empty (checked above)
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
                    self.name,
                    self.cross_scheduler,
                    self.enable_kthreads,
                    self.enable_task_freeze,
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
        self.seen_modes.add(result.mode)
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
        wdir = worker_dir(result.worker_id)

        # Successful executions can still produce no coverage, for example when
        # QEMU exits early or the cov stream stays empty.
        if not (wdir.cov.exists() and wdir.cov.stat().st_size > 0):
            logging.warning("Success test does not have cov file")
            return

        signal_records = cov_parse(wdir.cov)
        self.corpus.update_pc_symbolize(signal_records, self.name)

        seq = InputSeq()
        for op_tuple in result.ops:
            seq.append(op_tuple)

        seed_id = self._allocate_seed_id()

        FUZZ_CORPUS_DIR.mkdir(parents=True, exist_ok=True)

        new_signal_info = self.corpus.analyze_new_signals(
            seq=seq,
            signal_records=signal_records,
            name=self.name,
            output_path=FUZZ_CORPUS_DIR / f"{seed_id}.json",
        )
        if new_signal_info:
            new_signals, distinct_signals = new_signal_info

            cmd_meta = self.corpus.analyze_per_action_signals(
                seq=seq,
                signal_records=signal_records,
                new_signals=set(new_signals),
                name=self.name,
                output_path=FUZZ_CORPUS_DIR / f"{seed_id}_per_action.json",
            )

            productive_pivots = [
                MutationPivot(
                    cmd_id=entry["cmd_id"],
                    signal_ids=list(entry["new_signal_ids"]),
                )
                for entry in cmd_meta
                if entry["new_signal_ids"]
            ]

            n_new = len(new_signals)
            self.total_new_signals += n_new
            self.interval_signals += n_new
            seed = self.pool.add(
                result.ops,
                n_new,
                productive_pivots,
                seed_id=seed_id,
            )

            logging.info(
                f"[+] seed #{seed.seed_id}: +{n_new} new  "
                f"distinct={distinct_signals}  "
                f"productive_cmd={len(productive_pivots)} "
                f"covered={self.corpus.total_covered_tests}  "
                f"total={len(self.corpus.seen_signals)}  corpus={len(self.pool)}"
            )

        special_pivot_idxs = sorted({
            idx for idx in result.special_pivot_idxs
            if 0 <= idx < len(result.ops)
        })
        if not special_pivot_idxs:
            return

        special_pivots = [
            MutationPivot(
                cmd_id=idx,
                signal_ids=[],
                kind="special_state",
                special_score=1.0,
            )
            for idx in special_pivot_idxs
        ]
        seed = self.special_pool.add(
            result.ops,
            len(special_pivots),
            special_pivots,
            seed_id=seed_id,
        )
        self.total_special_seeds += 1
        logging.info(
            f"[+] special seed #{seed.seed_id}: pivots={len(special_pivots)}  "
            f"special_corpus={len(self.special_pool)}"
        )

    # Derive an error category from the prefix of an error string.
    def _classify_error(self, exc: str) -> str:
        category, sep, _ = str(exc).partition(":")
        if sep:
            return category.strip().lower()
        return "other"

    # Log per-result execution metadata.
    def _log_result(self, result: WorkResult) -> None:
        logging.info(
            f"[result] worker={result.worker_id}  "
            f"mode={result.mode}  "
            f"ops={len(result.ops)}  "
            f"exec_time={result.exec_time:.2f}s  "
            f"special_pivots={len(result.special_pivot_idxs)}"
        )

    # Route one worker result through success or error handling.
    def _process_result(self, result: Optional[WorkResult]) -> None:
        assert result is not None

        self._record_result_mode(result)
        self._log_result(result)
        if result.error:
            result.error_category = self._classify_error(result.error)
            self._handle_error_result(result)
            return

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
            if self.ci_mode and self.total_errors > 0:
                logging.info("[fuzz] CI mode stopping after first error result")
                self.shutdown_event.set()
                return
            if self.target_execs is not None and self.total_execs >= self.target_execs:
                logging.info(
                    f"[fuzz] Target run complete: collected {self.total_execs}/{self.target_execs} results"
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
            f"special_corpus={len(self.special_pool)}  "
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
            f"special_corpus={len(self.special_pool)} ({self.total_special_seeds} new)  "
            f"wall={elapsed:.0f}s"
        )

    # Enforce CLI-configured expectations after cleanup has finished.
    def _validate_outcome(self) -> None:
        if self.total_errors > 0:
            raise RuntimeError(f"Fuzzing reported {self.total_errors} error result(s)")

        required_modes = set(self.ci_plan)
        missing_modes = sorted(required_modes - self.seen_modes)
        if missing_modes:
            seen_modes = ", ".join(sorted(self.seen_modes)) or "none"
            missing = ", ".join(missing_modes)
            raise RuntimeError(
                f"Required fuzz mode(s) not observed: {missing}; seen={seen_modes}"
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
            if self.ci_mode:
                self._validate_outcome()

# Create a manager instance and run it with the provided fuzzing parameters.
def run_manager(
    n_workers: int,
    driver: Driver,
    name: str,
    steps: int,
    fresh_ratio: float,
    mutate_ratio: float = 0.35,
    special_mutate_ratio: float = 0.2,
    pivot_rarity_alpha: float = 1.0,
    cross_scheduler: bool = False,
    enable_kthreads: bool = False,
    enable_task_freeze: bool = True,
    pin_cpus: Optional[str] = None,
    ci_mode: bool = False,
) -> None:
    manager = FuzzManager(
        n_workers=n_workers,
        driver=driver,
        name=name,
        steps=steps,
        fresh_ratio=fresh_ratio,
        mutate_ratio=mutate_ratio,
        special_mutate_ratio=special_mutate_ratio,
        pivot_rarity_alpha=pivot_rarity_alpha,
        cross_scheduler=cross_scheduler,
        enable_kthreads=enable_kthreads,
        enable_task_freeze=enable_task_freeze,
        pin_cpus=pin_cpus,
        ci_mode=ci_mode,
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
