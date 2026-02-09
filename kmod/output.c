#include <linux/fs.h>
#include <linux/ftrace.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>

#include "internal.h"

#define K(s) "\"" #s "\": "
#define OUTPUT_BUF_SIZE 4096

static const char *output_paths[] = {
    "/dev/ttyAMA1", // ARM PL011
    "/dev/ttyS1",   // x86 8250
    NULL,
};

static struct file *output_file;

void kstep_output_init(void) {
  kstep_sysctl_write("kernel.printk", "%d", 7);

  for (const char **path = output_paths; *path; path++) {
    output_file = filp_open(*path, O_WRONLY | O_NOCTTY, 0);
    if (!IS_ERR(output_file))
      return;
  }
  panic("trace output not available");
}

void kstep_output(const void *buf, size_t len) {
  ssize_t ret = kernel_write(output_file, buf, len, NULL);
  if (ret < 0)
    panic("kstep_output failed: %ld", ret);
}

struct kstep_json {
  char buf[OUTPUT_BUF_SIZE];
  size_t len;
};

struct kstep_json *kstep_json_begin(void) {
  struct kstep_json *json = kzalloc(sizeof(*json), GFP_KERNEL);
  if (!json)
    panic("Failed to allocate json");
  kstep_json_field(json, "timestamp", "%lu", kstep_jiffies_get());
  json->buf[0] = '{';
  return json;
}

static void kstep_json_append(struct kstep_json *json, const char *buf,
                              size_t len) {
  if (json->len + len >= OUTPUT_BUF_SIZE)
    panic("json buffer overflow");
  memcpy(json->buf + json->len, buf, len);
  json->len += len;
}

void kstep_json_field(struct kstep_json *json, const char *key, const char *fmt,
                      ...) {
  kstep_json_append(json, ",\"", 2);
  kstep_json_append(json, key, strlen(key));
  kstep_json_append(json, "\":", 2);

  int rem = OUTPUT_BUF_SIZE - json->len;

  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(json->buf + json->len, rem, fmt, args);
  va_end(args);

  if (len < 0 || len >= rem)
    panic("json formatting failed");
  json->len += len;
}

void kstep_json_end(struct kstep_json *json) {
  kstep_json_append(json, "}\n", 2);
  kstep_output(json->buf, json->len);
  kfree(json);
}

static void print_rq(struct rq *rq) {
// https://github.com/torvalds/linux/commit/c2a295bffeaf9461ecba76dc9e4780c898c94f03
// https://github.com/torvalds/linux/commit/7b8a702d943827130cc00ae36075eff5500f86f1
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  int h_nr_runnable = rq->cfs.h_nr_runnable;
  int h_nr_queued = rq->cfs.h_nr_queued;
#else
  int h_nr_runnable = 0;
  int h_nr_queued = 0;
#endif

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  u64 avg_load = rq->cfs.avg_load;
  KSYM_IMPORT(avg_vruntime);
  u64 avg_vruntime = KSYM_avg_vruntime(&rq->cfs) - INIT_TIME_NS;
#else
  u64 avg_load = 0;
  u64 avg_vruntime = 0;
#endif

  u64 avg_util =
      rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg;

// https://github.com/torvalds/linux/commit/79f3f9bedd149ea438aaeb0fb6a083637affe205
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  u64 min_vruntime = rq->cfs.zero_vruntime;
#else
  u64 min_vruntime = rq->cfs.min_vruntime;
#endif

  pr_info("rq: {");
  pr_cont(K(cpu) "%d, ", rq->cpu);
  pr_cont(K(running) "%2d, ", rq->nr_running - (h_nr_queued - h_nr_runnable));
  pr_cont(K(queued) "%2d, ", rq->nr_running);
  pr_cont(K(avg_load) "%4llu, ", avg_load);
  pr_cont(K(avg_util) "%4llu, ", avg_util);
  pr_cont(K(min_vruntime) "%12lld, ", min_vruntime - INIT_TIME_NS);
  pr_cont(K(avg_vruntime) "%12lld", avg_vruntime);
  pr_cont("}\n");
}

void kstep_print_rq(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    print_rq(cpu_rq(cpu));
}

static void print_task(struct task_struct *p) {
  pr_info("task: {");
  pr_cont(K(pid) "%d, ", task_pid_nr(p));
  pr_cont(K(on_cpu) "%5s, ", p->on_cpu ? "true" : "false");
  pr_cont(K(cpu) "%d, ", task_cpu(p));
  pr_cont(K(state) "\"%c\", ", task_state_to_char(p));
  pr_cont(K(vruntime) "%12lld, ", p->se.vruntime);
  pr_cont(K(sum_exec) "%12lld", p->se.sum_exec_runtime);
  pr_cont("}\n");
}

void kstep_print_tasks(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == 0 || kstep_is_sys_kthread(p))
      continue;
    print_task(p);
  }
}

struct lb_env {
  struct sched_domain *sd;
  struct rq *src_rq;
  int src_cpu;
  int dst_cpu;
  struct rq *dst_rq;
  // other fields are not needed
};

static void load_balance_enter(unsigned long ip, unsigned long parent_ip,
                               struct ftrace_ops *op,
                               struct ftrace_regs *fregs) {
  struct lb_env *env = (void *)regs_get_kernel_argument((void *)fregs, 0);
  if (env->dst_cpu == 0)
    return;
  pr_info("load_balance: {" K(dst_cpu) "%d, " K(span) "\"%*pbl\", " K(
              name) "\"%s\"}\n",
          env->dst_cpu, cpumask_pr_args(sched_domain_span(env->sd)),
          env->sd->name);
}

struct ftrace_ops load_balance_enter_op = {
    .func = load_balance_enter,
    .flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION,
};

void kstep_trace_load_balance(void) {
  // We use `sched_balance_find_src_group`/`find_busiest_group` as a proxy to
  // trace `sched_balance_rq`/`load_balance` when `should_we_balance` is true.

// https://github.com/torvalds/linux/commit/82cf921432fc184adbbb9c1bced182564876ec5e
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  char *name = "sched_balance_find_src_group";
#else
  char *name = "find_busiest_group";
#endif

  if (ftrace_set_filter(&load_balance_enter_op, name, strlen(name), 1))
    panic("Failed to set filter for %s", name);

  if (register_ftrace_function(&load_balance_enter_op))
    panic("Failed to register ftrace function for %s", name);

  TRACE_INFO("Traced %s for load balancing", name);
}
