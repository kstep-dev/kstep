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
from scripts.gen_input_core import generate_next_command, init_genstate
from scripts.gen_input_ops import OP_NAME_TO_TYPE

_CRASH_MARKERS = [
    b"Kernel panic",
    b"kernel BUG at",
    b"BUG: ",
    b"KASAN: ",
    b"UBSAN: ",
    b"general protection fault",
    b"Oops: ",
]


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
) -> None:
    """Worker loop: run QEMU instances and report results to the manager."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)   # manager owns shutdown

    rng = random.Random(os.getpid() ^ (worker_id * 0x9E3779B9))
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
        crashed = False
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
            )

            # QEMU uses wait=on and blocks until we connect; retry until ready.
            try:
                sock = connect_to_kmod(sock_file, timeout=10.0, retries=200)
            except RuntimeError:
                proc.kill()
                raise RuntimeError("Failed to connect to kmod socket")

            sock.settimeout(60.0)
            sf = sock.makefile("rb")
            task_states = read_kmod_state(sf)  # kmod signals readiness

            gen = init_genstate(
                max_tasks=10,
                max_cgroups=10,
                cpus=driver.num_cpus,
                seed=rng.randint(0, 2**32 - 1),
            )
            for i in range(work.steps):
                op, a, b, c = generate_next_command(gen)
                if op == OP_NAME_TO_TYPE["TICK_REPEAT"]:
                    for _ in range(a):
                        ops_executed.append((OP_NAME_TO_TYPE["TICK"], 0, 0, 0))
                else:
                    ops_executed.append((op, a, b, c))
                sock.sendall(f"{op},{a},{b},{c}\n".encode())
                task_states = read_kmod_state(sf)
                if task_states is None:
                    sock.sendall(f"EXIT\n".encode())
                    raise RuntimeError("Failed to read task states")
                gen.update_from_kmod(task_states)
                logger.debug(f"STEP {i}: task_states updated: {gen.task_state}")


            sock.sendall(b"EXIT\n")
            sock.close()
            try:
                proc.wait(timeout=120)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

            cov_path = log_file.with_suffix(".cov")
            if cov_path.exists() and cov_path.stat().st_size > 0:
                result_cov = cov_path

        except Exception as exc:
            error = str(exc)
            if proc is not None:
                try:
                    proc.kill()
                    proc.wait()
                except Exception:
                    pass
            crashed = _check_for_crash(log_file)
            logger.error(f"Worker {worker_id}: {'[CRASH] ' if crashed else ''}{exc}")
        

        with log_file.open("r", errors="ignore") as lf:
            log_content = lf.read()
            if "fail" in log_content or "Fail" in log_content:
                logger.error(f"Worker {worker_id}: Detected 'fail' or 'Fail' in log file.")
                error = "fail"


        result_queue.put(WorkResult(
            worker_id=worker_id,
            ops=ops_executed,
            cov_file=result_cov,
            log_file=log_file if log_file.exists() else None,
            debug_log_file=worker_log if worker_log.exists() else None,
            linux_name=work.linux_name,
            exec_time=time.monotonic() - t0,
            seed_id=work.seed_id,
            error=error,
            crashed=crashed,
        ))
