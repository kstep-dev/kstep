#ifndef KSTEP_DRIVER_H
#define KSTEP_DRIVER_H

#include <linux/sched.h>

#include "event.h"

#define TRACE_INFO(fmt, ...) pr_info("\033[92m" fmt "\033[0m\n", ##__VA_ARGS__)
#define DRIVER_NAME_LEN 32

// A driver is the program kstep runs: it sets up the session and steps it. That is all it is --
// everything that happens during the session, down to the tick, is an event (event.h) a driver
// registers for in its setup() like anyone else. So a driver that watches something carries no more
// than one that does not, and the checkers and the shared-memory log watch the same events without
// the driver knowing.
struct kstep_driver {
  char name[DRIVER_NAME_LEN];
  void (*setup)(void); // build the session: the machine, the tasks, and what this driver watches
  void (*run)(void);   // step it
  u64 tick_interval_ns; // Virtual clock advance per tick in ns
};
#define KSTEP_DRIVER_DEFINE static struct kstep_driver DRIVER __used =

// The session's tasks, for anything that judges them as a set (the cli driver owns them)
struct task_struct **kstep_session_tasks(int *ntasks);

// checkers/: enabling a rule is the rule registering for the events it watches, so this is the
// whole interface -- the cli's `check` verb and nothing else.
int kstep_check_enable(const char *name); // 0, or -ENOENT for an unknown name

// io.c
struct kstep_json {
  size_t len;
  char buf[512 - sizeof(size_t)];
};
void kstep_json_begin(struct kstep_json *json);
void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *val_fmt, ...);
void kstep_json_field_str(struct kstep_json *json, const char *key,
                          const char *val);
void kstep_json_field_u64(struct kstep_json *json, const char *key, u64 val);
void kstep_json_field_s64(struct kstep_json *json, const char *key, s64 val);
void kstep_json_field_bool(struct kstep_json *json, const char *key, bool val);
void kstep_json_end(struct kstep_json *json);
void kstep_json_print_2kv(const char *key1, const char *val1, const char *key2,
                          const char *val2_fmt, ...);
#define kstep_pass(msg_fmt, ...)                                               \
  kstep_json_print_2kv("status", "pass", "message", "\"" msg_fmt "\"",         \
                       ##__VA_ARGS__)
#define kstep_fail(msg_fmt, ...)                                               \
  kstep_json_print_2kv("status", "fail", "message", "\"" msg_fmt "\"",         \
                       ##__VA_ARGS__)
void kstep_print_sched_debug(void);
void kstep_output_curr_task(void);
void kstep_output_nr_running(void);
void kstep_output_balance(int cpu, struct sched_domain *sd);
void kstep_output_migrate(u32 task, int src_cpu, int dst_cpu);

// tick.c
void kstep_tick(void);
void kstep_tick_repeat(int n);
void *kstep_tick_until(void *(*fn)(void));
void kstep_settle(void);

// task.c
struct task_struct *kstep_task_create(void);
void kstep_task_exit(struct task_struct *p);
int kstep_task_set_affinity(struct task_struct *p, const char *cpulist); // "1", "1-3", "1,3" within 1..N-1; -EINVAL if outside its cpuset
void kstep_task_set_fair(struct task_struct *p, int policy, int nice); // normal/batch/idle with its nice
void kstep_task_set_rt(struct task_struct *p, int policy, int prio);   // fifo/rr with its priority, 1..99
void kstep_task_pause(struct task_struct *p);
void kstep_task_yield(struct task_struct *p); // sched_yield(): gives the CPU up while staying runnable
void kstep_task_wakeup(struct task_struct *p);
void kstep_task_block(struct task_struct *p);
void kstep_task_chan_read(struct task_struct *p);
void kstep_task_chan_write(struct task_struct *p);

// kernel.c
int kstep_write(const char *path, const char *buf, size_t size);
int kstep_read(const char *path, char *buf, size_t size);
int kstep_mkdir(const char *dir);
void kstep_sysctl_write(const char *name, const char *fmt, ...);
void kstep_sched_feat_write(const char *fmt, ...);
void kstep_sched_feat_enable(const char *name);
void kstep_sched_feat_disable(const char *name);
int kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                       ...);
int kstep_cgroup_read(const char *name, const char *filename, char *buf, size_t size);
bool kstep_cgroup_exists(const char *name);
int kstep_cgroup_create(const char *name);
void kstep_cgroup_destroy(const char *name);
int kstep_cgroup_set_cpuset(const char *name, const char *cpuset);
int kstep_cgroup_set_weight(const char *name, int weight);
int kstep_cgroup_move_task(const char *name, int pid);
bool kstep_task_is_frozen(struct task_struct *p);
void kstep_freeze_task(struct task_struct *p);
void kstep_thaw_task(struct task_struct *p);
int kstep_eligible(struct sched_entity *se);
void kstep_check_extra_balance(int cpu, struct sched_domain *sd);

// cpu.c
#define CPU_SPEC_LEN 512
// The topology spec, parsed and applied in one go, with the sched domains rebuilt as needed:
//   KEY=group|group;KEY=group|group...   a group is a cpulist; CAP groups are cpulist:scale
// A key is a level (SMT, CLS, MC, PKG, NODE) or CAP, the capacity of the CPUs named in 1..1024.
//   e.g. "CLS=1-2|3-4;CAP=2,4:512"       two clusters, a little core in each
// The grammar and its rules are spelled out at the top of cpu.c. Frequency is not in the spec:
// it changes under a running system, so it is a per-CPU setter.
const char *kstep_topo_set(const char *spec); // NULL, or what is wrong with it
void kstep_freq_set(int cpu, int scale); // 1..1024
unsigned long kstep_freq_get(int cpu);
void kstep_topo_print(void);
void kstep_sd_flags_str(int flags, char *buf, size_t len);

#endif
