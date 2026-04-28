from typing import Callable, List, Optional
from dataclasses import dataclass, field

from .gen_input_state import (
    KTHREAD_BLOCK_REQUESTED,
    GenState,
    KTHREAD_BLOCKED,
    KTHREAD_CREATED,
    KTHREAD_DEAD,
    KTHREAD_SPIN,
    KTHREAD_SYNCWAKE_REQUESTED,
    KTHREAD_YIELD,
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
    "CGROUP_DESTROY": 16,
    "CGROUP_MOVE_TASK_ROOT": 17,
    "KTHREAD_CREATE": 18,
    "KTHREAD_BIND": 19,
    "KTHREAD_START": 20,
    "KTHREAD_YIELD": 21,
    "KTHREAD_BLOCK": 22,
    "KTHREAD_SYNCWAKE": 23,
    "TASK_FREEZE": 24,
}
OP_TYPE_TO_NAME = {v: k for k, v in OP_NAME_TO_TYPE.items()}

OpWeight = int | Callable[[GenState], int]

@dataclass
class Op:
    name: str
    weight: OpWeight
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
    # function that applies GenState side-effects for a replayed op (args already known, no randomness);
    # None means no side-effects beyond what update_from_kmod handles
    replay: Optional[Callable] = None

    def resolved_weight(self, m: GenState) -> int:
        if callable(self.weight):
            return self.weight(m)
        return self.weight


# Arguments
ARG_TASK = "task"
ARG_KTHREAD = "kthread"
ARG_CGROUP = "cgroup"
ARG_CPU = "cpu"
ARG_INT = "int"

# Resources
RESOURCE_TASK = "task"
RESOURCE_KTHREAD = "kthread"
RESOURCE_CGROUP = "cgroup"

# Generator parameters
# reproducing util_avg
# MIN_TICK = 50
# MAX_TICK = 150
# reproducing vruntime_overflow
MIN_TICK = 1
MAX_TICK = 20


def disable_on_small_topology(weight: int) -> OpWeight:
    return lambda m: 0 if m.cpus <= 2 else weight


def enable_on_cross_scheduler(weight: int) -> OpWeight:
    return lambda m: weight if m.cross_scheduler else 0


def enable_kthread_ops(weight: int) -> OpWeight:
    return lambda m: weight if m.enable_kthreads else 0


def enable_task_freeze_ops(weight: int) -> OpWeight:
    return lambda m: weight if m.enable_task_freeze else 0


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
    m.cgroup_remove_task(tid)
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


def op_task_freeze(m: GenState):
    tid = m.choose_task_in_state(TASK_SLEEPING)
    return (OP_NAME_TO_TYPE["TASK_FREEZE"], tid, 0, 0)


def op_task_set_prio(m: GenState):
    tid = m.choose_task_in_state(TASK_ON_CPU)
    prio = m.rnd.randint(-20, 19)
    return (OP_NAME_TO_TYPE["TASK_SET_PRIO"], tid, prio, 0)


def op_kthread_create(m: GenState):
    ktid = m.next_kthread_id()
    if ktid is None:
        return None
    return (OP_NAME_TO_TYPE["KTHREAD_CREATE"], ktid, 0, 0)


def op_kthread_bind(m: GenState):
    ktid = m.choose_kthread_in_states(
        (
            KTHREAD_CREATED,
            KTHREAD_YIELD,
            KTHREAD_BLOCKED,
        )
    )
    rng = m.choose_cpuset()
    if rng is None:
        return None
    begin, end = rng
    return (OP_NAME_TO_TYPE["KTHREAD_BIND"], ktid, begin, end)


def op_kthread_start(m: GenState):
    ktid = m.choose_kthread_in_state(KTHREAD_CREATED)
    return (OP_NAME_TO_TYPE["KTHREAD_START"], ktid, 0, 0)


def op_kthread_yield(m: GenState):
    ktid = m.choose_kthread_in_states((KTHREAD_SPIN, KTHREAD_YIELD))
    return (OP_NAME_TO_TYPE["KTHREAD_YIELD"], ktid, 0, 0)


def op_kthread_block(m: GenState):
    ktid = m.choose_kthread_in_states((KTHREAD_SPIN, KTHREAD_YIELD))
    return (OP_NAME_TO_TYPE["KTHREAD_BLOCK"], ktid, 0, 0)


