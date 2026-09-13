// The region of guest memory the host reads directly (shm.c writes it, website/site/kstep.mjs
// decodes it; keep the two in step): the machine's state, rewritten after every cli command, and a
// ring of trace events appended by scheduler hooks. gen is a seqlock over the state: odd while an
// update is in progress, even and unchanged around a consistent read. nevents counts events ever
// appended; the ring holds the last KSTEP_SHM_EVENTS of them.
#pragma once

#include <linux/types.h>

#define KSTEP_SHM_CPUS 8 // isolated CPUs 1..8
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_EVENTS 256

struct kstep_shm_hdr {
  u32 gen;
  u32 timestamp; // logical ticks
  u32 ncpus, ntasks;
  u32 nevents;
  u32 reserved[3];
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

enum kstep_shm_event_type { KSTEP_EVENT_BALANCE, KSTEP_EVENT_MIGRATE };

struct kstep_shm_event {
  u32 timestamp, type;
  u32 task, src_cpu, dst_cpu; // task: 0 for a balance
  char name[12];              // balance: the sched domain's name
};

struct kstep_shm {
  struct kstep_shm_hdr hdr;
  struct kstep_shm_cpu cpu[KSTEP_SHM_CPUS];
  struct kstep_shm_task task[KSTEP_SHM_TASKS];
  struct kstep_shm_event event[KSTEP_SHM_EVENTS];
};

static_assert(sizeof(struct kstep_shm_hdr) == 32);
static_assert(sizeof(struct kstep_shm_cpu) == 64);
static_assert(sizeof(struct kstep_shm_task) == 104);
static_assert(sizeof(struct kstep_shm_event) == 32);
