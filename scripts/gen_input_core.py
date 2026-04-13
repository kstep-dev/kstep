import random

from .gen_input_state import GenState, TASK_SLEEPING, TASK_ON_CPU
from .gen_input_ops import (
    OpWeight,
    build_ops,
    RESOURCE_TASK,
    RESOURCE_CGROUP,
    OP_NAME_TO_TYPE,
)


def choose_op(m: GenState, ops):
    weighted_ops = []
    for op in ops:
        weight = op.resolved_weight(m)
        if weight > 0:
            weighted_ops.append((op, weight))
    if not weighted_ops:
        raise ValueError("No applicable operations have a positive weight")
    total = sum(weight for _, weight in weighted_ops)
    pick = m.rnd.randint(1, total)
    cur = 0
    for op, weight in weighted_ops:
        cur += weight
        if pick <= cur:
            return op
    return weighted_ops[-1][0]

def resource_available(m: GenState, res: str) -> bool:
    if res == RESOURCE_TASK:
        return m.has_tasks()
    if res == RESOURCE_CGROUP:
        return m.has_cgroups()
    return False

def pick_producer(m: GenState, ops, res: str):
    for op in ops:
        if res in op.produces and op.resolved_weight(m) > 0:
            return op
    return None

OPS = []
_OP_REPLAY = {}


def configure_ops(weight_overrides: dict[str, OpWeight] | None = None) -> None:
    global OPS, _OP_REPLAY
    OPS = build_ops(weight_overrides=weight_overrides)
    _OP_REPLAY = {OP_NAME_TO_TYPE[op.name]: op.replay for op in OPS if op.replay}


configure_ops()

def init_genstate(
    max_tasks: int,
    max_cgroups: int,
    cpus: int,
    seed: int,
    cross_scheduler: bool = False,
) -> GenState:
    return GenState(
        max_tasks=max_tasks,
        max_cgroups=max_cgroups,
        cpus=cpus,
        rnd=random.Random(seed),
        cross_scheduler=cross_scheduler,
    )

def _generate_next_command(m: GenState) -> tuple[int, int, int, int]:
    """Generate the next command given actual task states from the kmod.
    task_states is a list of {"id": int, "state": int} dicts."""
    applicable = [op for op in OPS if op.is_applicable(m)]
    op = choose_op(m, applicable if applicable else OPS)
    for res in op.requires:
        if not resource_available(m, res):
            prod = pick_producer(m, OPS, res)
            if prod and prod.is_applicable(m):
                return prod.emit(m)
    return op.emit(m)

def _op_matches_task_state(m: GenState, op: tuple[int, int, int, int]) -> bool:
    op_type, a, _, _ = op
    task_state = m.task_state.get(a)

    if op_type == OP_NAME_TO_TYPE["TASK_WAKEUP"]:
        return task_state == TASK_SLEEPING
    if op_type in {
        OP_NAME_TO_TYPE["TASK_FORK"],
        OP_NAME_TO_TYPE["TASK_PIN"],
        OP_NAME_TO_TYPE["TASK_FIFO"],
        OP_NAME_TO_TYPE["TASK_CFS"],
        OP_NAME_TO_TYPE["TASK_PAUSE"],
        OP_NAME_TO_TYPE["TASK_SET_PRIO"],
    }:
        return task_state == TASK_ON_CPU
    return True

def generate_next_command(m: GenState) -> tuple[int, int, int, int]:
    while True:
        op = _generate_next_command(m)
        if op and _op_matches_task_state(m, op):
            return op


def replay_update_genstate(m: GenState, op: int, a: int, b: int, c: int) -> None:
    """Apply the GenState side-effects of a replayed op without calling emit().

    During replay, args come from the seed so emit() is not called.
    Task states are kept in sync via update_from_kmod(); this function
    handles the remaining state: cgroups, leaf_cgroups, and task2cgroups.
    Each op's replay logic lives in its Op.replay field in gen_input_ops.py.
    """
    fn = _OP_REPLAY.get(op)
    if fn:
        fn(m, a, b, c)
