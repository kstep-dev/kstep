// The region of guest memory the host reads directly (shm.c writes it; crates/core/src/shm.rs
// decodes it, with these structs generated from this header): the machine's state, rewritten
// after every cli command. gen is a seqlock over the state: odd while an update is in progress,
// even and unchanged around a consistent read. The coverage map (cov.c) is a region of its own,
// so adding a field here never moves it; both addresses are reported on the cli's ready line. Trace events are not here: scheduler hooks write
// them as JSON records on the driver's channel (io.c).
#pragma once

#include <linux/types.h>

// As many as the module itself allows (KSTEP_NR_CPUS): the cap is the kmod's, not this region's.
// KSTEP_SHM_GROUPS must track it -- a domain can have one group per CPU -- and KSTEP_SHM_DOMAINS
// is one record per (CPU, level) over the five topology levels.
#define KSTEP_SHM_CPUS 32 // isolated CPUs 1..32
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_CGROUPS 16
#define KSTEP_SHM_ENTITIES (KSTEP_SHM_TASKS + KSTEP_SHM_CGROUPS * KSTEP_SHM_CPUS) // every task's, plus one group entity per (cgroup, CPU)
#define KSTEP_SHM_DOMAINS (KSTEP_SHM_CPUS * 5) // one per (CPU, level)
#define KSTEP_SHM_GROUPS KSTEP_SHM_CPUS // a domain's balancing groups, at most one per CPU
#define KSTEP_COV_SIZE (1 << 16) // cov.c's edge map, saturating byte counts; fuzzer/src/main.rs MAP_SIZE

// The header describes the rest of the region: one descriptor per table, so the host reads where
// each table starts, how wide its records are and how many are there instead of hardcoding any of
// it. Only the first three fields are a fixed contract: magic, then layout, then gen. A host
// checks magic and layout before trusting anything else. Bump KSTEP_SHM_LAYOUT whenever a
// record's fields change meaning without changing its size -- the strides catch everything that
// resizes, this catches the rest.
#define KSTEP_SHM_MAGIC 0x5054536b // "kSTP", little endian
#define KSTEP_SHM_LAYOUT 6

// The machine and the tasks first, then one pair of tables per scheduling class: the class's
// queue on each CPU, and what is on those queues. A task's record says which class it is under;
// what that class makes of it is in the class's own member table, joined by task number. Nothing
// about one class sits in the CPU or task record, so adding a class is adding a pair here.
enum kstep_shm_tables {
  KSTEP_TBL_CPU, KSTEP_TBL_TASK, KSTEP_TBL_CGROUP, KSTEP_TBL_DOMAIN,
  KSTEP_TBL_CFS, KSTEP_TBL_ENTITY, // the fair class: the root cfs_rq per CPU, and every entity queued under it
  KSTEP_TBL_RT, KSTEP_TBL_RT_ENTITY, // the real-time class: the rt_rq per CPU, and the tasks on it
  KSTEP_TBL_N
};

struct kstep_shm_table {
  u32 n, max;      // records written by the last update, and the table's capacity
  u32 off, stride; // where the table starts in the region, and a record's size
};

struct kstep_shm_hdr {
  u32 magic, layout;
  u32 gen;
  u32 timestamp; // logical ticks
  struct kstep_shm_table table[KSTEP_TBL_N];
  u32 max_groups, group_stride; // the one nested table: a domain record's balancing groups
  u32 reserved[2];
};

// The runqueue itself, and nothing any one class owns: what the CPU is doing, and when its
// balancer next runs. What each class queued there looks like is in that class's table.
struct kstep_shm_cpu {
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u32 freq; // arch_freq_scale: the current frequency, which cpufreq moves under a running system
  u32 next_balance_in; // ticks until rq->next_balance comes due, 0 when it already has
  u64 nr_running, nr_switches;
};

