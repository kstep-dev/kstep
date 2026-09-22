// The region of guest memory the host reads directly: the machine's state, rewritten by shm.c
// after every cli command and decoded by crates/core/src/shm.rs with structs generated from this
// header. There is one writer, the controller, and every reader waits for the command's reply
// before copying the region, so no lock or generation count guards it. The coverage map (cov.c) is
// a region of its own, so adding a field here never moves it; both addresses are reported on the
// cli's ready line. Trace events are not here: scheduler hooks write them as JSON records on the
// driver's channel (io.c).
#pragma once

#include <linux/types.h>

// Bump whenever a struct below changes, in size or in meaning: a decoder built from another
// revision of this header refuses the region instead of misreading it.
#define KSTEP_SHM_LAYOUT 14

// hdr.features: what this kernel's scheduler has, so a reader shows what exists rather than
// knowing kernel versions. EEVDF (6.6+): the fair entities' lag, deadline, eligibility and pick.
#define KSTEP_SHM_EEVDF (1u << 0)

// Table capacities. CPUs: as many as the module allows (KSTEP_NR_CPUS). Domains: one record per
// (test CPU, topology level) over the five levels; their balancing groups are one flat table,
// each domain naming its run of it -- a real hierarchy has a few hundred at most, and a domain
// whose groups do not fit reports the ones that did. Entities: every task's plus one group entity
// per (cgroup, CPU).
#define KSTEP_SHM_CPUS 32
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_CGROUPS 16
#define KSTEP_SHM_ENTITIES (KSTEP_SHM_TASKS + KSTEP_SHM_CGROUPS * KSTEP_SHM_CPUS)
#define KSTEP_SHM_DOMAINS (KSTEP_SHM_CPUS * 5)
#define KSTEP_SHM_GROUPS 1024
#define KSTEP_SHM_SD_FLAGS 32   // sched-domain flag bits this kernel can have (sd_flags.h has ~14)
#define KSTEP_SHM_SD_FLAG_LEN 24
#define KSTEP_COV_SIZE (1 << 16) // cov.c's edge map, saturating byte counts; crates/core/src/shm.rs COV_SIZE

// The machine and the tasks first, then one pair of tables per scheduling class: the class's
// queue on each CPU (ncpus records, like the CPU table), and what is on those queues. A task's
// record says which class it is under; what that class makes of it is in the class's own member
// table, joined by task number. Nothing about one class sits in the CPU or task record, so adding
// a class is adding a pair here. Struct padding is the compiler's, mirrored by the generated
// structs, so no field is spent on it.
struct kstep_shm_hdr {
  u32 layout, features;
  u32 timestamp; // logical ticks
  u32 ncpus, ntasks, ncgroups, ndomains, ngroups;
  u32 nentities;    // the fair class: entities queued under the root cfs_rq of any CPU
  u32 nrt_entities; // the real-time class: tasks on any rt_rq
};

// The runqueue itself, and nothing any one class owns: what the CPU is doing, and when its
// balancer next runs. What each class queued there looks like is in that class's table.
struct kstep_shm_cpu {
  u64 nr_running, nr_switches;
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u32 freq; // arch_freq_scale: the current frequency, which cpufreq moves under a running system
  u32 next_balance_in; // ticks until rq->next_balance comes due, 0 when it already has
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
  u64 weight; // scale_load_down: the units the nice table is written in, nice 0 being 1024
  u64 sum_exec_runtime;
  // Signed: the kernel keeps vruntimes unsigned and compares them as (s64)(a - b), so a woken task
  // placed before the queue's average sits just below 2^64 -- a small negative number, shown as one
  s64 vruntime, deadline;
  u64 slice;
  // The queue's average vruntime minus this entity's, weighted: the one number comparable across
  // queues, since every cfs_rq has a clock of its own. Zero is fair, positive is owed time, and
  // EEVDF's eligibility is lag >= 0. Live from avg_vruntime while queued; the kernel's saved
  // se->vlag, which place_entity will restore, while not.
  s64 lag;
  u32 flags;
};

// A task's identity and its scheduling attributes -- the sched_attr fields every class reads or
// ignores but none owns -- plus the one counter that follows a task across classes. Which class
// holds it is `policy`; that class's view of it is the record with this task number in the
// class's member table, and a task is in exactly one of those at a time.
struct kstep_shm_task {
  u64 cpus;             // allowed CPUs as a bitmask
  u64 sum_exec_runtime; // CPU time given so far, under whichever class held it at the time
  u32 task, state, cpu, policy; // policy: the kernel's SCHED_* number
  s32 nice;         // kept, though only the fair classes read it
  u32 rt_priority;  // 1..99 under fifo/rr, 0 otherwise: the kernel zeroes it on the way back
  u32 cgroup;       // index into the cgroup table, as an entity's
};

