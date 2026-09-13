// The interactive driver. Commands arrive on the channel (io.c) one per line; each is answered with one
// JSON reply, {"timestamp":T,...} or {"timestamp":T,"error":"..."}, in the same ordered stream as
// nothing else. Machine state is not in the stream: after every command
// it is rewritten into a region of guest memory (shm.h) whose address the ready line reports, along
// with the trace events (a CPU's balancer looked for work; a task moved).
//
// Tasks are numbered 1, 2, .. in creation order; a number is never reused.
//
//   create                    new runnable CFS task on CPUs 1..N-1    -> {"timestamp":T,"task":N}
//   tick                      advance one tick
//   nice <n> <-20..19>        kept across a spell as fifo/rr
//   policy <n> <normal|batch|idle|fifo|rr>   real-time classes run at one fixed priority
//   affinity <n> <cpulist>    e.g. 1-2,4
//   pause <n> / wake <n>      sleep when the task next runs / wake it
//   wait <n> / post <n>       semaphore: sleep until a post / wake one waiter with a sync wakeup
//   kill <n>                  exit when the task next runs
//   cgroup-create /a          the parent must exist; / is the root
//   cgroup-weight /a <w>      cpu.weight, 1..10000 (default 100)
//   cgroup-cpus /a <cpulist>  cpuset.cpus
//   attach <n> /a             `attach <n> /` moves the task back to the root
//   exit                      end the session (the VM reboots)
#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/version.h>

#include "driver.h"
#include "internal.h"
#include "shm.h"

#define LINE_MAX 128

// The reply to the command being executed: run() opens it, a command adds its fields (none on
// plain success), run() closes it once the shared region is updated.
static struct kstep_json reply;

static void reply_error(const char *msg) { kstep_json_field_str(&reply, "error", msg); }

// EOPNOTSUPP/EBUSY is cgroup v2's no-internal-process rule.
static void reply_errno(int err) {
  char msg[48];
  if (!err)
    return;
  if (err == -EOPNOTSUPP || err == -EBUSY)
    return reply_error("cgroup cannot hold both tasks and controlled children");
  scnprintf(msg, sizeof(msg), "failed (errno %d)", -err);
  reply_error(msg);
}

static int last_cpu(void) { return num_online_cpus() - 1; }

// Consume one argument while preserving the rest for command-specific validation.
static char *take_arg(char **arg) {
  char *token = *arg ? strsep(arg, " \t") : NULL;

  if (*arg)
    *arg = strim(*arg);
  return token;
}

// The session's tasks by creation number: tasks[n - 1]. Numbering is this front-end's
// convention (the fuzz executor has its own); a number is never reused after `kill`.
static struct task_struct *tasks[KSTEP_SHM_TASKS];
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
  char *num_s = take_arg(arg);
  int n;

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
  struct task_struct *p;

  if (ntasks == KSTEP_SHM_TASKS)
    return reply_error("too many tasks");
  p = kstep_task_create();
  tasks[ntasks++] = p; // the task's kSTEP record keeps it referenced
  kstep_task_pin(p, 1, last_cpu()); // keep off CPU 0, kSTEP's control CPU
  kstep_task_wakeup(p);
  kstep_json_field_s64(&reply, "task", ntasks);
}

static const struct {
  const char *name;
  int policy;
} policies[] = {
    {"normal", SCHED_NORMAL}, {"batch", SCHED_BATCH}, {"idle", SCHED_IDLE},
    {"fifo", SCHED_FIFO},     {"rr", SCHED_RR},
};

static void cmd_tick(char *arg) { kstep_tick(); }

// Both task affinity and cgroup cpusets must exclude the controller CPU.
static bool parse_test_cpus(const char *arg, struct cpumask *mask) {
  return arg && !cpulist_parse(arg, mask) && !cpumask_empty(mask) &&
         !cpumask_test_cpu(0, mask) && cpumask_last(mask) <= last_cpu();
}

static void cmd_affinity(char *arg) {
  const char *usage = "usage: affinity <n> <cpulist within 1..N-1>";
  struct task_struct *p = parse_task(&arg, usage);
  struct cpumask mask;

  if (!p)
    return;
  if (!parse_test_cpus(arg, &mask))
    return reply_error(usage);
  int err = kstep_task_set_affinity(p, &mask);
  // sched_setaffinity rejects a mask disjoint from the task's cpuset (cgroup cpuset.cpus) with EINVAL.
  if (err == -EINVAL)
    return reply_error("cpulist has no CPU in the task's cgroup cpuset");
  reply_errno(err);
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
}

