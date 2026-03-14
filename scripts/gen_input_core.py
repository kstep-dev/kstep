import logging
import random

from .consts import LOGS_DIR
from .gen_input_state import GenState, TASK_RUNNABLE, TASK_SLEEPING, TASK_ON_CPU
from .gen_input_ops import build_ops, RESOURCE_TASK, RESOURCE_CGROUP, OP_NAME_TO_TYPE, OP_TYPE_TO_NAME
from .input_seq import InputSeq


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

OPS = build_ops()

def init_genstate(max_tasks: int, max_cgroups: int, cpus: int, seed: int) -> GenState:
    return GenState(max_tasks=max_tasks, max_cgroups=max_cgroups, cpus=cpus, rnd=random.Random(seed))

def _generate_next_command(m: GenState) -> tuple[int, int, int, int]:
    """Generate the next command given actual task states from the kmod.
    task_states is a list of {"id": int, "state": int} dicts."""
    applicable = [op for op in OPS if op.is_applicable(m)]
    op = choose_op(m, applicable if applicable else OPS)
    for res in op.requires:
        if not resource_available(m, res):
            prod = pick_producer(OPS, res)
            if prod and prod.is_applicable(m):
                return prod.emit(m)
    return op.emit(m)

def generate_next_command(m: GenState, task_states: list[dict]) -> tuple[int, int, int, int]:
    m.update_from_kmod(task_states)
    while True:
        op = _generate_next_command(m)
        if op:
            return op
