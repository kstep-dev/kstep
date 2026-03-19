from typing import Callable, List, Optional
from dataclasses import dataclass, field
from enum import IntEnum

from .gen_input_state import (
    GenState,
    TASK_SLEEPING,
    TASK_ON_CPU,
)

OP_NAME_TO_TYPE = {
    "TASK_CREATE": 0,
    "TASK_FORK": 1,
    "TASK_PIN": 2,
    "TASK_FIFO": 3,
    "TASK_CFS": 4,
    "TASK_PAUSE": 5,
    "TASK_WAKEUP": 6,
    "TASK_SET_PRIO": 7,
    "TICK": 8,
    "TICK_REPEAT": 9,
    "CGROUP_CREATE": 10,
    "CGROUP_SET_CPUSET": 11,
    "CGROUP_SET_WEIGHT": 12,
    "CGROUP_ADD_TASK": 13,
    "CPU_SET_FREQ": 14,
    "CPU_SET_CAPACITY": 15,
}
OP_TYPE_TO_NAME = {v: k for k, v in OP_NAME_TO_TYPE.items()}

@dataclass
class Op:
    name: str
    weight: int
    # function that returns True if the operation is applicable to the current state
    is_applicable: Callable
    # function that emits the operation; change the generator state accordingly; returns the operation arguments
    emit: Callable
    # list of resources required by the operation
    requires: List[str] = field(default_factory=list)
    # list of resources produced by the operation
    produces: List[str] = field(default_factory=list)
    # list of argument types
    arg_types: List[Optional[str]] = field(default_factory=lambda: [None, None, None])


# Arguments
ARG_TASK = "task"
ARG_CGROUP = "cgroup"
ARG_CPU = "cpu"
ARG_INT = "int"

# Resources
RESOURCE_TASK = "task"
RESOURCE_CGROUP = "cgroup"

# Generator parameters
MAX_TICK = 10

def op_task_create(m: GenState):
    tid = m.next_task_id()
    if tid is None:
        return None
    return (OP_NAME_TO_TYPE["TASK_CREATE"], tid, 0, 0)

