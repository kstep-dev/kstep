// Interactive driver on ttyS1, kSTEP's structured channel: commands are read from it, one
// per line, and each is answered with one flat JSON object written to it like any trace
// event (load_balance, migrate), so the whole structured output is one ordered stream.
// A reply carries "timestamp" (logical ticks) and no "type" (trace events have one); a
// failed command's reply is {"timestamp":..,"error":"..."}. CPU topology and capacity come
// from the usual topology=/capacity= boot parameters.
//
// Tasks are named by their creation number 1, 2, .. (the same on every kernel and run, so a
// script or a trace means the same thing everywhere); a number is never reused after `kill`.
//
//   create                    new runnable CFS task on CPUs 1..N-1     -> {"timestamp":T,"task":N}
//   tick                      advance one tick                         -> {"timestamp":T}
//   top                       one {"type":"cpu",...} record per isolated CPU and one
//                             {"type":"task",...} record per live task, then the reply
//                             {"timestamp":T,"tasks":N}. A record:
//         {"timestamp":T,"type":"task","task":N,"state":"running|runnable|sleeping|blocked","cpu":C,
//          "cpus":"1-2","cgroup":"/a","policy":"normal","nice":0,"weight":..,"sum_exec_runtime":..,"vruntime":..,...}
//   task <n>                  `top` for one task: its record, then {"timestamp":T,"tasks":1}
//   nice <n> <-20..19>        set the nice value (fair classes; kept across a spell as fifo/rr)
//   policy <n> <normal|batch|idle|fifo|rr>   set the scheduling class (real-time at one fixed priority)
//   affinity <n> <cpulist>    set the CPUs the task may run on, e.g. 1-2,4
//   pause <n>                 put the task to sleep (it does when it next runs)
//   wake <n>                  wake a paused task
//   wait <n>                  semaphore wait: the task sleeps until a post (or `wake`)
//   post <n>                  semaphore post: the task wakes one waiter with a sync (WF_SYNC)
//                             wakeup from its own CPU (a pipe underneath)
//   kill <n>                  ask the task to exit (it does when it next runs)
//   cgroup-create /a          create cgroup /a (its parent must exist; / is the root)
//   cgroup-weight /a <w>      the cgroup's cpu.weight, 1..10000 (default 100)
//   cgroup-cpus /a <cpulist>  the cgroup's cpuset.cpus
//   attach <n> /a             attach the task to a cgroup (`attach <n> /` back to the root)
//   exit                      end the session (the VM reboots)
//
// A task that has exited answers "no such task" and has no `top` record. The migrate event
// names the task by its number too (migrations of other processes are not reported).
#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#define LINE_MAX 128

static struct file *cmd; // /dev/ttyS1 for reading; replies go through the trace writer

static void reply(void) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_end(&json);
}

static void reply_error(const char *msg) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "error", msg);
  kstep_json_end(&json);
}

static int last_cpu(void) { return num_online_cpus() - 1; }

// The session's tasks by creation number: tasks[n - 1]. Each holds a reference, so the
// pointer stays valid after the task exits (exit_state says it did).
#define MAX_TASKS 64
static struct task_struct *tasks[MAX_TASKS];
static int ntasks;

static int task_number(struct task_struct *p) {
  for (int i = 0; i < ntasks; i++)
    if (tasks[i] == p)
      return i + 1;
  return 0;
}

// Take the leading task number off *arg (the rest stays in *arg) and look the task up; on
// failure reply with an error and return NULL.
static struct task_struct *parse_task(char **arg, const char *usage) {
  char *num_s = *arg ? strsep(arg, " \t") : NULL;
  int n;

  if (*arg)
    *arg = strim(*arg);
  if (!num_s || kstrtoint(num_s, 10, &n) || n <= 0) {
    reply_error(usage);
    return NULL;
  }
  if (n > ntasks || tasks[n - 1]->exit_state) {
    reply_error("no such task");
    return NULL;
  }
  return tasks[n - 1];
}