// One entry per live cgroup, the root ("/") first: what the kernel holds, not what a driver
// last wrote, so a weight or cpuset changed behind a program's back still shows up. This is the
// cgroup's configuration; what the scheduler does with it is in the entity table below.
struct kstep_shm_cgroup {
  u64 cpus;      // cpuset.cpus.effective as a bitmask
  u32 weight;    // cpu.weight, 0 where the cpu controller is not on
  char path[40]; // as cgroup_path() reports it, e.g. "/a/b"
};

// The fair class's root queue on one CPU: rq->cfs. The balancer reads this, not the runqueue:
// nr_running counts everything queued, h_nr_runnable only what the fair class will run, and a
// real-time task, or one left behind by delayed dequeue, is in one and not the other.
struct kstep_shm_cfs {
  u64 util_avg, load_avg, runnable_avg;
  u64 h_nr_runnable; // cfs_rq->h_nr_runnable: runnable tasks as the balancer counts them
  u32 cpu;
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
  struct kstep_shm_se se;
  u32 task;   // kSTEP task number, 0 for a cgroup's entity
  u32 cgroup; // index into the cgroup table
  u32 cpu;
};

// The real-time class's queue on one CPU: rq->rt. It picks the head of the highest non-empty
// priority list, so highest_prio is the whole decision, and bandwidth control can take the class
// off the CPU altogether: rt_time is what it has used of rt_runtime in the current period
// (sched_rt_period_us, 1 s), and throttled says the period's share is spent. Under
// CONFIG_RT_GROUP_SCHED these are the root task group's; the fields are zero without it.
struct kstep_shm_rt {
  u64 rt_time, rt_runtime; // ns
  u32 cpu, nr_running;
  u32 highest_prio; // rt_rq->highest_prio.curr: the kernel's prio, 0 (highest) .. 98; 100 when empty
  u32 throttled;
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
  u32 time_slice;
};

// One sched domain as the kernel built it, per CPU, innermost first -- not the topology the cli
// was asked for: Linux drops a level whose span adds nothing (sd_degenerate), so a level named in
// `cpu-topo` may simply not be here, which is the whole reason this is worth reporting. The
// balancing knobs are the ones that decide whether a given tick balances at all. flags is sd->flags
// as the kernel numbers its bits; the names, in that bit order, are in kstep_shm.sd_flag, written
// at init from the kernel's own sd_flags.h, so a reader never carries a table that can drift from
// the kernel in front of it (SD_SHARE_PKG_RESOURCES became SD_SHARE_LLC, and such renames are
// routine). Not here on purpose: sd->shared's nr_busy_cpus and has_idle_cores, which
// select_idle_sibling reads on a wakeup. Only the NOHZ path maintains nr_busy_cpus and this kernel
// is built without CONFIG_NO_HZ, so it would read "nothing is idle" on an empty machine.
struct kstep_shm_domain {
  u64 span, flags;
  u32 cpu;
  u32 group, ngroups; // this domain's balancing groups: the group table from `group`, `ngroups` of them, the CPU's own group first as sd->groups has it
  u32 imbalance_pct, balance_interval, busy_factor, cache_nice_tries;
  u32 nr_balance_failed; // per CPU: consecutive failures here, which escalate to active balancing
  // When and by whom this CPU's group is next balanced at this level: ticks until last_balance plus
  // the interval (busy_factor times longer while the CPU is busy, get_sd_balance_interval) falls
  // due, and the CPU that will run it -- should_we_balance's pick for the group: its first idle CPU,
  // on an idle core where the level is above SMT, else the group's first CPU.
  u32 next_balance_in, balancer;
  char name[8];          // the topology level: SMT, CLS, MC, PKG, NODE
};

struct kstep_shm_group {
  u64 span;
  u32 capacity;                   // sgc->capacity: the group's total, after RT/DL pressure
  u32 min_capacity, max_capacity; // per-CPU extremes in the group; what misfit compares against
  u32 weight;                     // CPUs in the group
};

struct kstep_shm {
  struct kstep_shm_hdr hdr;
  char sd_flag[KSTEP_SHM_SD_FLAGS][KSTEP_SHM_SD_FLAG_LEN]; // SD_* names by bit, "SD_" stripped; "" past the last
  struct kstep_shm_cpu cpu[KSTEP_SHM_CPUS];
  struct kstep_shm_task task[KSTEP_SHM_TASKS];
  struct kstep_shm_cgroup cgroup[KSTEP_SHM_CGROUPS];
  struct kstep_shm_domain domain[KSTEP_SHM_DOMAINS];
  struct kstep_shm_group group[KSTEP_SHM_GROUPS];
  struct kstep_shm_cfs cfs[KSTEP_SHM_CPUS];
  struct kstep_shm_entity entity[KSTEP_SHM_ENTITIES];
  struct kstep_shm_rt rt[KSTEP_SHM_CPUS];
  struct kstep_shm_rt_entity rt_entity[KSTEP_SHM_TASKS];
};