# Fork and create new tasks;
# It will only be executed after the target task is actually running on a CPU;
def op_task_fork(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    new_tid = m.next_task_id()
    if new_tid is None:
        return None
    if tid in m.task2cgroups:
        m.cgroup_add_task(m.task2cgroups[tid], new_tid)
    return (OP_NAME_TO_TYPE["TASK_FORK"], tid, new_tid, 0)


def op_task_pin(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    rng = m.choose_cpuset_task(tid)
    if rng is None:
        return None
    begin, end = rng
    return (OP_NAME_TO_TYPE["TASK_PIN"], tid, begin, end)


def op_task_fifo(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    return (OP_NAME_TO_TYPE["TASK_FIFO"], tid, 0, 0)


def op_task_cfs(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    return (OP_NAME_TO_TYPE["TASK_CFS"], tid, 0, 0)


def op_task_pause(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    return (OP_NAME_TO_TYPE["TASK_PAUSE"], tid, 0, 0)


def op_task_wakeup(m: GenState):
    tid = m.choose_task_in_state(TASK_SLEEPING)
    return (OP_NAME_TO_TYPE["TASK_WAKEUP"], tid, 0, 0)


def op_task_set_prio(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    prio = m.rnd.randint(-20, 19)
    return (OP_NAME_TO_TYPE["TASK_SET_PRIO"], tid, prio, 0)


def op_tick(m: GenState):
    return (OP_NAME_TO_TYPE["TICK"], 0, 0, 0)


def op_tick_repeat(m: GenState):
    n = m.rnd.randint(1, MAX_TICK)
    return (OP_NAME_TO_TYPE["TICK_REPEAT"], n, 0, 0)


def op_cgroup_create(m: GenState):
    child_id = m.next_cgroup_id()
    parent_id = m.choose_parent_cgroup_id()
    if child_id is None or parent_id is None:
        return None
    m.add_cgroup(parent_id, child_id)
    return (OP_NAME_TO_TYPE["CGROUP_CREATE"], parent_id, child_id, 0)


def op_cgroup_set_cpuset(m: GenState):
    cgroup_id = m.choose_cgroup()
    begin, end = m.choose_cpuset_cgroup(cgroup_id)
    m.cgroups[cgroup_id].cpuset = (begin, end)
    return (OP_NAME_TO_TYPE["CGROUP_SET_CPUSET"], cgroup_id, begin, end)


def op_cgroup_set_weight(m: GenState):
    cgroup_id = m.choose_cgroup()
    weight = m.rnd.randint(1, 10000)
    return (OP_NAME_TO_TYPE["CGROUP_SET_WEIGHT"], cgroup_id, weight, 0)


def op_cgroup_add_task(m: GenState):
    cgroup_id = m.choose_leaf_cgroup()
    tid = m.choose_task()
    m.cgroup_add_task(cgroup_id, tid)
    return (OP_NAME_TO_TYPE["CGROUP_ADD_TASK"], cgroup_id, tid, 0)


def op_cpu_set_freq(m: GenState):
    cpu = m.choose_cpu()
    scale = m.rnd.randint(1, 1024)
    return (OP_NAME_TO_TYPE["CPU_SET_FREQ"], cpu, scale, 0)


def op_cpu_set_capacity(m: GenState):
    cpu = m.choose_cpu()
    scale = m.rnd.randint(1, 1024)
    return (OP_NAME_TO_TYPE["CPU_SET_CAPACITY"], cpu, scale, 0)


OPS: List[Op] = [
    Op(
        name="TASK_CREATE",
        weight=6,
        is_applicable=lambda m: m.next_task_id() is not None,
        emit=op_task_create,
        produces=[RESOURCE_TASK],
        arg_types=[ARG_TASK, None, None],
    ),
    Op(
        name="TASK_FORK",
        weight=3,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU) and m.next_task_id() is not None,
        emit=op_task_fork,
        requires=[RESOURCE_TASK],
        produces=[RESOURCE_TASK],
        arg_types=[ARG_TASK, ARG_INT, None],
    ),
    Op(
        name="TASK_PIN",
        weight=3,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU) and m.cpus >= 2,
        emit=op_task_pin,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, ARG_CPU, ARG_CPU],
    ),
    Op(
        name="TASK_FIFO",
        weight=1,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
        emit=op_task_fifo,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, ARG_INT, None],
    ),
    Op(
        name="TASK_CFS",
        weight=2,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
        emit=op_task_cfs,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, ARG_INT, None],
    ),
    Op(
        name="TASK_PAUSE",
        weight=4,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
        emit=op_task_pause,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, None, None],
    ),
    Op(
        name="TASK_WAKEUP",
        weight=10,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_SLEEPING),
        emit=op_task_wakeup,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, None, None],
    ),
    Op(
        name="TASK_SET_PRIO",
        weight=3,
        is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
        emit=op_task_set_prio,
        requires=[RESOURCE_TASK],
        arg_types=[ARG_TASK, ARG_INT, None],
    ),
    Op(
        name="TICK",
        weight=15,
        is_applicable=lambda m: True,
        emit=op_tick,
    ),
    Op(
        name="TICK_REPEAT",
        weight=0,
        is_applicable=lambda m: True,
        emit=op_tick_repeat,
        arg_types=[ARG_INT, None, None],
    ),
    Op(
        name="CGROUP_CREATE",
        weight=2,
        is_applicable=lambda m: m.next_cgroup_id() is not None,
        emit=op_cgroup_create,
        produces=[RESOURCE_CGROUP],
        arg_types=[ARG_CGROUP, ARG_CGROUP, None],
    ),
    Op(
        name="CGROUP_SET_CPUSET",
        weight=2,
        is_applicable=lambda m: m.has_cgroups() and m.cpus >= 2,
        emit=op_cgroup_set_cpuset,
        requires=[RESOURCE_CGROUP],
        arg_types=[ARG_CGROUP, ARG_CPU, ARG_CPU],
    ),
    Op(
        name="CGROUP_SET_WEIGHT",
        weight=2,
        is_applicable=lambda m: m.has_cgroups(),
        emit=op_cgroup_set_weight,
        requires=[RESOURCE_CGROUP],
        arg_types=[ARG_CGROUP, ARG_INT, None],
    ),
    Op(
        name="CGROUP_ADD_TASK",
        weight=3,
        is_applicable=lambda m: m.has_cgroups() and m.has_tasks(),
        emit=op_cgroup_add_task,
        requires=[RESOURCE_CGROUP, RESOURCE_TASK],
        arg_types=[ARG_CGROUP, ARG_TASK, None],
    ),
    Op(
        name="CPU_SET_FREQ",
        weight=0,
        is_applicable=lambda m: m.cpus >= 2,
        emit=op_cpu_set_freq,
        arg_types=[ARG_CPU, ARG_INT, None],
    ),
    Op(
        name="CPU_SET_CAPACITY",
        weight=0,
        is_applicable=lambda m: m.cpus >= 2,
        emit=op_cpu_set_capacity,
        arg_types=[ARG_CPU, ARG_INT, None],
    ),
]


def build_ops() -> List[Op]:
    return OPS
