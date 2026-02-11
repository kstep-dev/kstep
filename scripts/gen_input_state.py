import random
from dataclasses import dataclass, field
from typing import List, Optional

# Cgroup constants
CGROUP_ROOT = "cgroot"

# Task states
TASK_SLEEPING = "sleeping"
TASK_RUNNABLE = "runnable"

@dataclass
class Cgroup:
    id: int
    parent_id: int  # -1 means root
    cpuset: tuple[int, int]

# Generator state
@dataclass
class GenState:
    # Generator parameters
    max_tasks: int
    max_cgroups: int
    cpus: int
    rnd: random.Random

    # Generator state: changed by operations that produce resources or consume resources
    tasks: List[int] = field(default_factory=list) # list of task ids
    task_state: dict = field(default_factory=dict) # key: task id, value: state
    cgroups: dict = field(default_factory=dict) # key: cgroup id, value: Cgroup

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
        choices = [tid for tid in self.tasks if self.task_state.get(tid) == state]
        return self.rnd.choice(choices)

    def next_task_id(self) -> Optional[int]:
        for i in range(self.max_tasks):
            if i not in self.tasks:
                return i
        return None

    def add_task(self, tid: int):
        self.tasks.append(tid)
        self.task_state[tid] = TASK_SLEEPING

    def remove_task(self, tid: int):
        self.tasks.remove(tid)
        del self.task_state[tid]
    
    def set_task_state(self, tid: int, state: str):
        self.task_state[tid] = state

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
    
    def choose_cpuset_cgroup(self, cgroup_id: int) -> tuple[int, int]:
        parent_id = self.cgroups[cgroup_id].parent_id
        if parent_id == -1:
            parent_range = self.root_cpuset()
        else:
            parent_range = self.cgroups[parent_id].cpuset
        begin, end = self.choose_cpuset_subset(parent_range[0], parent_range[1])
        return begin, end

    def choose_cpu(self) -> int:
        return self.rnd.randint(1, self.cpus - 1)

    # ======
    # cgroup related functions
    def has_cgroups(self) -> bool:
        return len(self.cgroups) > 0

    def choose_cgroup(self) -> int:
        return self.rnd.choice(list(self.cgroups.keys()))

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
        if parent_id == -1:
            cpuset = self.root_cpuset()
        else:
            cpuset = self.cgroups[parent_id].cpuset
        self.cgroups[child_id] = Cgroup(
            id=child_id,
            parent_id=parent_id,
            cpuset=cpuset,
        )

    def cgroup_name(self, cgroup_id: int) -> str:
        parts = [f"cg{cgroup_id}"]
        cur = self.cgroups.get(cgroup_id)
        while cur and cur.parent_id != -1:
            parts.append(f"cg{cur.parent_id}")
            cur = self.cgroups.get(cur.parent_id)
        parts.append(CGROUP_ROOT)
        return "/".join(reversed(parts))

