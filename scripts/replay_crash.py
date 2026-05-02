#!/usr/bin/env -S uv run --script

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run import Driver
from scripts.fuzz_common import WorkItem, worker_dir
from scripts.fuzz_worker import FuzzWorker


@dataclass(frozen=True)
class ReplayResult:
    crash_dir: Path
    rerun_dir: Path
    name: str
    num_cpus: int
    mem_mb: int
    executed_ops: int
    total_ops: int


def load_ops(crash_dir: Path) -> tuple[str, list[tuple[int, int, int, int]]]:
    ops_path = crash_dir / "ops.json"
    data = json.loads(ops_path.read_text())
    name = data["name"]
    ops = [tuple(op) for op in data["ops"]]
    return name, ops


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


def replay_crash(
    crash_dir: Path,
    name: str | None,
    num_cpus: int | None,
    mem_mb: int,
    timeout_sec: float,
) -> ReplayResult:
    crash_dir = crash_dir.resolve()
    ops_name, ops = load_ops(crash_dir)
    name = name or ops_name
    num_cpus = num_cpus or infer_num_cpus(crash_dir) or 3

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    rerun_dir = crash_dir / f"rerun_{timestamp}"
    rerun_dir.mkdir(parents=True, exist_ok=False)
    task_queue: "mp.Queue[WorkItem | None]" = mp.Queue()
    result_queue: "mp.Queue" = mp.Queue()
    task_queue.put(WorkItem(mode="replay", steps=0, ops=ops))
    task_queue.put(None)

    worker = FuzzWorker(
        worker_id=0,
        task_queue=task_queue,
        result_queue=result_queue,
        driver=Driver(name="executor", num_cpus=num_cpus, mem_mb=mem_mb),
        name=name,
        base_dir=rerun_dir,
        io_timeout_sec=timeout_sec,
    )
    worker.run()

    result = result_queue.get(timeout=5)
    print(f"replay error: {result.error_category}: {result.error}")

    wdir = worker_dir(0, base_dir=rerun_dir)

    if wdir.sock.exists():
        wdir.sock.unlink(missing_ok=True)

    return ReplayResult(
        crash_dir=crash_dir,
        rerun_dir=rerun_dir,
        name=name,
        num_cpus=num_cpus,
        mem_mb=mem_mb,
        executed_ops=len(result.ops),
        total_ops=len(ops),
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Replay the recorded ops from a crash directory through driver=executor."
    )
    parser.add_argument("crash_dir", type=Path)
    parser.add_argument("--name", type=str, default=None)
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=512)
    parser.add_argument("--timeout_sec", type=float, default=120.0)
    args = parser.parse_args()

    result = replay_crash(
        crash_dir=args.crash_dir,
        name=args.name,
        num_cpus=args.num_cpus,
        mem_mb=args.mem_mb,
        timeout_sec=args.timeout_sec,
    )

    
    print(f"crash_dir: {result.crash_dir}")
    print(f"rerun_dir: {result.rerun_dir}")
    print(f"name: {result.name}")
    print(f"num_cpus: {result.num_cpus}")
    print(f"mem_mb: {result.mem_mb}")
    print(f"ops: {result.executed_ops}/{result.total_ops}")


if __name__ == "__main__":
    main()
