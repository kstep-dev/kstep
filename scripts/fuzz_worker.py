"""Fuzzer worker: runs one QEMU instance per loop iteration and reports
raw coverage + the executed op sequence back to the manager."""

from __future__ import annotations

import logging
import multiprocessing as mp
import os
import random
import signal
import subprocess
import time
from pathlib import Path
from typing import Optional

from run import Driver, start_qemu
from scripts.fuzz_common import Ops, WorkItem, WorkResult, connect_to_kmod, read_kmod_state
from scripts.gen_input_core import generate_next_command, init_genstate, _op_matches_task_state, replay_update_genstate
from scripts.gen_input_ops import OP_NAME_TO_TYPE, OP_TYPE_TO_NAME

_CRASH_MARKERS = [
    b"Kernel panic",
    b"kernel BUG at",
    b"BUG: ",
    b"KASAN: ",
    b"UBSAN: ",
    b"general protection fault",
    b"Oops: ",
]


def _send_op(logger: logging.Logger, sock, sf, gen, op: int, a: int, b: int, c: int) -> bool:
    """Send one op to kmod, read back task state, and update gen. Raises on failure."""
    sock.sendall(f"{op},{a},{b},{c}\n".encode())
    logger.debug(f"SEND OP: {op},{a},{b},{c}")
    state = read_kmod_state(sf)
    if state is None:
        sock.sendall(b"EXIT\n")
        raise RuntimeError("Failed to read task states")
    logger.debug(f"Received task states")
    gen.update_from_kmod(state.task_states)
    return state.executed


def _check_for_crash(log_file: Path) -> bool:
    """Return True if the worker console log contains kernel crash markers."""
    try:
        content = log_file.read_bytes()
        return any(marker in content for marker in _CRASH_MARKERS)
    except Exception:
        return False


