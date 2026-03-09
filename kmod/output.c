#include "internal.h"

static struct file *output_file;

void kstep_output_init(void) {
  output_file = filp_open("/dev/ttyS1", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(output_file))
    panic("Failed to open /dev/ttyS1: %ld", PTR_ERR(output_file));
}

static void kstep_json_append_char(struct kstep_json *json, char c) {
  if (json->len + 1 >= sizeof(json->buf))
    panic("json buffer overflow");
  json->buf[json->len++] = c;
}

static void kstep_json_append_buf(struct kstep_json *json, const char *buf,
                                  size_t len) {
  if (json->len + len >= sizeof(json->buf))
    panic("json buffer overflow");
  memcpy(json->buf + json->len, buf, len);
  json->len += len;
}

static void kstep_json_append_str(struct kstep_json *json, const char *str) {
  kstep_json_append_char(json, '"');
  kstep_json_append_buf(json, str, strlen(str));
  kstep_json_append_char(json, '"');
}

static void kstep_json_append_fmt(struct kstep_json *json, const char *fmt,
                                  va_list args) {
  int rem = sizeof(json->buf) - json->len;
  int len = vsnprintf(json->buf + json->len, rem, fmt, args);
  if (len < 0 || len >= rem)
    panic("json formatting failed");
  json->len += len;
}

void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *val_fmt, ...) {
  kstep_json_append_str(json, key);
  kstep_json_append_char(json, ':');

  va_list args;
  va_start(args, val_fmt);
  kstep_json_append_fmt(json, val_fmt, args);
  va_end(args);

  kstep_json_append_char(json, ',');
}

void kstep_json_field_str(struct kstep_json *json, const char *key,
                          const char *val) {
  kstep_json_append_str(json, key);
  kstep_json_append_char(json, ':');
  kstep_json_append_str(json, val);
  kstep_json_append_char(json, ',');
}

void kstep_json_field_u64(struct kstep_json *json, const char *key, u64 val) {
  kstep_json_field_fmt(json, key, "%llu", val);
}

void kstep_json_field_s64(struct kstep_json *json, const char *key, s64 val) {
  kstep_json_field_fmt(json, key, "%lld", val);
}

void kstep_json_begin(struct kstep_json *json) {
  json->len = 0;
  kstep_json_append_char(json, '{');
  kstep_json_field_u64(json, "timestamp", kstep_jiffies_get());
}

void kstep_json_end(struct kstep_json *json) {
  if (json->len > 0 && json->buf[json->len - 1] == ',')
    json->len--;
  kstep_json_append_char(json, '}');
  kstep_json_append_char(json, '\n');
  ssize_t ret = kernel_write(output_file, json->buf, json->len, NULL);
  if (ret < 0)
    panic("write to output file failed: %ld", ret);
}

void kstep_json_print_2kv(const char *key1, const char *val1, const char *key2,
                          const char *val2_fmt, ...) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, key1, val1);
  kstep_json_append_str(&json, key2);
  kstep_json_append_char(&json, ':');

  va_list args;
  va_start(args, val2_fmt);
  kstep_json_append_fmt(&json, val2_fmt, args);
  va_end(args);

  kstep_json_append_char(&json, ',');
  kstep_json_end(&json);
}

void kstep_print_sched_debug(void) {
  KSYM_IMPORT(sysrq_sched_debug_show);
  KSYM_sysrq_sched_debug_show();
}

void kstep_output_curr_task(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct task_struct *curr = cpu_rq(cpu)->curr;
    struct kstep_json json;
    kstep_json_begin(&json);
    kstep_json_field_str(&json, "type", "curr_task");
    kstep_json_field_u64(&json, "cpu", cpu);
    kstep_json_field_u64(&json, "pid", task_pid_nr(curr));
    kstep_json_end(&json);
  }
}

void kstep_output_nr_running(void) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "nr_running");
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    char key[8];
    snprintf(key, sizeof(key), "cpu%d", cpu);
    kstep_json_field_u64(&json, key, cpu_rq(cpu)->nr_running);
  }
  kstep_json_end(&json);
}

void kstep_output_balance(int cpu, struct sched_domain *sd) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "load_balance");
  kstep_json_field_u64(&json, "dst_cpu", cpu);
  kstep_json_field_fmt(&json, "span", "\"%*pbl\"",
                       cpumask_pr_args(sched_domain_span(sd)));
  kstep_json_field_str(&json, "name", sd->name);
  kstep_json_end(&json);
}
