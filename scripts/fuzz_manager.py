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
from scripts.fuzz_common import SeedPool, WorkItem, WorkResult
from scripts.fuzz_worker import worker_main
from scripts.mutate import pick_pivot
from scripts.input_seq import InputSeq


# ──────────────────────────────────────────────────────────────────────────────
# Crash handling
# ──────────────────────────────────────────────────────────────────────────────

def _save_crash(result: WorkResult, fuzz_dir: Path) -> Path:
    """Persist a crashing input and its console log, routed by error category."""
    if result.error:
        category = result.error_category or "other"
    else:
        category = "None"
    category_dir = fuzz_dir / "crashes" / category
    category_dir.mkdir(parents=True, exist_ok=True)

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    crash_dir = category_dir / f"{category}_{ts}_w{result.worker_id}"
    crash_dir.mkdir()

    ops_data = {
        "linux_name": result.linux_name,
        "seed_id": result.seed_id,
        "ops": result.ops,
    }
    (crash_dir / "ops.json").write_text(json.dumps(ops_data, indent=2))

    if result.log_file and result.log_file.exists():
        shutil.copy2(result.log_file, crash_dir / "worker.log")

    if result.debug_log_file and result.debug_log_file.exists():
        shutil.copy2(result.debug_log_file, crash_dir / "worker.debug.log")

    if result.output_file and result.output_file.exists():
        shutil.copy2(result.output_file, crash_dir / "worker.jsonl")
    
    return crash_dir

# ──────────────────────────────────────────────────────────────────────────────
# Work scheduling
# ──────────────────────────────────────────────────────────────────────────────

def _next_work(
    linux_name: str,
    steps: int,
    pool: "SeedPool",
    fresh_ratio: float,
    mutate_ratio: float,
    rng: random.Random,
) -> WorkItem:
    r = rng.random()
    if len(pool) == 0 or r < fresh_ratio:
        return WorkItem(mode="fresh", steps=steps, linux_name=linux_name)
    seed = pool.pick_seed()
    assert seed is not None  # pool is non-empty (checked above)
    if r < fresh_ratio + mutate_ratio:
        pivot_idx = pick_pivot(seed.productive_cmd_ids, rng)
        if pivot_idx is not None:
            return WorkItem(
                mode="mutate",
                steps=steps,
                linux_name=linux_name,
                ops=seed.ops,
                seed_id=seed.seed_id,
                pivot_idx=pivot_idx,
            )
        # Seed has no productive commands yet; fall through to replay
    return WorkItem(mode="replay", steps=0, linux_name=linux_name,
                    ops=seed.ops, seed_id=seed.seed_id)


# ──────────────────────────────────────────────────────────────────────────────
# Manager
# ──────────────────────────────────────────────────────────────────────────────

