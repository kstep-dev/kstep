import random
from dataclasses import dataclass, field
from typing import List, Optional

# Cgroup constants
CGROUP_ROOT = "cgroot"

# Task states (matching kmod: 0=blocked, 1=runnable-on-runqueue, 2=running-on-CPU)
TASK_SLEEPING = "sleeping"   # blocked/dequeued; eligible for WAKEUP
TASK_RUNNABLE = "runnable"   # on runqueue but not on CPU
TASK_ON_CPU   = "on_cpu"     # currently executing; eligible for signal ops

# Kthread states exported by kmod/kthread.c
KTHREAD_CREATED = "created"
KTHREAD_SPIN = "spin"
KTHREAD_YIELD = "yield"
KTHREAD_BLOCK_REQUESTED = "block_requested"
KTHREAD_BLOCKED = "blocked"
KTHREAD_SYNCWAKE_REQUESTED = "syncwake_requested"
KTHREAD_DEAD = "dead"

@dataclass
class Cgroup:
    id: int
    parent_id: int  # -1 means root
    cpuset: tuple[int, int]
    effective_cpuset: tuple[int, int]

# Generator state
@dataclass
class GenState:
    # Generator parameters
    max_tasks: int
    max_kthreads: int
    max_cgroups: int
    cpus: int
    rnd: random.Random
    cross_scheduler: bool = False # whether testing across rt and cfs schedulers
    enable_kthreads: bool = False
    enable_task_freeze: bool = False

    # Generator state: changed by operations that produce resources or consume resources
    tasks: List[int] = field(default_factory=list) # list of task ids
    task_state: dict[int, str] = field(default_factory=dict) # key: task id, value: state
    kthreads: List[int] = field(default_factory=list) # list of kthread ids
    kthread_state: dict[int, str] = field(default_factory=dict) # key: kthread id, value: state
    cgroups: dict = field(default_factory=dict) # key: cgroup id, value: Cgroup
    leaf_cgroups: list[int] = field(default_factory=list) # list of leaf cgroup ids
    task2cgroups: dict = field(default_factory=dict) # key: task id, value: cgroup id

    # ======
    # Task related functions
    def has_tasks(self) -> bool:
        return len(self.tasks) > 0

    def has_tasks_in_state(self, state: str) -> bool:
        for tid in self.tasks:
            if self.task_state.get(tid) == state:
                return True
        return False

    def choose_task(self) -> int:
        return self.rnd.choice(self.tasks)

    def choose_task_in_state(self, state: str) -> int:
        choices = [tid for tid in self.tasks if self.task_state[tid] == state]
        return self.rnd.choice(choices)

    def next_task_id(self) -> Optional[int]:
        for i in range(self.max_tasks):
            if i not in self.tasks:
                return i
        return None

    _KMOD_STATE_MAP = {0: TASK_SLEEPING, 1: TASK_RUNNABLE, 2: TASK_ON_CPU}

    # ======
    # Kthread related functions
    def has_kthreads(self) -> bool:
        return len(self.kthreads) > 0

    def has_kthreads_in_state(self, state: str) -> bool:
        return any(self.kthread_state.get(ktid) == state for ktid in self.kthreads)

    def has_kthreads_in_states(self, states: tuple[str, ...]) -> bool:
        return any(self.kthread_state.get(ktid) in states for ktid in self.kthreads)

    def choose_kthread(self) -> int:
        return self.rnd.choice(self.kthreads)

    def choose_kthread_in_state(self, state: str) -> int:
        choices = [ktid for ktid in self.kthreads if self.kthread_state.get(ktid) == state]
        return self.rnd.choice(choices)

    def choose_kthread_in_states(self, states: tuple[str, ...]) -> int:
        choices = [ktid for ktid in self.kthreads if self.kthread_state.get(ktid) in states]
        return self.rnd.choice(choices)

    def next_kthread_id(self) -> Optional[int]:
        for i in range(self.max_kthreads):
            if i not in self.kthreads:
                return i
        return None

    _KMOD_KTHREAD_STATE_MAP = {
        0: KTHREAD_CREATED,
        1: KTHREAD_SPIN,
        2: KTHREAD_YIELD,
        3: KTHREAD_BLOCK_REQUESTED,
        4: KTHREAD_BLOCKED,
        5: KTHREAD_SYNCWAKE_REQUESTED,
        6: KTHREAD_DEAD,
    }

    def update_from_kmod(self, task_states: list[dict], kthread_states: list[dict] | None = None):
        """Sync task list and states from the kmod's STATE response.
        task_states is a list of {"id": int, "state": int} dicts where
        state is 0=blocked, 1=runnable, 2=on_cpu."""
        kmod = {d["id"]: self._KMOD_STATE_MAP[d["state"]] for d in task_states}
        for tid, py_state in kmod.items():
            if tid not in self.tasks:
                self.tasks.append(tid)
            self.task_state[tid] = py_state
        for tid in list(self.tasks):
            if tid not in kmod:
                self.tasks.remove(tid)
                self.task_state.pop(tid, None)

        if kthread_states is None:
            return

        kmod_kthreads = {
            d["id"]: self._KMOD_KTHREAD_STATE_MAP[d["state"]]
            for d in kthread_states
        }
        for ktid, py_state in kmod_kthreads.items():
            if ktid not in self.kthreads:
                self.kthreads.append(ktid)
            self.kthread_state[ktid] = py_state
        for ktid in list(self.kthreads):
            if ktid not in kmod_kthreads:
                self.kthreads.remove(ktid)
                self.kthread_state.pop(ktid, None)

    # ======
    # cpuset related functions
    def root_cpuset(self) -> tuple[int, int]:
        if self.cpus >= 2:
            return (1, self.cpus - 1)
        return (0, 0)

    def choose_cpuset(self):
        if self.cpus < 2:
            return None
        begin = self.rnd.randint(1, self.cpus - 1)
        end = self.rnd.randint(begin, self.cpus - 1)
        return begin, end

    def choose_cpuset_subset(self, begin: int, end: int) -> tuple[int, int]:
        if begin > end:
            return begin, end
        new_begin = self.rnd.randint(begin, end)
        new_end = self.rnd.randint(new_begin, end)
        return new_begin, new_end

    def _parent_effective_cpuset(self, parent_id: int) -> tuple[int, int]:
        if parent_id == -1:
            return self.root_cpuset()
        return self.cgroups[parent_id].effective_cpuset

    def _compute_effective_cpuset(self, cpuset: tuple[int, int],
                                  parent_effective: tuple[int, int]) -> tuple[int, int]:
        if cpuset[0] > cpuset[1]:
            return parent_effective
        begin = max(cpuset[0], parent_effective[0])
        end = min(cpuset[1], parent_effective[1])
        if begin > end:
            return parent_effective
        return begin, end

    def _refresh_effective_cpuset(self, cgroup_id: int):
        cgroup = self.cgroups[cgroup_id]
        parent_effective = self._parent_effective_cpuset(cgroup.parent_id)
        cgroup.effective_cpuset = self._compute_effective_cpuset(
            cgroup.cpuset, parent_effective)
        for child_id, child in self.cgroups.items():
            if child.parent_id == cgroup_id:
                self._refresh_effective_cpuset(child_id)
    
    def choose_cpuset_cgroup(self, cgroup_id: int) -> tuple[int, int]:
        begin, end = self.choose_cpuset_subset(1, self.cpus - 1)
        return begin, end

    def choose_cpuset_task(self, task_id: int):
        if task_id not in self.task2cgroups:
            return self.choose_cpuset()
        cgroup_id = self.task2cgroups[task_id]
        effective = self.cgroups[cgroup_id].effective_cpuset
        return self.choose_cpuset_subset(effective[0], effective[1])

    def choose_cpu(self) -> int:
        return self.rnd.randint(1, self.cpus - 1)

    # ======
    # cgroup related functions
    def has_cgroups(self) -> bool:
        return len(self.cgroups) > 0

    def has_tasks_in_cgroups(self) -> bool:
        return bool(self.task2cgroups)

    def choose_cgroup(self) -> int:
        return self.rnd.choice(list(self.cgroups.keys()))

    def choose_task_in_cgroup(self) -> tuple[int, int]:
        task_id = self.rnd.choice(list(self.task2cgroups.keys()))
        return self.task2cgroups[task_id], task_id
    
    def choose_leaf_cgroup(self) -> int:
        if not self.leaf_cgroups:
            return self.choose_cgroup()
        return self.rnd.choice(self.leaf_cgroups)

    def destroyable_leaf_cgroups(self) -> list[int]:
        return list(self.leaf_cgroups)

    def choose_destroyable_leaf_cgroup(self) -> int:
        return self.rnd.choice(self.destroyable_leaf_cgroups())

    def next_cgroup_id(self) -> Optional[int]:
        for i in range(self.max_cgroups):
            if i not in self.cgroups:
                return i
        return None

    def choose_parent_cgroup_id(self) -> int:
        choices = [-1]
        choices.extend(self.cgroups.keys())
        return self.rnd.choice(choices)

    def add_cgroup(self, parent_id: int, child_id: int):
        parent_effective = self._parent_effective_cpuset(parent_id)
        default_cpuset = self.root_cpuset()
        self.cgroups[child_id] = Cgroup(
            id=child_id,
            parent_id=parent_id,
            cpuset=default_cpuset,
            effective_cpuset=self._compute_effective_cpuset(default_cpuset, parent_effective),
        )
        if child_id not in self.leaf_cgroups:
            self.leaf_cgroups.append(child_id)
        if parent_id != -1 and parent_id in self.leaf_cgroups:
            self.leaf_cgroups.remove(parent_id)
        if parent_id != -1:
            for task_id, cgroup_id in list(self.task2cgroups.items()):
                if cgroup_id == parent_id:
                    self.cgroup_remove_task(task_id)

    def set_cgroup_cpuset(self, cgroup_id: int, cpuset: tuple[int, int]):
        self.cgroups[cgroup_id].cpuset = cpuset
        self._refresh_effective_cpuset(cgroup_id)

    def cgroup_name(self, cgroup_id: int) -> str:
        parts = [f"cg{cgroup_id}"]
        cur = self.cgroups.get(cgroup_id)
        while cur and cur.parent_id != -1:
            parts.append(f"cg{cur.parent_id}")
            cur = self.cgroups.get(cur.parent_id)
        parts.append(CGROUP_ROOT)
        return "/".join(reversed(parts))

    def cgroup_add_task(self, cgroup_id: int, task_id: int):
        self.task2cgroups[task_id] = cgroup_id

    def cgroup_remove_task(self, task_id: int):
        self.task2cgroups.pop(task_id, None)

    def remove_cgroup(self, cgroup_id: int):
        cgroup = self.cgroups.pop(cgroup_id, None)
        if cgroup is None:
            return

        self.leaf_cgroups.remove(cgroup_id)

        for task_id, task_cgroup_id in list(self.task2cgroups.items()):
            if task_cgroup_id == cgroup_id:
                self.cgroup_remove_task(task_id)

        parent_id = cgroup.parent_id
        if parent_id == -1:
            return

        if all(child.parent_id != parent_id for child in self.cgroups.values()) and parent_id not in self.leaf_cgroups:
            self.leaf_cgroups.append(parent_id)
