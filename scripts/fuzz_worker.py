"""Fuzzer worker: runs one QEMU instance per loop iteration and reports
raw coverage + the executed op sequence back to the manager."""

from __future__ import annotations

import logging
import multiprocessing as mp
import os
import random
import signal
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Optional

from run import Driver, start_qemu
from scripts.fuzz_common import CheckerStatus, Ops, WorkItem, WorkResult, worker_paths
from scripts.gen_input_core import (
    _op_matches_task_state,
    generate_next_command,
    init_genstate,
    replay_update_genstate,
)
from scripts.gen_input_ops import OP_NAME_TO_TYPE, OP_TYPE_TO_NAME
from scripts.gen_input_state import GenState

# Marker byte written by kstep_write_state
OP_TYPE_NR = len(OP_NAME_TO_TYPE)
CHECKER_STATUS_PREFIX = b"CHECKER,"

_CRASH_MARKERS = [
    b"kernel panic",
    b"kernel bug at",
    b"bug: ",
    b"kasan: ",
    b"ubsan: ",
    b"general protection fault",
    b"oops: ",
    b"error",
    b"warn",
    b"fail"
]

_KSTEP_START_MARKER = b"Run /init as init process"


def _read_log_bytes(log_file: Path) -> bytes:
    try:
        return log_file.read_bytes()
    except Exception:
        return b""


def _has_kstep_start_marker(log_file: Path) -> bool:
    return _KSTEP_START_MARKER in _read_log_bytes(log_file)


def _log_region_after_kstep_start(log_file: Path) -> bytes:
    content = _read_log_bytes(log_file)
    start = content.find(_KSTEP_START_MARKER)
    if start < 0:
        return content
    return content[start:]


# Detect whether the QEMU console log contains a kernel crash signature.
def _check_for_crash(log_file: Path) -> bool:
    try:
        content = _log_region_after_kstep_start(log_file).lower()
        return any(marker in content for marker in _CRASH_MARKERS)
    except Exception:
        return False


@dataclass
class KmodState:
    task_states: list[dict]
    executed: bool
    executed_steps: int

@dataclass
class FuzzWorkerSession:
    proc: subprocess.Popen
    sock: socket.socket
    sock_file_path: Path
    sock_file: BinaryIO
    gen: GenState
    logger: logging.Logger

    # Connect the worker session to the executor socket and initialize I/O state.
    def __init__(
        self,
        gen,
        proc,
        socke_file_path,
        logger,
        timeout,
        retries: int = 200,
    ):
        self.gen = gen
        self.proc = proc
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock_file_path = socke_file_path
        self.logger = logger
        self.timeout = timeout

        sleep_time = timeout / retries
        for _ in range(retries):
            try:
                self.sock.connect(str(self.sock_file_path))
                self.sock.settimeout(timeout)
                self.sock_file = self.sock.makefile("rb")
                return
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(sleep_time)
        self.kill()
        raise RuntimeError(f"Timeout: Timed out connecting to {self.sock_file}")

    # Close session resources and optionally terminate the backing QEMU process.
    def kill(self, kill_proc=True):
        for resource in (self.sock_file, self.sock):
            try:
                resource.close()
            except Exception:
                pass
        if kill_proc:
            try:
                self.proc.kill()
            finally:
                self.proc.wait()
        else:
            try:
                self.proc.wait(timeout=self.timeout)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("Timeout: Timed out waiting for the QEMU process") from exc

    # Read one protocol frame from the executor socket.
    def _read_frame(self) -> Optional[bytes]:
        buf = bytearray()
        while True:
            ch = self.sock_file.read(1)
            if not ch:
                return None
            buf += ch
            if ch == b"\n":
                return bytes(buf)

    # Decode the next task-state frame from the executor stream.
    def read_kmod_state(self) -> Optional[KmodState]:
        """Read state from kmod socket, discarding TTY echo."""
        while True:
            line = self._read_frame()
            if not line:
                return None
            if line[0] != OP_TYPE_NR:
                continue

            payload = line[1:-1]
            if not payload:
                return KmodState(task_states=[], executed=False, executed_steps=0)
            if len(payload) < 2 or (len(payload) - 2) % 2 != 0:
                continue

            executed = bool(payload[0])
            executed_steps = payload[1] - 11
            if executed_steps < 0:
                continue
            task_states = [
                {"id": payload[i] - 11, "state": payload[i + 1]}
                for i in range(2, len(payload), 2)
            ]
            return KmodState(
                task_states=task_states,
                executed=executed,
                executed_steps=executed_steps,
            )

    # Decode the final checker status sent after EXIT.
    def read_checker_status(self) -> Optional[CheckerStatus]:
        while True:
            line = self._read_frame()
            if not line:
                return None
            if not line.startswith(CHECKER_STATUS_PREFIX):
                continue

            payload = line[len(CHECKER_STATUS_PREFIX):-1].strip()
            parts = payload.split(b",", 2)
            if len(parts) != 3:
                continue

            work_conserving_broken, cfs_util_decay_broken, rt_util_decay_broken = parts

            try:
                result = CheckerStatus(
                    work_conserving_broken=(work_conserving_broken == b"1"),
                    cfs_util_decay_broken=(int(cfs_util_decay_broken) > 0),
                    rt_util_decay_broken=(int(rt_util_decay_broken) > 0),
                )
            except Exception as _:
                continue

            return result

    # Send one op to kmod and fold the returned state into the generator model.
    def send_op(self, op: int, a: int, b: int, c: int) -> int:
        self.sock.sendall(f"{op},{a},{b},{c}\n".encode())

        state = self.read_kmod_state()
        if state is None:
            self.sock.sendall(b"EXIT\n")
            raise RuntimeError("Readfail: Failed to read task states")

        self.gen.update_from_kmod(state.task_states)
        if not state.executed:
            return 0
        return state.executed_steps