enum kstep_shm_task_state { KSTEP_TASK_RUNNING, KSTEP_TASK_RUNNABLE, KSTEP_TASK_SLEEPING, KSTEP_TASK_BLOCKED };
// se.flags. The page draws these; it does not work any of them out, so the kernel's own answer is
// what is shown: eligible is entity_eligible, pick is pick_eevdf on the entity's queue, curr is
// the queue's curr, which for a cgroup entity means a task under it is running.
#define KSTEP_SE_ELIGIBLE 1
#define KSTEP_SE_DELAYED 2
#define KSTEP_SE_ON_RQ 4
#define KSTEP_SE_CURR 8
#define KSTEP_SE_PICK 16

// A sched_entity as the fair class sees it, whether it belongs to a task or to a cgroup on one
// CPU: the queue picks between entities without caring which, so both records carry this block
// and the page decodes it once.
struct kstep_shm_se {
  u32 flags;
  u32 share; // of the CPU, in 1/1024: this weight over its queue's, times the parent entity's share, up to the root -- the walk update_cfs_rq_h_load makes over load averages, made over weights
  u64 weight; // scale_load_down: the units the nice table is written in, nice 0 being 1024
  u64 sum_exec_runtime, vruntime, deadline, slice;
  // The queue's average vruntime minus this entity's, weighted: the one number comparable across
  // queues, since every cfs_rq has a clock of its own. Zero is fair, positive is owed time, and
  // EEVDF's eligibility is lag >= 0. Live from avg_vruntime while queued; the kernel's saved
  // se->vlag, which place_entity will restore, while not.
  s64 lag;
};

// A task's identity and its scheduling attributes -- the sched_attr fields every class reads or
// ignores but none owns -- plus the one counter that follows a task across classes. Which class
// holds it is `policy`; that class's view of it is the record with this task number in the
// class's member table, and a task is in exactly one of those at a time.
struct kstep_shm_task {
  u32 task, state, cpu, policy; // policy: the kernel's SCHED_* number
  s32 nice;         // kept, though only the fair classes read it
  u32 rt_priority;  // 1..99 under fifo/rr, 0 otherwise: the kernel zeroes it on the way back
  u32 cgroup;       // index into the cgroup table, as an entity's
  u32 reserved;
  u64 cpus;             // allowed CPUs as a bitmask
  u64 sum_exec_runtime; // CPU time given so far, under whichever class held it at the time
};

// One entry per live cgroup, the root ("/") first: what the kernel holds, not what a driver
// last wrote, so a weight or cpuset changed behind a program's back still shows up. This is the
// cgroup's configuration; what the scheduler does with it is in the entity table below.
struct kstep_shm_cgroup {
  char path[40]; // as cgroup_path() reports it, e.g. "/a/b"
  u64 cpus;      // cpuset.cpus.effective as a bitmask
  u32 weight;    // cpu.weight, 0 where the cpu controller is not on
  u32 reserved;
};

// The fair class's root queue on one CPU: rq->cfs. The balancer reads this, not the runqueue:
// nr_running counts everything queued, h_nr_runnable only what the fair class will run, and a
// real-time task, or one left behind by delayed dequeue, is in one and not the other.
struct kstep_shm_cfs {
  u32 cpu, reserved;
  u64 min_vruntime, util_avg, load_avg, runnable_avg;
  u64 h_nr_runnable; // cfs_rq->h_nr_runnable: runnable tasks as the balancer counts them
};

// One sched_entity as the fair class sees it: a task's, or a cgroup's group entity on one CPU.
// With the cpu controller on, a cgroup owns a sched_entity and a cfs_rq per CPU, and it is those
// entities -- not the tasks -- that the parent queue picks between: four equal tasks split
// 1/2 : 1/6 : 1/6 : 1/6 when one sits alone in its cgroup, and nothing in the task table says
// why. The kernel keeps no per-cgroup total: each CPU's entity has its own weight (tg->shares
// divided by calc_group_shares), vruntime and runtime, so this is one record per (cgroup, CPU).
// The root task group owns no entity -- its cfs_rq is the runqueue's own -- so it has no rows.
// A task's record is here only while a fair policy holds it; its cgroup is the queue it is on.
struct kstep_shm_entity {
  u32 task;   // kSTEP task number, 0 for a cgroup's entity
  u32 cgroup; // index into the cgroup table
  u32 cpu, reserved;
  struct kstep_shm_se se;
};