def worker_main(
    worker_id: int,
    task_queue: "mp.Queue[Optional[WorkItem]]",
    result_queue: "mp.Queue[WorkResult]",
    driver: Driver,
    fuzz_dir: Path,
    qemu_cpus: Optional[str] = None,
) -> None:
    """Worker loop: run QEMU instances and report results to the manager."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)   # manager owns shutdown

    rng = random.Random(os.getpid() ^ (worker_id + 1) * 0x9E3779B9)
    log_file  = fuzz_dir / f"worker_{worker_id}.log"
    sock_file = fuzz_dir / f"worker_{worker_id}.sock"
    worker_log = fuzz_dir / f"worker_{worker_id}.debug.log"

    # Set up worker-specific file logger
    logger = logging.getLogger(f"worker_{worker_id}")
    logger.setLevel(logging.DEBUG)
    logger.propagate = False  # Don't propagate to root logger (stdout)
    handler = logging.FileHandler(worker_log, mode="w")
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
    logger.addHandler(handler)

    while True:
        work: Optional[WorkItem] = task_queue.get()
        if work is None:        # poison pill
            break

        sock_file.unlink(missing_ok=True)
        t0 = time.monotonic()
        ops_executed: Ops = []
        result_cov: Optional[Path] = None
        error: Optional[str] = None
        error_category: Optional[str] = None
        proc = None

        try:
            handler.stream.seek(0)
            handler.stream.truncate()

            proc = start_qemu(
                driver=driver,
                linux_name=work.linux_name,
                log_file=log_file,
                sock_file=sock_file,
                quiet=True,
                cpu_affinity=qemu_cpus,
            )

            # QEMU uses wait=on and blocks until we connect; retry until ready.
            try:
                sock = connect_to_kmod(sock_file, timeout=10.0, retries=200)
            except RuntimeError as e:
                raise RuntimeError("Failed to connect to kmod socket") from e

            sock.settimeout(60.0)
            sf = sock.makefile("rb")
            state = read_kmod_state(sf)  # kmod signals readiness; result unused, gen not yet initialized
            if state is None:
                raise RuntimeError("kmod socket closed before ready signal")

            gen_seed = 0 if work.mode in ("replay", "mutate") else rng.randint(0, 2**32 - 1)
            gen = init_genstate(max_tasks=10, max_cgroups=10, cpus=driver.num_cpus, seed=gen_seed)


            _TICK = OP_NAME_TO_TYPE["TICK"]

            if work.mode in ("replay", "mutate"):
                assert work.ops is not None
                # For mutate: replay only up to and including the pivot command.
                # For replay: replay the full sequence.
                if work.mode == "mutate" and work.pivot_idx is not None:
                    prefix_len = min(work.pivot_idx + 1, len(work.ops))
                else:
                    prefix_len = len(work.ops)

                for i, (op, a, b, c) in enumerate(work.ops[:prefix_len]):
                    for t in range(50):
                        if _op_matches_task_state(gen, (op, a, b, c)):
                            executed = _send_op(logger, sock, sf, gen, op, a, b, c)
                            
                            if executed:
                                logger.debug(
                                    f"REPLAY {i}: op={op},{a},{b},{c} executed={executed} task_state={gen.task_state}"
                                )
                                ops_executed.append((op, a, b, c))
                                replay_update_genstate(gen, op, a, b, c)
                                break
                        
                        logger.debug(f"REPLAY {i} retry {t}: task_state={gen.task_state}")
                        continue
                            
                    else:
                        op_name = OP_TYPE_TO_NAME.get(op, str(op))
                        actual = gen.task_state.get(a, "not_found")
                        sock.sendall(b"EXIT\n")
                        raise RuntimeError(
                            f"replay mismatch at step {i} after 50 retries: "
                            f"{op_name}(task={a}) task_state={actual!r}"
                        )

                # After replaying the prefix, interactively generate new commands.
                if work.mode == "mutate" and work.pivot_idx is not None:
                    i = 0
                    while i < work.steps:
                        op, a, b, c = generate_next_command(gen)

                        executed = _send_op(logger, sock, sf, gen, op, a, b, c)
                        
                        if executed:
                            logger.debug(
                                f"MUTATE-GEN {i}: op={op},{a},{b},{c} executed={executed} task_state={gen.task_state}"
                            )
                            ops_executed.append((op, a, b, c))
                            i += 1
            else:
                i = 0
                while i < work.steps:
                    op, a, b, c = generate_next_command(gen)

                    executed = _send_op(logger, sock, sf, gen, op, a, b, c)
                    if executed:
                        logger.debug(f"STEP {i}: executed={executed} task_state={gen.task_state}")
                        ops_executed.append((op, a, b, c))
                        i += 1


            sock.sendall(b"EXIT\n")
            sock.close()
            
            proc.wait(timeout=120)

            cov_path = log_file.with_suffix(".cov")
            if cov_path.exists() and cov_path.stat().st_size > 0:
                result_cov = cov_path

        except Exception as exc:
            if proc is not None:
                try:
                    proc.kill()
                    proc.wait()
                except Exception:
                    pass
            if isinstance(exc, subprocess.TimeoutExpired):
                error_category = "timedout"
            elif _check_for_crash(log_file):
                error_category = "crash"
            elif "replay mismatch" in str(exc):
                error_category = "retry_tick"
            else:
                error_category = "other"
            error = str(exc)
            logger.error(f"Worker {worker_id}: {exc}")

        if error is None:
            if work.mode == "replay":
                assert work.ops is not None
                expected_ops = len(work.ops)
            elif work.mode == "mutate":
                assert work.ops is not None
                expected_ops = min(work.pivot_idx + 1, len(work.ops)) if work.pivot_idx is not None else len(work.ops)
            else:
                expected_ops = work.steps

            if len(ops_executed) < expected_ops:
                error = f"op count mismatch: expected {expected_ops}, got {len(ops_executed)}"
                error_category = "op_mismatch"
                logger.error(f"Worker {worker_id}: {error}")
            else:
                with log_file.open("r", errors="ignore") as lf:
                    log_content = lf.read()
                    log_content = log_content.lower()
                    if "fail" in log_content or "warn" in log_content:
                        error = "fail_log"
                        error_category = "fail_log"
                        logger.error(f"Worker {worker_id}: Detected 'fail' or 'warn' in log file.")
            
        if error is None:
            error = "None"
            error_category = "None"

        result_queue.put(WorkResult(
            worker_id=worker_id,
            ops=ops_executed,
            cov_file=result_cov,
            log_file=log_file if log_file.exists() else None,
            debug_log_file=worker_log if worker_log.exists() else None,
            linux_name=work.linux_name,
            exec_time=time.monotonic() - t0,
            mode=work.mode,
            seed_id=work.seed_id,
            error=error,
            error_category=error_category,
        ))
