// The region of guest memory the host reads directly (shm.c writes it, website/site/kstep.mjs
// decodes it; keep the two in step): the machine's state, rewritten after every cli command, and
// the coverage map (cov.c). gen is a seqlock over the state: odd while an update is in progress,
// even and unchanged around a consistent read. Trace events are not here: scheduler hooks write
// them as JSON records on the driver's channel (io.c).
#pragma once

#include <linux/stddef.h>
#include <linux/types.h>

#define KSTEP_SHM_CPUS 8 // isolated CPUs 1..8
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_COV (1 << 16) // edge map, saturating byte counts

struct kstep_shm_hdr {
  u32 gen;
  u32 timestamp; // logical ticks
  u32 ncpus, ntasks;
  u32 reserved[4];
};

struct kstep_shm_cpu {
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u64 nr_running, nr_switches, min_vruntime, util_avg, load_avg, runnable_avg;
};

enum kstep_shm_task_state { KSTEP_TASK_RUNNING, KSTEP_TASK_RUNNABLE, KSTEP_TASK_SLEEPING, KSTEP_TASK_BLOCKED };
#define KSTEP_TASK_ELIGIBLE 1 // flags
#define KSTEP_TASK_DELAYED 2

struct kstep_shm_task {
  u32 task, state, cpu, policy; // policy: the kernel's SCHED_* number
  s32 nice;
  u32 flags;
  u64 cpus; // allowed CPUs as a bitmask
  u64 weight, sum_exec_runtime, vruntime, deadline, slice;
  char cgroup[32];
};

struct kstep_shm {
  struct kstep_shm_hdr hdr;
  struct kstep_shm_cpu cpu[KSTEP_SHM_CPUS];
  struct kstep_shm_task task[KSTEP_SHM_TASKS];
  u8 cov[KSTEP_SHM_COV];
};

static_assert(sizeof(struct kstep_shm_hdr) == 32);
static_assert(sizeof(struct kstep_shm_cpu) == 64);
static_assert(sizeof(struct kstep_shm_task) == 104);
static_assert(offsetof(struct kstep_shm, cov) == 7200); // fuzzer/src/main.rs reads it at this offset