// The real-time class's queue on one CPU: rq->rt. It picks the head of the highest non-empty
// priority list, so highest_prio is the whole decision, and bandwidth control can take the class
// off the CPU altogether: rt_time is what it has used of rt_runtime in the current period
// (sched_rt_period_us, 1 s), and throttled says the period's share is spent. Under
// CONFIG_RT_GROUP_SCHED these are the root task group's; the fields are zero without it.
struct kstep_shm_rt {
  u32 cpu, nr_running;
  u32 highest_prio; // rt_rq->highest_prio.curr: the kernel's prio, 0 (highest) .. 98; 100 when empty
  u32 throttled;
  u64 rt_time, rt_runtime; // ns
};

#define KSTEP_RT_ON_RQ 1 // rt.on_rq: enqueued
#define KSTEP_RT_CURR 2  // running there
#define KSTEP_RT_PICK 4  // the head of the highest list: what pick_next_task_rt would take

// A task on the real-time class, one record per task under fifo or rr. position is its place in
// its priority's list, 0 the head, so rows sort by priority then position. FIFO runs the head
// until it yields or blocks; RR moves the head to the tail when its timeslice runs out, and
// time_slice is what is left of it in ticks (RR_TIMESLICE is 100 ms).
struct kstep_shm_rt_entity {
  u32 task, cpu;
  u32 flags, position;
  u32 time_slice, reserved;
};

// One sched domain as the kernel built it, per CPU, innermost first -- not the topology the cli
// was asked for: Linux drops a level whose span adds nothing (sd_degenerate), so a level named in
// `cpu-topo` may simply not be here, which is the whole reason this is worth reporting. The
// balancing knobs are the ones that decide whether a given tick balances at all.
struct kstep_shm_group {
  u64 span;
  u32 capacity;                   // sgc->capacity: the group's total, after RT/DL pressure
  u32 min_capacity, max_capacity; // per-CPU extremes in the group; what misfit compares against
  u32 weight;                     // CPUs in the group
};

struct kstep_shm_domain {
  u32 cpu, ngroups;
  u64 span;
  char name[8];    // the topology level: SMT, CLS, MC, PKG, NODE
  char flags[160]; // SD_* names as the kernel has them, "SD_" stripped, e.g. "SHARE_LLC, PREFER_SIBLING"
  u32 imbalance_pct, balance_interval, busy_factor, cache_nice_tries;
  u32 nr_balance_failed; // per CPU: consecutive failures here, which escalate to active balancing
  u32 last_balance_ago;  // ticks since this CPU last balanced at this level
  // Not here on purpose: sd->shared's nr_busy_cpus and has_idle_cores, which select_idle_sibling
  // reads on a wakeup. Only the NOHZ path maintains nr_busy_cpus (set_cpu_sd_state_busy/idle) and
  // this kernel is built without CONFIG_NO_HZ, so it sits at the domain's weight forever; measured
  // has_idle_cores likewise never leaves 0. Both would read "nothing is idle" on an empty machine.
  struct kstep_shm_group group[KSTEP_SHM_GROUPS];
};

struct kstep_shm {
  struct kstep_shm_hdr hdr;
  struct kstep_shm_cpu cpu[KSTEP_SHM_CPUS];
  struct kstep_shm_task task[KSTEP_SHM_TASKS];
  struct kstep_shm_cgroup cgroup[KSTEP_SHM_CGROUPS];
  struct kstep_shm_domain domain[KSTEP_SHM_DOMAINS];
  struct kstep_shm_cfs cfs[KSTEP_SHM_CPUS];
  struct kstep_shm_entity entity[KSTEP_SHM_ENTITIES];
  struct kstep_shm_rt rt[KSTEP_SHM_CPUS];
  struct kstep_shm_rt_entity rt_entity[KSTEP_SHM_TASKS];
};
