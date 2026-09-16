// The region of guest memory the host reads directly (shm.c writes it, website/site/kstep.mjs
// decodes it; keep the two in step): the machine's state, rewritten after every cli command. gen
// is a seqlock over the state: odd while an update is in progress,
// even and unchanged around a consistent read. The coverage map (cov.c) is a region of its own,
// so adding a field here never moves it; both addresses are reported on the cli's ready line. Trace events are not here: scheduler hooks write
// them as JSON records on the driver's channel (io.c).
#pragma once

#include <linux/stddef.h>
#include <linux/types.h>

#define KSTEP_SHM_CPUS 8 // isolated CPUs 1..8
#define KSTEP_SHM_TASKS 64
#define KSTEP_SHM_CGROUPS 16
#define KSTEP_COV_SIZE (1 << 16) // cov.c's edge map, saturating byte counts; fuzzer/src/main.rs MAP_SIZE

struct kstep_shm_hdr {
  u32 gen;
  u32 timestamp; // logical ticks
  u32 ncpus, ntasks, ncgroups;
  u32 reserved[3];
};

struct kstep_shm_cpu {
  u32 cpu, curr; // curr: kSTEP task number running there, 0 for none or another process
  u32 idle, capacity;
  u32 freq; // arch_freq_scale: the current frequency, which cpufreq moves under a running system
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

// One entry per live cgroup, the root ("/") first: what the kernel holds, not what a driver
// last wrote, so a weight or cpuset changed behind a program's back still shows up.
struct kstep_shm_cgroup {
  char path[40]; // as cgroup_path() reports it, e.g. "/a/b"
  u64 cpus;      // cpuset.cpus.effective as a bitmask
  u32 weight;    // cpu.weight, 0 where the cpu controller is not on
  u32 reserved;
};

struct kstep_shm {
  struct kstep_shm_hdr hdr;
  struct kstep_shm_cpu cpu[KSTEP_SHM_CPUS];
  struct kstep_shm_task task[KSTEP_SHM_TASKS];
  struct kstep_shm_cgroup cgroup[KSTEP_SHM_CGROUPS];
};

static_assert(sizeof(struct kstep_shm_hdr) == 32);
static_assert(sizeof(struct kstep_shm_cpu) == 72);
static_assert(sizeof(struct kstep_shm_task) == 104);
static_assert(sizeof(struct kstep_shm_cgroup) == 56);
