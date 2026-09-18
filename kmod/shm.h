// The region of guest memory the host reads directly (shm.c writes it, website/site/kstep.mjs
// decodes it; keep the two in step): the machine's state, rewritten after every cli command. gen
// is a seqlock over the state: odd while an update is in progress,
// even and unchanged around a consistent read. The coverage map (cov.c) is a region of its own,
// so adding a field here never moves it; both addresses are reported on the cli's ready line. Trace events are not here: scheduler hooks write
// them as JSON records on the driver's channel (io.c).
#pragma once

#include <linux/stddef.h>
#include <linux/types.h>

// As many as the module itself allows (KSTEP_NR_CPUS): the cap is the kmod's, not this region's.
// KSTEP_SHM_GROUPS must track it -- a domain can have one group per CPU -- and KSTEP_SHM_DOMAINS
// is one record per (CPU, level) over the five topology levels.
#define KSTEP_SHM_CPUS 32 // isolated CPUs 1..32
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_CGROUPS 16
#define KSTEP_SHM_ENTITIES (KSTEP_SHM_CGROUPS * KSTEP_SHM_CPUS) // one group entity per (cgroup, CPU)
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
#define KSTEP_SHM_LAYOUT 5

enum kstep_shm_tables { KSTEP_TBL_CPU, KSTEP_TBL_TASK, KSTEP_TBL_CGROUP, KSTEP_TBL_ENTITY, KSTEP_TBL_DOMAIN, KSTEP_TBL_N };

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

// The first block is the runqueue's own state; the second is what the load balancer reads when it
// runs, which is not the same thing: nr_running counts everything queued, the balancer counts
// h_nr_runnable, and a task left behind by delayed dequeue is in one and not the other.
struct kstep_shm_cpu {
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u32 freq; // arch_freq_scale: the current frequency, which cpufreq moves under a running system
  u64 nr_running, nr_switches, min_vruntime, util_avg, load_avg, runnable_avg;
  u64 h_nr_runnable;   // cfs_rq->h_nr_runnable: runnable tasks as the balancer counts them
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

struct kstep_shm_task {
  u32 task, state, cpu, policy; // policy: the kernel's SCHED_* number
  s32 nice;         // kept, though only the fair classes read it
  u32 rt_priority;  // 1..99 under fifo/rr, 0 otherwise: the kernel zeroes it on the way back
  u32 cgroup;       // index into the cgroup table, as an entity's
  u32 reserved;
  u64 cpus; // allowed CPUs as a bitmask
  struct kstep_shm_se se;
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

// A cgroup's group entity on one CPU. With the cpu controller on, a cgroup owns a sched_entity
// and a cfs_rq per CPU, and it is those entities -- not the tasks -- that the parent queue picks
// between: four equal tasks split 1/2 : 1/6 : 1/6 : 1/6 when one sits alone in its cgroup, and
// nothing in the task table says why. The kernel keeps no per-cgroup total: each CPU's entity has
// its own weight (tg->shares divided by calc_group_shares), vruntime and runtime, so this is one
// record per (cgroup, CPU), the same fields a task record carries for its own entity. The root
// task group owns no entity -- its cfs_rq is the runqueue's own -- so it has no rows.
struct kstep_shm_entity {
  u32 cgroup; // index into the cgroup table
  u32 cpu;
  struct kstep_shm_se se;
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
  struct kstep_shm_entity entity[KSTEP_SHM_ENTITIES];
  struct kstep_shm_domain domain[KSTEP_SHM_DOMAINS];
};

static_assert(sizeof(struct kstep_shm_hdr) == 112);
static_assert(sizeof(struct kstep_shm_cpu) == 88);
static_assert(sizeof(struct kstep_shm_se) == 56);
static_assert(sizeof(struct kstep_shm_task) == 96);
static_assert(sizeof(struct kstep_shm_cgroup) == 56);
static_assert(sizeof(struct kstep_shm_entity) == 64);
static_assert(sizeof(struct kstep_shm_group) == 24);
static_assert(sizeof(struct kstep_shm_domain) == 208 + KSTEP_SHM_GROUPS * 24);