static void cmd_create(char *arg) {
  struct kstep_json json;
  struct task_struct *p;

  if (ntasks == MAX_TASKS)
    return reply_error("too many tasks");
  p = kstep_task_create();
  tasks[ntasks++] = get_task_struct(p);
  kstep_task_pin(p, 1, last_cpu()); // keep off CPU 0, kSTEP's control CPU
  kstep_task_wakeup(p);
  kstep_json_begin(&json);
  kstep_json_field_s64(&json, "task", ntasks);
  kstep_json_end(&json);
}

static void cmd_tick(char *arg) {
  kstep_tick();
  reply();
}

static const struct {
  const char *name;
  int policy;
} policies[] = {
    {"normal", SCHED_NORMAL}, {"batch", SCHED_BATCH}, {"idle", SCHED_IDLE},
    {"fifo", SCHED_FIFO},     {"rr", SCHED_RR},
};

static const char *policy_name(int policy) {
  for (int i = 0; i < ARRAY_SIZE(policies); i++)
    if (policies[i].policy == policy)
      return policies[i].name;
  return "?";
}

// One task's scheduler state as a "task" record.
static void write_task(struct task_struct *p) {
  struct kstep_json json;
  unsigned int state = READ_ONCE(p->__state);

  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "task");
  kstep_json_field_s64(&json, "task", task_number(p));
  kstep_json_field_str(&json, "state", p->on_cpu ? "running" : state == TASK_RUNNING ? "runnable" :
                                        state & TASK_INTERRUPTIBLE ? "sleeping" : "blocked");
  kstep_json_field_s64(&json, "cpu", task_cpu(p));
  kstep_json_field_fmt(&json, "cpus", "\"%*pbl\"", cpumask_pr_args(p->cpus_ptr));
  {
    char cgroup[64];
    rcu_read_lock();
    if (cgroup_path(task_dfl_cgroup(p), cgroup, sizeof(cgroup)) < 0)
      strscpy(cgroup, "?", sizeof(cgroup));
    rcu_read_unlock();
    kstep_json_field_str(&json, "cgroup", cgroup);
  }
  kstep_json_field_str(&json, "policy", policy_name(p->policy));
  kstep_json_field_s64(&json, "nice", task_nice(p));
  kstep_json_field_u64(&json, "weight", p->se.load.weight);
  kstep_json_field_u64(&json, "sum_exec_runtime", p->se.sum_exec_runtime);
  kstep_json_field_u64(&json, "vruntime", p->se.vruntime);
  // EEVDF: eligible = vruntime <= the queue's weighted average; delayed = dequeued while
  // ineligible and kept on the queue until eligible (sched_delayed)
  kstep_json_field_bool(&json, "eligible", p->se.on_rq && kstep_eligible(&p->se));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
  kstep_json_field_bool(&json, "delayed", p->se.sched_delayed);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  kstep_json_field_u64(&json, "deadline", p->se.deadline);
  kstep_json_field_u64(&json, "slice", p->se.slice);
#endif
  kstep_json_end(&json);
}

static void reply_tasks(int n) {
  struct kstep_json json;

  kstep_json_begin(&json);
  kstep_json_field_s64(&json, "tasks", n);
  kstep_json_end(&json);
}

static void cmd_task(char *arg) {
  struct task_struct *p = parse_task(&arg, "usage: task <n>");

  if (!p)
    return;
  write_task(p);
  reply_tasks(1);
}

