import logging
import random

from .consts import LOGS_DIR
from .gen_input_state import (
    GenState,
    TASK_RUNNABLE,
    TASK_SLEEPING,
)
from .gen_input_ops import build_ops, RESOURCE_TASK, RESOURCE_CGROUP, OP_NAME_TO_TYPE


def choose_op(m: GenState, ops):
    total = sum(op.weight for op in ops)
    pick = m.rnd.randint(1, total)
    cur = 0
    for op in ops:
        cur += op.weight
        if pick <= cur:
            return op
    return ops[-1]


def resource_available(m: GenState, res: str) -> bool:
    if res == RESOURCE_TASK:
        return m.has_tasks()
    if res == RESOURCE_CGROUP:
        return m.has_cgroups()
    return False


def pick_producer(ops, res: str):
    for op in ops:
        if res in op.produces:
            return op
    return None


def generate_sequence(steps: int, max_tasks: int, max_cgroups: int, cpus: int, seed: int):
    logger = logging.getLogger("gen_input")
    rnd = random.Random(seed)
    m = GenState(max_tasks=max_tasks, max_cgroups=max_cgroups, cpus=cpus, rnd=rnd)
    ops = build_ops()
    seq = []
    while len(seq) < steps:
        op = choose_op(m, ops)
        for res in op.requires:
            if not resource_available(m, res):
                prod = pick_producer(ops, res)
                if prod and prod.is_applicable(m):
                    seq.append(prod.emit(m))
                    logger.info(f"generated op {len(seq)}: {seq[-1]}")
        if op.is_applicable(m):
            seq.append(op.emit(m))
            logger.info(f"generated op {len(seq)}: {seq[-1]}")
    return seq


def validate_sequence(seq, max_tasks: int, max_cgroups: int, cpus: int) -> bool:
    tasks = set()
    cgroups = set()
    cgroup_parent = {}
    cgroup_cpuset = {}
    task_state = {}
    logger = logging.getLogger("gen_input")

    def fail(idx: int, name: str, a: int, b: int, c: int, msg: str) -> bool:
        logger.error(f"validate op {idx}: ({name}, {a}, {b}, {c}) -> FAIL: {msg}")
        return False

    def check_cpu_range(begin: int, end: int) -> bool:
        return cpus >= 2 and 1 <= begin <= end <= (cpus - 1)

    for idx, (name, a, b, c) in enumerate(seq):
        if name == OP_NAME_TO_TYPE["TASK_CREATE"]:
            if a < 0 or a >= max_tasks or a in tasks:
                return fail(idx, name, a, b, c, "invalid task id")
            tasks.add(a)
            task_state[a] = TASK_SLEEPING
        elif name == OP_NAME_TO_TYPE["TASK_FORK"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for fork")
            if b < 0 or b >= max_tasks or b in tasks:
                return fail(idx, name, a, b, c, "invalid fork task id")
            tasks.add(b)
            task_state[b] = TASK_RUNNABLE
        elif name == OP_NAME_TO_TYPE["TASK_PIN"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for pin")
            if not check_cpu_range(b, c):
                return fail(idx, name, a, b, c, "invalid cpu range for pin")
        elif name == OP_NAME_TO_TYPE["TASK_FIFO"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for fifo")
        elif name == OP_NAME_TO_TYPE["TASK_CFS"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for cfs")
        elif name == OP_NAME_TO_TYPE["TASK_PAUSE"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for pause")
            task_state[a] = TASK_SLEEPING
        elif name == OP_NAME_TO_TYPE["TASK_WAKEUP"]:
            if a not in tasks or task_state.get(a) != TASK_SLEEPING:
                return fail(idx, name, a, b, c, "task not sleeping for wakeup")
            task_state[a] = TASK_RUNNABLE
        elif name == OP_NAME_TO_TYPE["TASK_SET_PRIO"]:
            if a not in tasks or task_state.get(a) != TASK_RUNNABLE:
                return fail(idx, name, a, b, c, "task not runnable for set prio")
        elif name == OP_NAME_TO_TYPE["CGROUP_CREATE"]:
            parent_id = a
            child_id = b
            if child_id < 0 or child_id >= max_cgroups or child_id in cgroups:
                return fail(idx, name, a, b, c, "invalid cgroup id")
            if parent_id != -1 and parent_id not in cgroups:
                return fail(idx, name, a, b, c, "invalid cgroup parent")
            cgroups.add(child_id)
            cgroup_parent[child_id] = parent_id
            if parent_id == -1:
                cgroup_cpuset[child_id] = (1, cpus - 1) if cpus >= 2 else (0, 0)
            else:
                cgroup_cpuset[child_id] = cgroup_cpuset.get(parent_id, (1, cpus - 1))
        elif name == OP_NAME_TO_TYPE["CGROUP_SET_CPUSET"]:
            if a not in cgroups:
                return fail(idx, name, a, b, c, "unknown cgroup for cpuset")
            if not check_cpu_range(b, c):
                return fail(idx, name, a, b, c, "invalid cpuset range")
            parent_id = cgroup_parent.get(a, -1)
            if parent_id != -1:
                parent_range = cgroup_cpuset.get(parent_id)
                if parent_range and not (parent_range[0] <= b <= c <= parent_range[1]):
                    return fail(idx, name, a, b, c, "cpuset outside parent range")
            cgroup_cpuset[a] = (b, c)
        elif name == OP_NAME_TO_TYPE["CGROUP_SET_WEIGHT"]:
            if a not in cgroups:
                return fail(idx, name, a, b, c, "unknown cgroup for weight")
        elif name == OP_NAME_TO_TYPE["CGROUP_ADD_TASK"]:
            if a not in cgroups or b not in tasks:
                return fail(idx, name, a, b, c, "unknown cgroup or task for add")
        elif name == OP_NAME_TO_TYPE["CPU_SET_FREQ"]:
            if cpus < 2 or b <= 0:
                return fail(idx, name, a, b, c, "invalid cpu freq or cpu count")
        elif name == OP_NAME_TO_TYPE["CPU_SET_CAPACITY"]:
            if cpus < 2 or b <= 0:
                return fail(idx, name, a, b, c, "invalid cpu capacity or cpu count")
        elif name in (OP_NAME_TO_TYPE["TICK"], OP_NAME_TO_TYPE["TICK_REPEAT"]):
            pass
        else:
            return fail(idx, name, a, b, c, "unknown op")

        logger.info(f"validate op {idx}: ({name}, {a}, {b}, {c}) -> OK")
    return True

def generate_input(
    steps: int,
    max_tasks: int,
    max_cgroups: int,
    cpus: int,
    seed: int | None,
) -> list[tuple[int, int, int, int]]:

    log_path = LOGS_DIR / f"gen_input_{seed}.log"
    
    logger = logging.getLogger("gen_input")
    logger.setLevel(logging.INFO)
    handler = logging.FileHandler(log_path, mode="w", encoding="utf-8")
    handler.setLevel(logging.INFO)
    logger.addHandler(handler)

    if seed is None:
        seed = random.SystemRandom().randint(1, 2**31 - 1)
    
    seq = generate_sequence(steps, max_tasks, max_cgroups, cpus, seed)
    if not validate_sequence(seq, max_tasks, max_cgroups, cpus):
        raise SystemExit("validation failed for generated sequence")

    return seq
