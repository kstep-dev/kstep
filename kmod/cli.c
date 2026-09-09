// Interactive driver: a line-oriented command interface on /dev/ttyS3.
//
// One command per line in, one flat JSON object per line out. Every reply carries
// "timestamp" (logical ticks) and "ok"; errors add "error". The trace on ttyS1 is
// produced as for any other driver, and CPU topology/capacity come from the usual
// topology=/capacity= boot parameters.
//
//   create                    new runnable CFS task on CPUs 1..N-1     -> {..,"pid":N}
//   tick                      advance one tick; report who runs where   -> {..,"cpu1":N,"cpu2":N,...}  (0 = idle)
//   task <pid>                scheduler counters of one task           -> {..,"state":..,"cpus":"1-2","vruntime":..,...}
//   nice <pid> <-20..19>      set the nice value
//   affinity <pid> <cpulist>  set the CPUs the task may run on, e.g. 1-2,4
//   pause <pid>               put the task to sleep (it does when it next runs)
//   wake <pid>                wake a paused task
//   kill <pid>                ask the task to exit (it does when it next runs)
//   exit                      end the session (the VM reboots)
//
// A <pid> that no longer exists answers "no such task", which is how a client
// learns about exits.
#include <linux/cpumask.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#define LINE_MAX 128

static struct file *cmd; // /dev/ttyS3

static struct task_struct *find(pid_t pid) {
  struct task_struct *p;

  rcu_read_lock();
  p = pid_task(find_vpid(pid), PIDTYPE_PID); // NULL once the task has exited
  rcu_read_unlock();
  return p && !p->exit_state ? p : NULL;
}

static void reply_begin(struct kstep_json *json, bool ok) {
  kstep_json_begin(json);
  kstep_json_field_bool(json, "ok", ok);
}

static void reply_end(struct kstep_json *json) { kstep_json_end_to(json, cmd); }

static void reply_ok(void) {
  struct kstep_json json;
  reply_begin(&json, true);
  reply_end(&json);
}

static void reply_error(const char *msg) {
  struct kstep_json json;
  reply_begin(&json, false);
  kstep_json_field_str(&json, "error", msg);
  reply_end(&json);
}

static int last_cpu(void) { return num_online_cpus() - 1; }

// Take the leading <pid> off *arg (the rest stays in *arg) and look the task up;
// replies with an error and returns NULL if there is no such task.
static struct task_struct *parse_task(char **arg, const char *usage) {
  char *pid_s = *arg ? strsep(arg, " \t") : NULL;
  struct task_struct *p;
  int pid;

  if (*arg)
    *arg = strim(*arg);
  if (!pid_s || kstrtoint(pid_s, 10, &pid) || pid <= 0) {
    reply_error(usage);
    return NULL;
  }
  p = find(pid);
  if (!p)
    reply_error("no such task");
  return p;
}

static void cmd_create(void) {
  struct kstep_json json;
  struct task_struct *p = kstep_task_create();

  kstep_task_pin(p, 1, last_cpu()); // keep off CPU 0, kSTEP's control CPU
  kstep_task_wakeup(p);
  reply_begin(&json, true);
  kstep_json_field_s64(&json, "pid", p->pid);
  reply_end(&json);
}

static void cmd_tick(void) {
  struct kstep_json json;

  kstep_tick();
  reply_begin(&json, true);
  for (int cpu = 1; cpu <= last_cpu(); cpu++) {
    char key[8];
    snprintf(key, sizeof(key), "cpu%d", cpu);
    kstep_json_field_s64(&json, key, task_pid_nr(cpu_rq(cpu)->curr));
  }
  reply_end(&json);
}

static void cmd_task(char *arg) {
  struct kstep_json json;
  struct task_struct *p = parse_task(&arg, "usage: task <pid>");

  if (!p)
    return;
  reply_begin(&json, true);
  kstep_json_field_s64(&json, "pid", p->pid);
  kstep_json_field_u64(&json, "state", READ_ONCE(p->__state));
  kstep_json_field_s64(&json, "on_cpu", p->on_cpu);
  kstep_json_field_s64(&json, "cpu", task_cpu(p));
  kstep_json_field_fmt(&json, "cpus", "\"%*pbl\"", cpumask_pr_args(p->cpus_ptr));
  kstep_json_field_s64(&json, "nice", task_nice(p));
  kstep_json_field_u64(&json, "weight", p->se.load.weight);
  kstep_json_field_u64(&json, "sum_exec_runtime", p->se.sum_exec_runtime);
  kstep_json_field_u64(&json, "vruntime", p->se.vruntime);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  kstep_json_field_u64(&json, "deadline", p->se.deadline);
  kstep_json_field_u64(&json, "slice", p->se.slice);
#endif
  reply_end(&json);
}

static void cmd_affinity(char *arg) {
  const char *usage = "usage: affinity <pid> <cpulist within 1..N-1>";
  struct task_struct *p = parse_task(&arg, usage);
  struct cpumask mask;

  if (!p)
    return;
  if (!arg || cpulist_parse(arg, &mask) || cpumask_empty(&mask) ||
      cpumask_test_cpu(0, &mask) || cpumask_last(&mask) > last_cpu()) {
    reply_error(usage);
    return;
  }
  kstep_task_set_affinity(p, &mask);
  reply_ok();
}

static void cmd_nice(char *arg) {
  const char *usage = "usage: nice <pid> <-20..19>";
  struct task_struct *p = parse_task(&arg, usage);
  int n;

  if (!p)
    return;
  if (!arg || kstrtoint(arg, 10, &n) || n < MIN_NICE || n > MAX_NICE) {
    reply_error(usage);
    return;
  }
  kstep_task_set_prio(p, n);
  reply_ok();
}

// pause, wake and kill all just signal the task; SIGUSR1 handlers in the guest
// process (user/user.c) do the actual pause(), wakeup and exit when it next runs.
static void cmd_signal(char *arg, const char *usage,
                       void (*fn)(struct task_struct *)) {
  struct task_struct *p = parse_task(&arg, usage);

  if (!p)
    return;
  fn(p);
  reply_ok();
}

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

  if (!strcmp(verb, "create"))
    cmd_create();
  else if (!strcmp(verb, "tick"))
    cmd_tick();
  else if (!strcmp(verb, "task"))
    cmd_task(arg);
  else if (!strcmp(verb, "pause"))
    cmd_signal(arg, "usage: pause <pid>", kstep_task_pause);
  else if (!strcmp(verb, "wake"))
    cmd_signal(arg, "usage: wake <pid>", kstep_task_wakeup);
  else if (!strcmp(verb, "kill"))
    cmd_signal(arg, "usage: kill <pid>", kstep_task_exit);
  else if (!strcmp(verb, "affinity"))
    cmd_affinity(arg);
  else if (!strcmp(verb, "nice"))
    cmd_nice(arg);
  else if (!strcmp(verb, "exit")) {
    reply_ok();
    return false;
  } else
    reply_error("unknown command");
  return true;
}

static void setup(void) {
  cmd = filp_open("/dev/ttyS3", O_RDWR | O_NOCTTY, 0);
  if (IS_ERR(cmd))
    panic("Failed to open /dev/ttyS3: %ld", PTR_ERR(cmd));
}

static void run(void) {
  struct kstep_json json;
  char line[LINE_MAX], c;
  size_t len = 0;
  loff_t pos = 0;

  reply_begin(&json, true);
  kstep_json_field_bool(&json, "ready", true);
  reply_end(&json);
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
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 1000,
};