static void cmd_top(char *arg) {
  int n = 0;

  // Isolated CPUs are held between scheduler events while commands run. Read their
  // queues directly: counting task records would miss delayed dequeue accounting.
  for (int cpu = 1; cpu <= last_cpu(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    struct kstep_json json;

    kstep_json_begin(&json);
    kstep_json_field_str(&json, "type", "cpu");
    kstep_json_field_u64(&json, "cpu", cpu);
    kstep_json_field_u64(&json, "current", task_number(rq->curr));
    kstep_json_field_bool(&json, "idle", rq->curr == rq->idle);
    kstep_json_field_u64(&json, "nr_running", rq->nr_running);
    kstep_json_field_u64(&json, "capacity", arch_scale_cpu_capacity(cpu));
    kstep_json_field_u64(&json, "nr_switches", rq->nr_switches);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
    kstep_json_field_u64(&json, "min_vruntime", rq->cfs.min_vruntime);
#endif
#ifdef CONFIG_SMP
    kstep_json_field_u64(&json, "cfs_util_avg", rq->cfs.avg.util_avg);
    kstep_json_field_u64(&json, "cfs_load_avg", rq->cfs.avg.load_avg);
    kstep_json_field_u64(&json, "cfs_runnable_avg", rq->cfs.avg.runnable_avg);
#endif
    kstep_json_end(&json);
  }
  for (int i = 0; i < ntasks; i++)
    if (!tasks[i]->exit_state) {
      write_task(tasks[i]);
      n++;
    }
  reply_tasks(n);
}

static void cmd_affinity(char *arg) {
  const char *usage = "usage: affinity <n> <cpulist within 1..N-1>";
  struct task_struct *p = parse_task(&arg, usage);
  struct cpumask mask;

  if (!p)
    return;
  if (!arg || cpulist_parse(arg, &mask) || cpumask_empty(&mask) ||
      cpumask_test_cpu(0, &mask) || cpumask_last(&mask) > last_cpu())
    return reply_error(usage);
  kstep_task_set_affinity(p, &mask);
  reply();
}

static void cmd_nice(char *arg) {
  const char *usage = "usage: nice <n> <-20..19>";
  struct task_struct *p = parse_task(&arg, usage);
  int n;

  if (!p)
    return;
  if (!arg || kstrtoint(arg, 10, &n) || n < MIN_NICE || n > MAX_NICE)
    return reply_error(usage);
  kstep_task_set_nice(p, n);
  reply();
}

static void cmd_policy(char *arg) {
  const char *usage = "usage: policy <n> <normal|batch|idle|fifo|rr>";
  struct task_struct *p = parse_task(&arg, usage);

  if (!p)
    return;
  for (int i = 0; i < ARRAY_SIZE(policies); i++)
    if (arg && !strcmp(arg, policies[i].name)) {
      kstep_task_set_policy(p, policies[i].policy);
      return reply();
    }
  reply_error(usage);
}

// cgroups are named by their path under the cgroup root, "/" being the root itself; the
// kstep_cgroup_* helpers take the path without the leading slash. Take the leading path
// off *arg; it must exist unless `create`.
static const char *parse_cgroup(char **arg, const char *usage, bool create) {
  char *path = *arg ? strsep(arg, " \t") : NULL;

  if (*arg)
    *arg = strim(*arg);
  if (!path || path[0] != '/' || strstr(path, "..") || strlen(path) >= 48) {
    reply_error(usage);
    return NULL;
  }
  if (create ? (!path[1] || kstep_cgroup_exists(path + 1)) : (path[1] && !kstep_cgroup_exists(path + 1))) {
    reply_error(create ? "cgroup exists" : "no such cgroup");
    return NULL;
  }
  return path + 1;
}

static void cmd_cgroup_create(char *arg) {
  const char *name = parse_cgroup(&arg, "usage: cgroup-create /path", true);

  if (!name)
    return;
  kstep_cgroup_create(name);
  reply();
}

static void cmd_cgroup_weight(char *arg) {
  const char *usage = "usage: cgroup-weight /path <1..10000>";
  const char *name = parse_cgroup(&arg, usage, false);
  int weight;

  if (!name)
    return;
  if (!arg || kstrtoint(arg, 10, &weight) || weight < 1 || weight > 10000)
    return reply_error(usage);
  kstep_cgroup_set_weight(name, weight);
  reply();
}

static void cmd_cgroup_cpus(char *arg) {
  const char *usage = "usage: cgroup-cpus /path <cpulist within 1..N-1>";
  const char *name = parse_cgroup(&arg, usage, false);
  struct cpumask mask;

  if (!name)
    return;
  if (!arg || cpulist_parse(arg, &mask) || cpumask_empty(&mask) ||
      cpumask_test_cpu(0, &mask) || cpumask_last(&mask) > last_cpu())
    return reply_error(usage);
  kstep_cgroup_set_cpuset(name, arg);
  reply();
}

static void cmd_attach(char *arg) {
  const char *usage = "usage: attach <n> /path";
  struct task_struct *p = parse_task(&arg, usage);
  const char *name;

  if (!p || !(name = parse_cgroup(&arg, usage, false)))
    return;
  kstep_cgroup_move_task(name, p->pid);
  reply();
}

static const struct {
  const char *verb;
  void (*fn)(char *arg);
} commands[] = {
    {"create", cmd_create}, {"tick", cmd_tick},         {"task", cmd_task},         {"top", cmd_top},
    {"nice", cmd_nice},     {"policy", cmd_policy},     {"affinity", cmd_affinity}, {"attach", cmd_attach},
    {"cgroup-create", cmd_cgroup_create}, {"cgroup-weight", cmd_cgroup_weight},
    {"cgroup-cpus", cmd_cgroup_cpus},
};

// Verbs that just signal a task: the SIGUSR1 handler in user/user.c does the actual
// pause(), wakeup, wait, post or exit when the task next runs.
static const struct {
  const char *verb;
  void (*fn)(struct task_struct *p);
} signals[] = {
    {"pause", kstep_task_pause}, {"wake", kstep_task_wakeup}, {"wait", kstep_task_wait},
    {"post", kstep_task_post},   {"kill", kstep_task_exit},
};

/* Returns false when the session should end. */
static bool execute(char *line) {
  char *verb, *arg;

  line = strim(line);
  if (!*line || *line == '#')
    return true;
  verb = strsep(&line, " \t");
  arg = line ? strim(line) : NULL;
  if (arg && !*arg)
    arg = NULL;

  for (int i = 0; i < ARRAY_SIZE(commands); i++)
    if (!strcmp(verb, commands[i].verb)) {
      commands[i].fn(arg);
      return true;
    }
  for (int i = 0; i < ARRAY_SIZE(signals); i++)
    if (!strcmp(verb, signals[i].verb)) {
      char usage[32];
      struct task_struct *p;

      snprintf(usage, sizeof(usage), "usage: %s <n>", verb);
      p = parse_task(&arg, usage);
      if (p) {
        signals[i].fn(p);
        reply();
      }
      return true;
    }
  if (!strcmp(verb, "exit")) {
    reply();
    return false;
  }
  reply_error("unknown command");
  return true;
}

// The migrate event with the task named by its number; other processes are not reported.
static void output_migrate(struct task_struct *p, int src_cpu, int dst_cpu) {
  struct kstep_json json;
  int n = task_number(p);

  if (!n)
    return;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "migrate");
  kstep_json_field_s64(&json, "task", n);
  kstep_json_field_s64(&json, "src_cpu", src_cpu);
  kstep_json_field_s64(&json, "dst_cpu", dst_cpu);
  kstep_json_end(&json);
}