def op_kthread_syncwake(m: GenState):
    wakee = m.choose_kthread_in_state(KTHREAD_BLOCKED)
    wakers = [
        ktid for ktid in m.kthreads
        if ktid != wakee and m.kthread_state.get(ktid) in (KTHREAD_SPIN, KTHREAD_YIELD)
    ]
    waker = m.rnd.choice(wakers)
    return (OP_NAME_TO_TYPE["KTHREAD_SYNCWAKE"], waker, wakee, 0)


def op_tick(m: GenState):
    return (OP_NAME_TO_TYPE["TICK"], 0, 0, 0)


def op_tick_repeat(m: GenState):
    n = m.rnd.randint(MIN_TICK, MAX_TICK)
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
    m.set_cgroup_cpuset(cgroup_id, (begin, end))
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


def op_cgroup_destroy(m: GenState):
    cgroup_id = m.choose_destroyable_leaf_cgroup()
    m.remove_cgroup(cgroup_id)
    return (OP_NAME_TO_TYPE["CGROUP_DESTROY"], cgroup_id, 0, 0)


def op_cgroup_move_task_root(m: GenState):
    cgroup_id, tid = m.choose_task_in_cgroup()
    m.cgroup_remove_task(tid)
    return (OP_NAME_TO_TYPE["CGROUP_MOVE_TASK_ROOT"], cgroup_id, tid, 0)


def op_cpu_set_freq(m: GenState):
    cpu = m.choose_cpu()
    scale = m.rnd.randint(1, 1024)
    return (OP_NAME_TO_TYPE["CPU_SET_FREQ"], cpu, scale, 0)


def op_cpu_set_capacity(m: GenState):
    cpu = m.choose_cpu()
    scale = m.rnd.randint(1, 1024)
    return (OP_NAME_TO_TYPE["CPU_SET_CAPACITY"], cpu, scale, 0)


# Replay functions: apply GenState side-effects from a replayed op's known args (a, b, c).
# Only ops whose emit() mutates state beyond task_state (which update_from_kmod handles) need one.

def replay_task_fork(m: GenState, a: int, b: int, c: int):
    # a=parent_tid, b=new_tid; propagate cgroup membership
    if a in m.task2cgroups:
        m.cgroup_add_task(m.task2cgroups[a], b)

def replay_task_fifo(m: GenState, a: int, b: int, c: int):
    # a=tid; FIFO transitions move the task back to the root cgroup
    m.cgroup_remove_task(a)

def replay_cgroup_create(m: GenState, a: int, b: int, c: int):
    # a=parent_id, b=child_id
    if b not in m.cgroups:
        m.add_cgroup(a, b)

def replay_cgroup_set_cpuset(m: GenState, a: int, b: int, c: int):
    # a=cgroup_id, b=begin, c=end
    if a in m.cgroups:
        m.set_cgroup_cpuset(a, (b, c))

def replay_cgroup_add_task(m: GenState, a: int, b: int, c: int):
    # a=cgroup_id, b=tid
    m.cgroup_add_task(a, b)

def replay_cgroup_destroy(m: GenState, a: int, b: int, c: int):
    # a=cgroup_id
    m.remove_cgroup(a)

def replay_cgroup_move_task_root(m: GenState, a: int, b: int, c: int):
    # a=cgroup_id, b=tid
    m.cgroup_remove_task(b)


def has_bindable_kthreads(m: GenState) -> bool:
    return any(
        m.kthread_state.get(ktid) in {
            KTHREAD_CREATED,
            KTHREAD_YIELD,
            KTHREAD_BLOCK_REQUESTED,
            KTHREAD_BLOCKED,
            KTHREAD_SYNCWAKE_REQUESTED,
        }
        for ktid in m.kthreads)


def has_syncwake_pair(m: GenState) -> bool:
    wakees = [ktid for ktid in m.kthreads if m.kthread_state.get(ktid) == KTHREAD_BLOCKED]
    wakers = [
        ktid for ktid in m.kthreads
        if m.kthread_state.get(ktid) in (KTHREAD_SPIN, KTHREAD_YIELD)
    ]
    return any(waker != wakee for waker in wakers for wakee in wakees)


