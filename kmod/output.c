#include <linux/fs.h>
#include <linux/ftrace.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>

#include "internal.h"

#define K(s) "\"" #s "\": "
#define OUTPUT_BUF_SIZE 65536

static struct file *output_file;

void kstep_output_init(void) {
  output_file = filp_open("/dev/ttyS1", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(output_file))
    panic("Failed to open /dev/ttyS1: %ld", PTR_ERR(output_file));
}

struct kstep_json {
  char buf[OUTPUT_BUF_SIZE];
  size_t len;
};

struct kstep_json *kstep_json_begin(void) {
  struct kstep_json *json = kzalloc(sizeof(*json), GFP_KERNEL);
  if (!json)
    panic("Failed to allocate json");
  json->buf[json->len++] = '{';
  kstep_json_field_u64(json, "timestamp", kstep_jiffies_get());
  return json;
}

static void kstep_json_append(struct kstep_json *json, const char *buf,
                              size_t len) {
  if (json->len + len >= OUTPUT_BUF_SIZE)
    panic("json buffer overflow");
  memcpy(json->buf + json->len, buf, len);
  json->len += len;
}

void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *value, ...) {
  kstep_json_append(json, "\"", 1);
  kstep_json_append(json, key, strlen(key));
  kstep_json_append(json, "\":", 2);

  int rem = OUTPUT_BUF_SIZE - json->len;

  va_list args;
  va_start(args, value);
  int len = vsnprintf(json->buf + json->len, rem, value, args);
  va_end(args);

  if (len < 0 || len >= rem)
    panic("json formatting failed");
  json->len += len;

  kstep_json_append(json, ",", 1);
}

void kstep_json_field_str(struct kstep_json *json, const char *key,
                          const char *value) {
  kstep_json_append(json, "\"", 1);
  kstep_json_append(json, key, strlen(key));
  kstep_json_append(json, "\":\"", 3);
  kstep_json_append(json, value, strlen(value));
  kstep_json_append(json, "\",", 2);
}

void kstep_json_field_u64(struct kstep_json *json, const char *key, u64 value) {
  kstep_json_field_fmt(json, key, "%llu", value);
}

void kstep_json_field_s64(struct kstep_json *json, const char *key, s64 value) {
  kstep_json_field_fmt(json, key, "%lld", value);
}

void kstep_json_end(struct kstep_json *json) {
  json->buf[json->len - 1] = '}';
  kstep_json_append(json, "\n", 1);
  ssize_t ret = kernel_write(output_file, json->buf, json->len, NULL);
  if (ret < 0)
    panic("write to output file failed: %ld", ret);
  kfree(json);
}

void kstep_print_sched_debug(void) {
  KSYM_IMPORT(sysrq_sched_debug_show);
  KSYM_sysrq_sched_debug_show();
}

void kstep_output_curr_task(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct task_struct *curr = cpu_rq(cpu)->curr;
    struct kstep_json *json = kstep_json_begin();
    kstep_json_field_str(json, "type", "curr_task");
    kstep_json_field_u64(json, "cpu", cpu);
    kstep_json_field_u64(json, "pid", task_pid_nr(curr));
    kstep_json_end(json);
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
  if (kstep_jiffies_get() == 0)
    return;

  struct kstep_json *json = kstep_json_begin();
  kstep_json_field_str(json, "type", "load_balance");
  kstep_json_field_u64(json, "dst_cpu", env->dst_cpu);
  kstep_json_field_fmt(json, "span", "\"%*pbl\"",
                       cpumask_pr_args(sched_domain_span(env->sd)));
  kstep_json_field_str(json, "name", env->sd->name);
  kstep_json_end(json);
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