static void setup(void) {
  cmd = filp_open("/dev/ttyS1", O_RDONLY | O_NOCTTY, 0);
  if (IS_ERR(cmd))
    panic("Failed to open /dev/ttyS1: %ld", PTR_ERR(cmd));
}

static void run(void) {
  struct kstep_json json;
  char line[LINE_MAX], c;
  size_t len = 0;
  loff_t pos = 0;

  kstep_json_begin(&json);
  kstep_json_field_bool(&json, "ready", true);
  kstep_json_end(&json);
  while (true) {
    if (kernel_read(cmd, &c, 1, &pos) != 1)
      continue;
    if (c == '\n') {
      line[len] = '\0';
      len = 0;
      if (!execute(line))
        break;
    } else if (len + 1 < LINE_MAX && isprint(c)) {
      line[len++] = c;
    }
  }
  filp_close(cmd, NULL);
}

KSTEP_DRIVER_DEFINE{
    .name = "cli",
    .setup = setup,
    .run = run,
    // events in the stream: load_balance = a CPU's balancer looked for work (after
    // should_we_balance); migrate = a task actually moved (balancing or wakeup placement)
    .on_sched_balance_selected = kstep_output_balance,
    .on_task_migrate = output_migrate,
    .step_interval_us = 1000, // no on_tick_begin: clients ask with `top`
};