def build_ops(weight_overrides: Optional[dict[str, OpWeight]] = None) -> List[Op]:
    weights: dict[str, OpWeight] = {
        "TASK_CREATE": 6,
        "TASK_FORK": 3,
        "TASK_PIN": disable_on_small_topology(3),
        "TASK_FIFO": enable_on_cross_scheduler(2),
        "TASK_CFS": enable_on_cross_scheduler(2),
        "TASK_PAUSE": 4,
        "TASK_WAKEUP": 10,
        "TASK_FREEZE": enable_task_freeze_ops(2),
        "TASK_SET_PRIO": 3,
        "TICK": 0,
        "TICK_REPEAT": 5,
        "CGROUP_CREATE": 4,
        "CGROUP_SET_CPUSET": disable_on_small_topology(1),
        "CGROUP_SET_WEIGHT": 2,
        "CGROUP_ADD_TASK": 3,
        "CGROUP_DESTROY": 1,
        "CGROUP_MOVE_TASK_ROOT": 2,
        "CPU_SET_FREQ": 0,
        "CPU_SET_CAPACITY": 0,
        "KTHREAD_CREATE": enable_kthread_ops(2),
        "KTHREAD_BIND": enable_kthread_ops(2),
        "KTHREAD_START": enable_kthread_ops(2),
        "KTHREAD_YIELD": enable_kthread_ops(0),
        "KTHREAD_BLOCK": enable_kthread_ops(2),
        "KTHREAD_SYNCWAKE": enable_kthread_ops(2),
    }
    if weight_overrides:
        weights.update(weight_overrides)
    return [
        Op(
            name="TASK_CREATE",
            weight=weights["TASK_CREATE"],
            is_applicable=lambda m: m.next_task_id() is not None,
            emit=op_task_create,
            produces=[RESOURCE_TASK],
            arg_types=[ARG_TASK, None, None],
        ),
        Op(
            name="TASK_FORK",
            weight=weights["TASK_FORK"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU) and m.next_task_id() is not None,
            emit=op_task_fork,
            requires=[RESOURCE_TASK],
            produces=[RESOURCE_TASK],
            arg_types=[ARG_TASK, ARG_INT, None],
            replay=replay_task_fork,
        ),
        Op(
            name="TASK_PIN",
            weight=weights["TASK_PIN"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU) and m.cpus >= 2,
            emit=op_task_pin,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, ARG_CPU, ARG_CPU],
        ),
        Op(
            name="TASK_FIFO",
            weight=weights["TASK_FIFO"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
            emit=op_task_fifo,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, ARG_INT, None],
            replay=replay_task_fifo,
        ),
        Op(
            name="TASK_CFS",
            weight=weights["TASK_CFS"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
            emit=op_task_cfs,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, ARG_INT, None],
        ),
        Op(
            name="TASK_PAUSE",
            weight=weights["TASK_PAUSE"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
            emit=op_task_pause,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, None, None],
        ),
        Op(
            name="TASK_WAKEUP",
            weight=weights["TASK_WAKEUP"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_SLEEPING),
            emit=op_task_wakeup,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, None, None],
        ),
        Op(
            name="TASK_FREEZE",
            weight=weights["TASK_FREEZE"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_SLEEPING),
            emit=op_task_freeze,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, None, None],
        ),
        Op(
            name="TASK_SET_PRIO",
            weight=weights["TASK_SET_PRIO"],
            is_applicable=lambda m: m.has_tasks_in_state(TASK_ON_CPU),
            emit=op_task_set_prio,
            requires=[RESOURCE_TASK],
            arg_types=[ARG_TASK, ARG_INT, None],
        ),
        Op(
            name="KTHREAD_CREATE",
            weight=weights["KTHREAD_CREATE"],
            is_applicable=lambda m: m.next_kthread_id() is not None,
            emit=op_kthread_create,
            produces=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, None, None],
        ),
        Op(
            name="KTHREAD_BIND",
            weight=weights["KTHREAD_BIND"],
            is_applicable=lambda m: has_bindable_kthreads(m) and m.cpus >= 2,
            emit=op_kthread_bind,
            requires=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, ARG_CPU, ARG_CPU],
        ),
        Op(
            name="KTHREAD_START",
            weight=weights["KTHREAD_START"],
            is_applicable=lambda m: m.has_kthreads_in_state(KTHREAD_CREATED),
            emit=op_kthread_start,
            requires=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, None, None],
        ),
        Op(
            name="KTHREAD_YIELD",
            weight=weights["KTHREAD_YIELD"],
            is_applicable=lambda m: m.has_kthreads_in_states((KTHREAD_SPIN, KTHREAD_YIELD)),
            emit=op_kthread_yield,
            requires=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, None, None],
        ),
        Op(
            name="KTHREAD_BLOCK",
            weight=weights["KTHREAD_BLOCK"],
            is_applicable=lambda m: m.has_kthreads_in_states((KTHREAD_SPIN, KTHREAD_YIELD)),
            emit=op_kthread_block,
            requires=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, None, None],
        ),
        Op(
            name="KTHREAD_SYNCWAKE",
            weight=weights["KTHREAD_SYNCWAKE"],
            is_applicable=has_syncwake_pair,
            emit=op_kthread_syncwake,
            requires=[RESOURCE_KTHREAD],
            arg_types=[ARG_KTHREAD, ARG_KTHREAD, None],
        ),
        Op(
            name="TICK",
            weight=weights["TICK"],
            is_applicable=lambda m: True,
            emit=op_tick,
        ),
        Op(
            name="TICK_REPEAT",
            weight=weights["TICK_REPEAT"],
            is_applicable=lambda m: True,
            emit=op_tick_repeat,
            arg_types=[ARG_INT, None, None],
        ),
        Op(
            name="CGROUP_CREATE",
            weight=weights["CGROUP_CREATE"],
            is_applicable=lambda m: m.next_cgroup_id() is not None,
            emit=op_cgroup_create,
            produces=[RESOURCE_CGROUP],
            arg_types=[ARG_CGROUP, ARG_CGROUP, None],
            replay=replay_cgroup_create,
        ),
        Op(
            name="CGROUP_SET_CPUSET",
            weight=weights["CGROUP_SET_CPUSET"],
            is_applicable=lambda m: m.has_cgroups() and m.cpus >= 2,
            emit=op_cgroup_set_cpuset,
            requires=[RESOURCE_CGROUP],
            arg_types=[ARG_CGROUP, ARG_CPU, ARG_CPU],
            replay=replay_cgroup_set_cpuset,
        ),
        Op(
            name="CGROUP_SET_WEIGHT",
            weight=weights["CGROUP_SET_WEIGHT"],
            is_applicable=lambda m: m.has_cgroups(),
            emit=op_cgroup_set_weight,
            requires=[RESOURCE_CGROUP],
            arg_types=[ARG_CGROUP, ARG_INT, None],
        ),
        Op(
            name="CGROUP_ADD_TASK",
            weight=weights["CGROUP_ADD_TASK"],
            is_applicable=lambda m: m.has_cgroups() and m.has_tasks(),
            emit=op_cgroup_add_task,
            requires=[RESOURCE_CGROUP, RESOURCE_TASK],
            arg_types=[ARG_CGROUP, ARG_TASK, None],
            replay=replay_cgroup_add_task,
        ),
        Op(
            name="CGROUP_DESTROY",
            weight=weights["CGROUP_DESTROY"],
            is_applicable=lambda m: bool(m.destroyable_leaf_cgroups()),
            emit=op_cgroup_destroy,
            requires=[RESOURCE_CGROUP],
            arg_types=[ARG_CGROUP, None, None],
            replay=replay_cgroup_destroy,
        ),
        Op(
            name="CGROUP_MOVE_TASK_ROOT",
            weight=weights["CGROUP_MOVE_TASK_ROOT"],
            is_applicable=lambda m: m.has_tasks_in_cgroups(),
            emit=op_cgroup_move_task_root,
            requires=[RESOURCE_CGROUP, RESOURCE_TASK],
            arg_types=[ARG_CGROUP, ARG_TASK, None],
            replay=replay_cgroup_move_task_root,
        ),
        Op(
            name="CPU_SET_FREQ",
            weight=weights["CPU_SET_FREQ"],
            is_applicable=lambda m: m.cpus >= 2,
            emit=op_cpu_set_freq,
            arg_types=[ARG_CPU, ARG_INT, None],
        ),
        Op(
            name="CPU_SET_CAPACITY",
            weight=weights["CPU_SET_CAPACITY"],
            is_applicable=lambda m: m.cpus >= 2,
            emit=op_cpu_set_capacity,
            arg_types=[ARG_CPU, ARG_INT, None],
        ),
    ]
