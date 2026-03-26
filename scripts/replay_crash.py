#!/usr/bin/env -S uv run --script

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run import Driver, start_qemu
from scripts.fuzz_common import connect_to_kmod, read_kmod_state, read_work_conserving_status


@dataclass(frozen=True)
class ReplayResult:
    crash_dir: Path
    rerun_dir: Path
    linux_name: str
    num_cpus: int
    mem_mb: int
    executed_ops: int
    total_ops: int
    work_conserving_broken: bool | None


def load_ops(crash_dir: Path) -> tuple[str, list[tuple[int, int, int, int]]]:
    ops_path = crash_dir / "ops.json"
    data = json.loads(ops_path.read_text())
    linux_name = data["linux_name"]
    ops = [tuple(op) for op in data["ops"]]
    return linux_name, ops


def infer_num_cpus(crash_dir: Path) -> int | None:
    log_path = crash_dir / "worker.log"
    if not log_path.exists():
        return None

    content = log_path.read_text(errors="ignore")
    patterns = [
        r"Brought up \d+ node[s]?, (\d+) CPUs",
        r"Allowing (\d+) present CPUs plus",
        r"Num\. threads per package:\s+(\d+)",
    ]
    for pattern in patterns:
        match = re.search(pattern, content)
        if match:
            return int(match.group(1))
    return None


def send_op(sock, sf, op: tuple[int, int, int, int], debug_log: TextIO) -> bool:
    payload = f"{op[0]},{op[1]},{op[2]},{op[3]}\n".encode()
    sock.sendall(payload)
    debug_log.write(f"SEND OP: {op[0]},{op[1]},{op[2]},{op[3]}\n")
    debug_log.flush()

    state = read_kmod_state(sf)
    if state is None:
        raise RuntimeError("executor socket closed while waiting for task state")

    debug_log.write(
        f"RECV executed={state.executed} task_states={state.task_states}\n"
    )
    debug_log.flush()
    return state.executed


def send_op_with_retry(
    sock,
    sf,
    idx: int,
    op: tuple[int, int, int, int],
    debug_log: TextIO,
    retries: int = 50,
) -> None:
    for attempt in range(retries):
        executed = send_op(sock, sf, op, debug_log)
        if executed:
            if attempt > 0:
                debug_log.write(
                    f"RETRY step={idx} succeeded_after={attempt + 1}\n"
                )
                debug_log.flush()
            return
        debug_log.write(
            f"RETRY step={idx} attempt={attempt + 1} executed=False\n"
        )
        debug_log.flush()

    raise RuntimeError(
        f"op {idx} was rejected after {retries} retries: "
        f"{op[0]},{op[1]},{op[2]},{op[3]}"
    )


def replay_crash(
    crash_dir: Path,
    linux_name: str | None,
    num_cpus: int | None,
    mem_mb: int,
    timeout_sec: float,
) -> ReplayResult:
    crash_dir = crash_dir.resolve()
    ops_linux_name, ops = load_ops(crash_dir)
    linux_name = linux_name or ops_linux_name
    num_cpus = num_cpus or infer_num_cpus(crash_dir) or 3

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    rerun_dir = crash_dir / f"rerun_{timestamp}"
    rerun_dir.mkdir(parents=True, exist_ok=False)

    log_file = rerun_dir / "worker.log"
    sock_file = rerun_dir / "executor.sock"
    debug_log_path = rerun_dir / "worker.debug.log"
    proc = None
    sock = None

    try:
        proc = start_qemu(
            driver=Driver(name="executor", num_cpus=num_cpus, mem_mb=mem_mb),
            linux_name=linux_name,
            log_file=log_file,
            sock_file=sock_file,
            quiet=True,
        )

        sock = connect_to_kmod(sock_file, timeout=10.0, retries=200)
        sock.settimeout(timeout_sec)
        sf = sock.makefile("rb")

        state = read_kmod_state(sf)
        if state is None:
            raise RuntimeError("executor socket closed before ready signal")

        executed_ops = 0
        with debug_log_path.open("w") as debug_log:
            debug_log.write(
                f"Replay start: linux_name={linux_name} num_cpus={num_cpus} mem_mb={mem_mb}\n"
            )
            for idx, op in enumerate(ops):
                send_op_with_retry(sock, sf, idx, op, debug_log)
                executed_ops += 1

            sock.sendall(b"EXIT\n")
            debug_log.write("SEND EXIT\n")
            debug_log.flush()
            work_conserving_broken = read_work_conserving_status(sf)
            debug_log.write(
                f"RECV work_conserving_broken={work_conserving_broken}\n"
            )

        proc.wait(timeout=timeout_sec)

        return ReplayResult(
            crash_dir=crash_dir,
            rerun_dir=rerun_dir,
            linux_name=linux_name,
            num_cpus=num_cpus,
            mem_mb=mem_mb,
            executed_ops=executed_ops,
            total_ops=len(ops),
            work_conserving_broken=work_conserving_broken,
        )
    except Exception:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        if proc is not None:
            try:
                proc.kill()
                proc.wait()
            except Exception:
                pass
        raise
    finally:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Replay the recorded ops from a crash directory through driver=executor."
    )
    parser.add_argument("crash_dir", type=Path)
    parser.add_argument("--linux_name", type=str, default=None)
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=512)
    parser.add_argument("--timeout_sec", type=float, default=120.0)
    args = parser.parse_args()

    result = replay_crash(
        crash_dir=args.crash_dir,
        linux_name=args.linux_name,
        num_cpus=args.num_cpus,
        mem_mb=args.mem_mb,
        timeout_sec=args.timeout_sec,
    )

    status = (
        "BROKEN"
        if result.work_conserving_broken
        else "NOT_BROKEN"
        if result.work_conserving_broken is not None
        else "UNKNOWN"
    )
    print(f"crash_dir: {result.crash_dir}")
    print(f"rerun_dir: {result.rerun_dir}")
    print(f"linux_name: {result.linux_name}")
    print(f"num_cpus: {result.num_cpus}")
    print(f"mem_mb: {result.mem_mb}")
    print(f"ops: {result.executed_ops}/{result.total_ops}")
    print(f"work_conserving: {status}")


if __name__ == "__main__":
    main()