def run_manager(
    n_workers: int,
    driver: Driver,
    linux_name: str,
    steps: int,
    fuzz_dir: Path,
    fresh_ratio: float,
    mutate_ratio: float = 0.35,
    pin_cpus: bool = False,
    demo: bool = False,
) -> None:
    task_queue: "mp.Queue[Optional[WorkItem]]" = mp.Queue(maxsize=n_workers * 2)
    result_queue: "mp.Queue[WorkResult]" = mp.Queue()

    corpus = SignalCorpus()
    pool   = SeedPool()
    rng    = random.Random()

    t_start           = time.monotonic()
    total_execs       = 0
    total_fresh       = 0
    total_replay      = 0
    total_mutate      = 0
    total_errors      = 0
    errors_by_cat: dict[str, int] = {}
    total_new_signals = 0
    interval_signals  = 0
    last_stat_t       = t_start

    # CPU pinning: assign each QEMU worker_i to CPUs [i*num_cpus, (i+1)*num_cpus-1]
    # and restrict Python processes to the remaining CPUs.
    qemu_cpu_lists: list[Optional[str]] = [None] * n_workers
    if pin_cpus:
        host_cpus = os.cpu_count() or 1
        qemu_total = n_workers * driver.num_cpus
        if qemu_total < host_cpus:
            for wid in range(n_workers):
                start = wid * driver.num_cpus
                end = start + driver.num_cpus - 1
                qemu_cpu_lists[wid] = f"{start}-{end}"
            python_cpus = set(range(qemu_total, host_cpus))
            os.sched_setaffinity(0, python_cpus)  # workers inherit via fork
            logging.info(
                f"CPU pinning: QEMU {qemu_cpu_lists}, Python {sorted(python_cpus)}"
            )
        else:
            raise RuntimeError(f"pin_cpus: need {qemu_total} QEMU + 1 Python CPUs, only {host_cpus} available")

    # Spawn worker processes
    procs: list[mp.Process] = []
    for wid in range(n_workers):
        p = mp.Process(
            target=worker_main,
            args=(wid, task_queue, result_queue, driver, fuzz_dir, qemu_cpu_lists[wid]),
            daemon=True,
        )
        p.start()
        procs.append(p)
        logging.info(f"Spawned worker {wid} (pid={p.pid})")

    shutdown_event = mp.Event()

    def _on_sigint(_sig, _frame):
        if not shutdown_event.is_set():
            logging.info("[fuzz] Caught Ctrl-C – shutting down…")
            shutdown_event.set()

    def _put_task_interruptibly(item: Optional[WorkItem], *, force: bool = False) -> bool:
        while force or not shutdown_event.is_set():
            try:
                task_queue.put(item, timeout=0.5)
                return True
            except queue.Full:
                if force:
                    return False
            except (BrokenPipeError, EOFError, OSError):
                shutdown_event.set()
                return False
        return False

    prev_handler = signal.signal(signal.SIGINT, _on_sigint)

    # Pre-fill the queue so no worker idles at startup.
    for _ in range(n_workers):
        if not _put_task_interruptibly(_next_work(linux_name, steps, pool, fresh_ratio, mutate_ratio, rng)):
            break

    target_execs = n_workers if demo else -1

    while not shutdown_event.is_set():
        try:
            result: WorkResult = result_queue.get(timeout=0.5)
        except Exception:
            continue

        if result is not None:
            total_execs += 1
            if result.mode == "replay":
                total_replay += 1
            elif result.mode == "mutate":
                total_mutate += 1
            else:
                total_fresh += 1

            if result.error:
                total_errors += 1
                cat = result.error_category or "other"
                errors_by_cat[cat] = errors_by_cat.get(cat, 0) + 1
                crash_dir = _save_crash(result, fuzz_dir)
                logging.error(
                    f"[error] worker={result.worker_id}  "
                    f"crash={cat == 'crash'}  "
                    f"timedout={cat == 'timedout'}  "
                    f"retry_tick={cat == 'retry_tick'}  "
                    f"category={cat}  "
                    f"msg={result.error!r}  "
                    f"saved={crash_dir}"
                )
            else:
                _save_crash(result, fuzz_dir)

                if result.cov_file:
                    signal_records = cov_parse(result.cov_file)
                    corpus.update_pc_symbolize(signal_records, result.linux_name)

                    seq = InputSeq()
                    for op_tuple in result.ops:
                        seq.append(op_tuple)

                    new_signals = corpus.analyze_new_signals(
                        seq=seq,
                        signal_records=signal_records,
                        linux_name=result.linux_name,
                    )
                    if new_signals:
                        cmd_meta = corpus.analyze_per_action_signals(
                            seq=seq,
                            signal_records=signal_records,
                            new_signals=set(new_signals),
                            linux_name=result.linux_name,
                        )
                        productive_cmd_ids = [
                            entry["cmd_id"] for entry in cmd_meta if entry["new_edges"]
                        ]
                        n_new = len(new_signals)
                        total_new_signals += n_new
                        interval_signals  += n_new
                        seed = pool.add(result.ops, n_new, productive_cmd_ids)
                        logging.info(
                            f"[+] seed #{seed.seed_id}: +{n_new} new  "
                            f"total={len(corpus.seen_signals)}  corpus={len(pool)}"
                        )


            if target_execs != -1 and total_execs >= target_execs:
                logging.info(
                    f"[fuzz] Demo mode complete: collected {total_execs}/{target_execs} results"
                )
                shutdown_event.set()
            else:
                # Immediately replace the consumed slot
                _put_task_interruptibly(
                    _next_work(linux_name, steps, pool, fresh_ratio, mutate_ratio, rng)
                )

        # Periodic stats (every 10 s)
        now = time.monotonic()
        if now - last_stat_t >= 10:
            elapsed = now - t_start
            cat_str = "  ".join(f"{k}={v}" for k, v in sorted(errors_by_cat.items()))
            logging.info(
                f"[{elapsed:6.0f}s]  "
                f"execs={total_execs} ({total_execs / max(elapsed, 1):.2f}/s)  "
                f"fresh={total_fresh}  replay={total_replay}  mutate={total_mutate}  "
                f"signals={len(corpus.seen_signals)} (+{interval_signals} this interval)  "
                f"new_total={total_new_signals}  "
                f"corpus={len(pool)}  "
                f"errors={total_errors}"
                + (f"  [{cat_str}]" if cat_str else "")
            )
            interval_signals = 0
            last_stat_t = now

    # Graceful shutdown
    signal.signal(signal.SIGINT, prev_handler)  # Restore original handler
    logging.info("[fuzz] Sending poison pills to workers…")
    for _ in procs:
        _put_task_interruptibly(None, force=True)
    for p in procs:
        p.join(timeout=10)
        if p.is_alive():
            logging.warning(f"Worker {p.pid} didn't exit, terminating…")
            p.terminate()
            p.join(timeout=5)

    task_queue.close()
    task_queue.cancel_join_thread()
    result_queue.close()
    result_queue.cancel_join_thread()

    elapsed = time.monotonic() - t_start
    logging.info(
        f"[fuzz] Done – "
        f"execs={total_execs}  "
        f"signals={len(corpus.seen_signals)} ({total_new_signals} new)  "
        f"corpus={len(pool)}  "
        f"wall={elapsed:.0f}s"
    )