static void cmd_policy(char *arg) {
  const char *usage = "usage: policy <n> <normal|batch|idle|fifo|rr>";
  struct task_struct *p = parse_task(&arg, usage);

  if (!p)
    return;
  for (int i = 0; i < ARRAY_SIZE(policies); i++)
    if (arg && !strcmp(arg, policies[i].name)) {
      kstep_task_set_policy(p, policies[i].policy);
      return;
    }
  reply_error(usage);
}

// Take the leading cgroup path off *arg; the kstep_cgroup_* helpers want it without the
// leading slash. It must exist unless `create`.
static const char *parse_cgroup(char **arg, const char *usage, bool create) {
  char *path = take_arg(arg);

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
  reply_errno(kstep_cgroup_create(name));
}

static void cmd_cgroup_weight(char *arg) {
  const char *usage = "usage: cgroup-weight /path <1..10000>";
  const char *name = parse_cgroup(&arg, usage, false);
  int weight;

  if (!name)
    return;
  if (!arg || kstrtoint(arg, 10, &weight) || weight < 1 || weight > 10000)
    return reply_error(usage);
  reply_errno(kstep_cgroup_set_weight(name, weight));
}

static void cmd_cgroup_cpus(char *arg) {
  const char *usage = "usage: cgroup-cpus /path <cpulist within 1..N-1>";
  const char *name = parse_cgroup(&arg, usage, false);
  struct cpumask mask;

  if (!name)
    return;
  if (!parse_test_cpus(arg, &mask))
    return reply_error(usage);
  reply_errno(kstep_cgroup_set_cpuset(name, arg));
}

static void cmd_attach(char *arg) {
  const char *usage = "usage: attach <n> /path";
  struct task_struct *p = parse_task(&arg, usage);
  const char *name;

  if (!p || !(name = parse_cgroup(&arg, usage, false)))
    return;
  reply_errno(kstep_cgroup_move_task(name, p->pid));
}

static const struct {
  const char *verb;
  void (*fn)(char *arg);
} commands[] = {
    {"create", cmd_create}, {"tick", cmd_tick},
    {"nice", cmd_nice},     {"policy", cmd_policy},     {"affinity", cmd_affinity}, {"attach", cmd_attach},
    {"cgroup-create", cmd_cgroup_create}, {"cgroup-weight", cmd_cgroup_weight},
    {"cgroup-cpus", cmd_cgroup_cpus},
};

// Verbs that signal a task: the SIGUSR1 handler in user/user.c does the work; the signal has
// settled before the verb returns (see kstep_task_signal).
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
      if (p)
        signals[i].fn(p);
      return true;
    }
  if (!strcmp(verb, "exit"))
    return false;
  reply_error("unknown command");
  return true;
}

// on_task_migrate: one of our tasks moved (other processes are not reported)
static void shm_migrate(struct task_struct *p, int src_cpu, int dst_cpu) {
  int n = task_number(p);

  if (n)
    kstep_shm_event(KSTEP_EVENT_MIGRATE, n, src_cpu, dst_cpu, NULL);
}

static phys_addr_t shm_phys;

static void setup(void) { shm_phys = kstep_shm_init(); }

static void run(void) {
  struct kstep_json json;
  char line[LINE_MAX];
  bool more = true;

  kstep_json_begin(&json);
  kstep_json_field_bool(&json, "ready", true);
  kstep_json_field_u64(&json, "shm", shm_phys); // guest-physical address of the shared region
  kstep_json_end(&json);
  while (more) {
    char *l;

    kstep_io_readline(line, sizeof(line));
    l = strim(line);
    if (!*l || *l == '#')
      continue;
    kstep_json_begin(&reply);
    more = execute(l);
    kstep_shm_update(tasks, ntasks); // before the reply, so the region is current when it lands
    kstep_json_end(&reply);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "cli",
    .setup = setup,
    .run = run,
    .on_sched_balance_selected = kstep_shm_balance,
    .on_task_migrate = shm_migrate,
};
