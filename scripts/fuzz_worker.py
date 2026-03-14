"""Fuzzer worker: runs one QEMU instance per loop iteration and reports
raw coverage + the executed op sequence back to the manager."""

from __future__ import annotations

import logging
import multiprocessing as mp
import os
import random
import signal
import socket as socket_mod
import subprocess
import time
from pathlib import Path
from typing import Optional

from run import Driver, start_qemu
from scripts.fuzz_common import Ops, WorkItem, WorkResult
from scripts.gen_input_core import generate_next_command, init_genstate
from scripts.gen_input_ops import OP_NAME_TO_TYPE

_OP_TYPE_NR = 16   # marker byte written by kstep_write_state

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


def _read_state(sf) -> list[dict]:
    """Block until a kmod state message arrives, discarding any TTY echo.
    Raises EOFError if the socket is closed (QEMU exited)."""
    while True:
        line = sf.readline()
        if not line:
            raise EOFError("kmod socket closed (QEMU exited)")
        if line[0] == _OP_TYPE_NR:
            payload = line[1:-1]   # strip marker byte and trailing '\n'
            return [{"id": payload[i], "state": payload[i + 1]}
                    for i in range(0, len(payload), 2)]


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
            proc = start_qemu(
                driver=driver,
                linux_name=work.linux_name,
                log_file=log_file,
                sock_file=sock_file,
                quiet=True,
            )

            # QEMU uses wait=on and blocks until we connect; retry until ready.
            sock = socket_mod.socket(socket_mod.AF_UNIX, socket_mod.SOCK_STREAM)
            for _ in range(200):
                try:
                    sock.connect(str(sock_file))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    time.sleep(0.05)
            else:
                proc.kill()
                raise RuntimeError("Timed out connecting to kmod socket")

            # Per-op timeout: if kmod doesn't respond within 30 s, abort.
            sock.settimeout(30.0)
            sf = sock.makefile("rb")
            task_states = _read_state(sf)   # kmod signals readiness

            if work.mode == "fresh":
                gen = init_genstate(
                    max_tasks=10,
                    max_cgroups=10,
                    cpus=driver.num_cpus,
                    seed=rng.randint(0, 2**32 - 1),
                )
                for _ in range(work.steps):
                    op, a, b, c = generate_next_command(gen, task_states)
                    if op == OP_NAME_TO_TYPE["TICK_REPEAT"]:
                        for _ in range(a):
                            ops_executed.append((OP_NAME_TO_TYPE["TICK"], 0, 0, 0))
                    else:
                        ops_executed.append((op, a, b, c))
                    sock.sendall(f"{op},{a},{b},{c}\n".encode())
                    task_states = _read_state(sf)

            else:   # replay exact seed (no mutation)
                for op, a, b, c in work.ops:
                    ops_executed.append((op, a, b, c))
                    sock.sendall(f"{op},{a},{b},{c}\n".encode())
                    task_states = _read_state(sf)

            sock.sendall(b"EXIT\n")
            sock.close()
            try:
                proc.wait(timeout=60)
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
            level = logging.ERROR if crashed else logging.WARNING
            logging.log(level, f"Worker {worker_id}: {'[CRASH] ' if crashed else ''}{exc}")

        result_queue.put(WorkResult(
            worker_id=worker_id,
            ops=ops_executed,
            cov_file=result_cov,
            log_file=log_file if log_file.exists() else None,
            linux_name=work.linux_name,
            exec_time=time.monotonic() - t0,
            seed_id=work.seed_id,
            error=error,
            crashed=crashed,
        ))
