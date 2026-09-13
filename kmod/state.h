// The machine's state as the cli driver shares it with the host through guest memory: a
// fixed-layout table the controller rewrites after every command, read by the page straight
// out of QEMU's RAM (website/site/kstep.mjs mirrors this layout; keep the two in step).
// gen is a seqlock: odd while an update is in progress, even and unchanged around a consistent read.
#pragma once

#include <linux/types.h>

#define KSTEP_STATE_CPUS 8   // isolated CPUs 1..8
#define KSTEP_STATE_TASKS 64

struct kstep_state_hdr {
  u32 gen;
  u32 timestamp; // logical ticks
  u32 ncpus, ntasks;
};

struct kstep_state_cpu {
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u64 nr_running, nr_switches, min_vruntime, util_avg, load_avg, runnable_avg;
};

enum kstep_state_task_state { KSTEP_TASK_RUNNING, KSTEP_TASK_RUNNABLE, KSTEP_TASK_SLEEPING, KSTEP_TASK_BLOCKED };
#define KSTEP_TASK_ELIGIBLE 1 // flags
#define KSTEP_TASK_DELAYED 2

struct kstep_state_task {
  u32 task, state, cpu, policy; // policy: the kernel's SCHED_* number
  s32 nice;
  u32 flags;
  u64 cpus; // allowed CPUs as a bitmask
  u64 weight, sum_exec_runtime, vruntime, deadline, slice;
  char cgroup[32];
};

struct kstep_state {
  struct kstep_state_hdr hdr;
  struct kstep_state_cpu cpu[KSTEP_STATE_CPUS];
  struct kstep_state_task task[KSTEP_STATE_TASKS];
};

static_assert(sizeof(struct kstep_state_hdr) == 16);
static_assert(sizeof(struct kstep_state_cpu) == 64);
static_assert(sizeof(struct kstep_state_task) == 104);
static_assert(sizeof(struct kstep_state) <= 2 * PAGE_SIZE);