class FuzzWorker:
    def __init__(
        self,
        worker_id: int,
        task_queue: "mp.Queue[Optional[WorkItem]]",
        result_queue: "mp.Queue[WorkResult]",
        driver: Driver,
        linux_name: str,
        cross_scheduler: bool = False,
        qemu_cpus: Optional[str] = None,
        rng_seed: Optional[int] = None,
        base_dir: Optional[Path] = None,
        io_timeout_sec: float = 120.0,
    ) -> None:
        self.worker_id = worker_id
        self.task_queue = task_queue
        self.result_queue = result_queue
        self.driver = driver
        self.linux_name = linux_name
        self.cross_scheduler = cross_scheduler
        self.qemu_cpus = qemu_cpus
        self.io_timeout_sec = io_timeout_sec
        self.paths = (
            worker_paths(worker_id, base_dir=base_dir)
            if base_dir is not None
            else worker_paths(worker_id)
        )

        seed = rng_seed if rng_seed is not None else (os.getpid() ^ (worker_id + 1) * 0x9E3779B9)
        self.rng = random.Random(seed)
        self.logger, self.handler = self._init_logger()

    # Build the per-worker debug logger.
    def _init_logger(self) -> tuple[logging.Logger, logging.FileHandler]:
        logger = logging.getLogger(f"worker_{self.worker_id}")
        logger.setLevel(logging.DEBUG)
        logger.propagate = False
        logger.handlers.clear()

        handler = logging.FileHandler(self.paths.debug_log_file, mode="w")
        handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
        logger.addHandler(handler)
        return logger, handler

    # Clear the previous work item's debug log before starting a new run.
    def _reset_debug_log(self) -> None:
        self.handler.stream.seek(0)
        self.handler.stream.truncate()

    # Start QEMU, connect to kmod, and initialize generator state for this work item.
    def _start_session(self, work: WorkItem) -> FuzzWorkerSession:
        proc = start_qemu(
            driver=self.driver,
            linux_name=self.linux_name,
            log_file=self.paths.log_file,
            sock_file=self.paths.sock_file,
            quiet=True,
            cpu_affinity=self.qemu_cpus,
            if_update_latest=False
        )

        gen_seed = 0 if work.mode in ("replay") else self.rng.randint(0, 2**32 - 1)
        kstep_cpus = self.driver.num_cpus - 1
        max_tasks = self.rng.randint(kstep_cpus * 1, kstep_cpus * 6)
        max_cgroups = self.rng.randint(kstep_cpus * 1, kstep_cpus * 6)
        gen = init_genstate(
            max_tasks,
            max_cgroups,
            self.driver.num_cpus,
            gen_seed,
            cross_scheduler=self.cross_scheduler,
        )

        session = FuzzWorkerSession(gen, proc, self.paths.sock_file, self.logger, self.io_timeout_sec)

        if session.read_kmod_state() is None:
            session.kill()
            raise RuntimeError("Readfail: kmod socket closed before ready signal")

        return session

    # Execute generated operations until the requested number of successful steps is reached.
    def _execute_generated_ops(
        self,
        work: WorkItem,
        session: FuzzWorkerSession,
        ops_executed: Optional[Ops] = None,
        special_pivot_idxs: Optional[list[int]] = None,
        *,
        log_prefix: str = "STEP",
    ) -> tuple[Ops, list[int]]:
        if ops_executed is None:
            ops_executed = []
        if special_pivot_idxs is None:
            special_pivot_idxs = []

        generated = 0
        while generated < work.steps:
            op, a, b, c = generate_next_command(session.gen)
            executed_steps = session.send_op(op, a, b, c)
            if executed_steps > 0:
                
                if op == OP_NAME_TO_TYPE["TICK_REPEAT"]:
                    for i in range(executed_steps):
                        ops_executed.append((OP_NAME_TO_TYPE["TICK"], 0, 0, 0))
                        self.logger.debug(
                            f"{log_prefix}: op={OP_NAME_TO_TYPE["TICK"]},0,0,0 "
                            f"executed_steps={executed_steps} task_state={session.gen.task_state}"
                        )
                    if executed_steps < a and executed_steps > 0:
                        special_pivot_idxs.append(len(ops_executed) - 1)
                        self.logger.debug(
                            f"{log_prefix}: special_pivot={special_pivot_idxs[-1]} "
                            f"special_state=1"
                        )
                else:
                    self.logger.debug(
                        f"{log_prefix}: op={op},{a},{b},{c} "
                        f"executed_steps={executed_steps} task_state={session.gen.task_state}"
                    )
                    ops_executed.append((op, a, b, c))
                    generated += 1
        return ops_executed, special_pivot_idxs

    # Replay the seed prefix and optionally extend it with generated mutation steps.
    def _execute_replay_or_mutate(
        self,
        work: WorkItem,
        session: FuzzWorkerSession,
    ) -> tuple[Ops, list[int]]:
        assert work.ops is not None
        ops_executed: Ops = []
        special_pivot_idxs: list[int] = []
        prefix_len = len(work.ops)
        if work.mode == "mutate" and work.pivot_idx is not None:
            prefix_len = min(work.pivot_idx + 1, len(work.ops))

        replayed = 0

        for i, (op, a, b, c) in enumerate(work.ops[:prefix_len]):
            for retry in range(50):
                if _op_matches_task_state(session.gen, (op, a, b, c)):
                    executed_steps = session.send_op(op, a, b, c)
                    if executed_steps > 0:
                        self.logger.debug(
                            f"REPLAY: op={op},{a},{b},{c} executed_steps={executed_steps} task_state={session.gen.task_state}"
                        )
                        ops_executed.append((op, a, b, c))
                        replay_update_genstate(session.gen, op, a, b, c)
                        if op != OP_NAME_TO_TYPE["TICK"] or op != OP_NAME_TO_TYPE["TICK_REPEAT"]:
                            replayed += 1
                        break
                self.logger.debug(f"REPLAY {i} retry {retry}: task_state={session.gen.task_state}")
            else:
                op_name = OP_TYPE_TO_NAME.get(op, str(op))
                actual = session.gen.task_state.get(a, "not_found")
                session.sock.sendall(b"EXIT\n")
                raise RuntimeError(
                    f"Replayfail: replay mismatch at step {i} after 50 retries: "
                    f"{op_name}(task={a}) task_state={actual!r}"
                )

        if work.mode == "mutate" and work.pivot_idx is not None:
            work.steps = max(int(work.steps * 0.2), work.steps - replayed)
            return self._execute_generated_ops(
                work,
                session,
                ops_executed,
                special_pivot_idxs,
                log_prefix="MUTATE-GEN",
            )
        return ops_executed, special_pivot_idxs

    # Dispatch execution based on whether the work item is fresh, replay, or mutate.
    def _execute_work(
        self,
        work: WorkItem,
        session: FuzzWorkerSession,
    ) -> tuple[Ops, list[int]]:
        if work.mode in ("replay", "mutate"):
            return self._execute_replay_or_mutate(work, session)
        return self._execute_generated_ops(work, session)

    # Ask the executor to exit cleanly and report checker outputs.
    def _finish_session(
        self,
        session: FuzzWorkerSession,
    ) -> tuple[Optional[str], Optional[CheckerStatus]]:
        session.sock.sendall(b"EXIT\n")
        checker_status = session.read_checker_status()
        self.logger.info(
            "EXIT status: work_conserving_broken=%s cfs_util_decay_broken=%s rt_util_decay_broken=%s",
            None if checker_status is None else checker_status.work_conserving_broken,
            None if checker_status is None else checker_status.cfs_util_decay_broken,
            None if checker_status is None else checker_status.rt_util_decay_broken,
        )

        session.kill(kill_proc=False)
        if checker_status is None:
            return "checker: missing checker status", None
        return None, checker_status

    # Compute how many ops must execute for this work item to count as complete.
    def _expected_ops(self, work: WorkItem) -> int:
        if work.mode == "replay":
            assert work.ops is not None
            return len(work.ops)
        if work.mode == "mutate":
            assert work.ops is not None
            if work.pivot_idx is not None:
                return min(work.pivot_idx + 1, len(work.ops))
            return len(work.ops)
        return work.steps

    # Check the run result for short execution or error markers in the worker log.
    def _validate_result(
        self,
        work: WorkItem,
        ops_executed: Ops,
        error: Optional[str],
    ) -> Optional[str]:
        if error is not None:
            return error

        expected_ops = self._expected_ops(work)
        if len(ops_executed) < expected_ops:
            error = f"missop: expected {expected_ops}, got {len(ops_executed)}"
            self.logger.error(f"Worker {self.worker_id}: {error}")
            return error

        if not _has_kstep_start_marker(self.paths.log_file):
            self.logger.error(
                f"Worker {self.worker_id}: Missing kSTEP start marker "
                f"{_KSTEP_START_MARKER.decode('utf-8', errors='ignore')!r} in log file."
            )
            return "bootfail: missing kstep start marker"

        if _check_for_crash(self.paths.log_file):
            return "errorlog: error log"
        return None

    # Run one work item end-to-end and package the resulting execution record.
    def _run_one(self, work: WorkItem) -> WorkResult:
        self.paths.sock_file.unlink(missing_ok=True)
        self._reset_debug_log()

        t0 = time.monotonic()
        ops_executed: Ops = []
        special_pivot_idxs: list[int] = []
        error: Optional[str] = None
        checker_status: Optional[CheckerStatus] = None
        session: Optional[FuzzWorkerSession] = None

        try:
            session = self._start_session(work)
        except Exception as exc:
            return WorkResult(
                worker_id=self.worker_id,
                ops=ops_executed,
                exec_time=time.monotonic() - t0,
                mode=work.mode,
                seed_id=work.seed_id,
                checker_status=checker_status,
                error=str(exc),
            )

        try:
            ops_executed, special_pivot_idxs = self._execute_work(work, session)
            error, checker_status = self._finish_session(session)
        except Exception as exc:
            session.kill(kill_proc=True)
            self.logger.error(f"Worker {self.worker_id}: {exc}")
            if _check_for_crash(self.paths.log_file):
                error = "qemucrash: qemucrash"
            elif not _has_kstep_start_marker(self.paths.log_file):
                error = "bootfail: missing kstep start marker"
            else:
                error = str(exc)
            self.logger.error("execute op fail: " + error)

        error = self._validate_result(work, ops_executed, error)
        return WorkResult(
            worker_id=self.worker_id,
            ops=ops_executed,
            exec_time=time.monotonic() - t0,
            mode=work.mode,
            seed_id=work.seed_id,
            checker_status=checker_status,
            special_pivot_idxs=special_pivot_idxs,
            error=error,
        )

    # Consume queued work items until the manager sends the shutdown sentinel.
    def run(self) -> None:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        while True:
            work: Optional[WorkItem] = self.task_queue.get()
            if work is None:
                break
            self.result_queue.put(self._run_one(work))
        return


# Launch a single worker process wrapper around the FuzzWorker class.
def worker_main(
    worker_id: int,
    task_queue: "mp.Queue[Optional[WorkItem]]",
    result_queue: "mp.Queue[WorkResult]",
    driver: Driver,
    linux_name: str,
    cross_scheduler: bool = False,
    qemu_cpus: Optional[str] = None,
) -> None:
    worker = FuzzWorker(
        worker_id=worker_id,
        task_queue=task_queue,
        result_queue=result_queue,
        driver=driver,
        linux_name=linux_name,
        cross_scheduler=cross_scheduler,
        qemu_cpus=qemu_cpus,
    )
    worker.run()
