// Interactive driver on ttyS1, kSTEP's structured channel: commands are read from it, one
// per line, and each is answered with one flat JSON object written to it like any trace
// event (load_balance, migrate), so the whole structured output is one ordered stream.
// A reply carries "timestamp" (logical ticks) and no "type" (trace events have one); a
// failed command's reply is {"timestamp":..,"error":"..."}. CPU topology and capacity come
// from the usual topology=/capacity= boot parameters.
//
//   create                    new runnable CFS task on CPUs 1..N-1     -> {"timestamp":T,"pid":N}
//   tick                      advance one tick; report the pid running on each test CPU
//                                                          -> {"timestamp":T,"cpu1":N,"cpu2":N,...}  (0 = idle)
//   task <pid>                scheduler counters of one task           -> {"timestamp":T,"pid":N,"state":..,"cpus":"1-2","vruntime":..,...}
//   nice <pid> <-20..19>      set the nice value
//   affinity <pid> <cpulist>  set the CPUs the task may run on, e.g. 1-2,4
//   pause <pid>               put the task to sleep (it does when it next runs)
//   wake <pid>                wake a paused task
//   wait <pid>                semaphore wait: the task sleeps until a post (or `wake`)
//   post <pid>                semaphore post: the task wakes one waiter with a sync (WF_SYNC)
//                             wakeup from its own CPU (a pipe underneath)
//   kill <pid>                ask the task to exit (it does when it next runs)
//   exit                      end the session (the VM reboots)
//
// A <pid> that no longer exists answers "no such task", which is how a client learns
// about exits.
#include <linux/cpumask.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/pid.h>
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

// Take the leading <pid> off *arg (the rest stays in *arg) and look the task up; on
// failure reply with an error and return NULL.
static struct task_struct *parse_task(char **arg, const char *usage) {
  char *pid_s = *arg ? strsep(arg, " \t") : NULL;
  struct task_struct *p = NULL;
  int pid;

  if (*arg)
    *arg = strim(*arg);
  if (!pid_s || kstrtoint(pid_s, 10, &pid) || pid <= 0) {
    reply_error(usage);
    return NULL;
  }
  rcu_read_lock();
  p = pid_task(find_vpid(pid), PIDTYPE_PID); // NULL once the task has exited
  rcu_read_unlock();
  if (!p || p->exit_state) {
    reply_error("no such task");
    return NULL;
  }
  return p;
}

static void cmd_create(char *arg) {
  struct kstep_json json;
  struct task_struct *p = kstep_task_create();

  kstep_task_pin(p, 1, last_cpu()); // keep off CPU 0, kSTEP's control CPU
  kstep_task_wakeup(p);
  kstep_json_begin(&json);
  kstep_json_field_s64(&json, "pid", p->pid);
  kstep_json_end(&json);
}

static void cmd_tick(char *arg) {
  struct kstep_json json;

  kstep_tick();
  kstep_json_begin(&json);
  for (int cpu = 1; cpu <= last_cpu(); cpu++) {
    char key[8];
    snprintf(key, sizeof(key), "cpu%d", cpu);
    kstep_json_field_s64(&json, key, task_pid_nr(cpu_rq(cpu)->curr));
  }
  kstep_json_end(&json);
}

static void cmd_task(char *arg) {
  struct kstep_json json;
  struct task_struct *p = parse_task(&arg, "usage: task <pid>");

  if (!p)
    return;
  kstep_json_begin(&json);
  kstep_json_field_s64(&json, "pid", p->pid);
  kstep_json_field_u64(&json, "state", READ_ONCE(p->__state));
  kstep_json_field_s64(&json, "on_cpu", p->on_cpu);
  kstep_json_field_s64(&json, "cpu", task_cpu(p));
  kstep_json_field_fmt(&json, "cpus", "\"%*pbl\"", cpumask_pr_args(p->cpus_ptr));
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

static void cmd_affinity(char *arg) {
  const char *usage = "usage: affinity <pid> <cpulist within 1..N-1>";
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
  const char *usage = "usage: nice <pid> <-20..19>";
  struct task_struct *p = parse_task(&arg, usage);
  int n;

  if (!p)
    return;
  if (!arg || kstrtoint(arg, 10, &n) || n < MIN_NICE || n > MAX_NICE)
    return reply_error(usage);
  kstep_task_set_prio(p, n);
  reply();
}

static const struct {
  const char *verb;
  void (*fn)(char *arg);
} commands[] = {
    {"create", cmd_create}, {"tick", cmd_tick},         {"task", cmd_task},
    {"nice", cmd_nice},     {"affinity", cmd_affinity},
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

      snprintf(usage, sizeof(usage), "usage: %s <pid>", verb);
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
    .on_task_migrate = kstep_output_migrate,
    .step_interval_us = 1000, // no on_tick_begin: the tick reply reports who runs where
};
