#include "internal.h"

static struct file *output_file;

void kstep_output_init(void) {
  output_file = filp_open("/dev/hvc0", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(output_file))
    panic("Failed to open /dev/hvc0: %ld", PTR_ERR(output_file));
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

static void kstep_json_field_vfmt(struct kstep_json *json, const char *key,
                                   const char *val_fmt, va_list args) {
  kstep_json_append_str(json, key);
  kstep_json_append_char(json, ':');

  int rem = sizeof(json->buf) - json->len;
  int len = vsnprintf(json->buf + json->len, rem, val_fmt, args);
  if (len < 0 || len >= rem)
    panic("json formatting failed");
  json->len += len;
  kstep_json_append_char(json, ',');
}

void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *val_fmt, ...) {
  va_list args;
  va_start(args, val_fmt);
  kstep_json_field_vfmt(json, key, val_fmt, args);
  va_end(args);
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

void kstep_json_field_bool(struct kstep_json *json, const char *key, bool val) {
  kstep_json_field_fmt(json, key, "%s", val ? "true" : "false");
}

// Only the controller, on CPU 0 in process context, writes to the port. Every line is appended here
// first; the controller then writes the buffer out, so lines from scheduler hooks (any CPU, interrupts
// off, runqueue locks held) come out before its own and the stream keeps its order. A tty write from
// a hook (8250 or hvc alike) would join the tty's wait queue and spin for the port lock, while CPU 0
// draining the port holds that lock and spins in try_to_wake_up() on the writer, still on-cpu inside
// __schedule().
static char pending[1 << 16];
static size_t pending_len;
static DEFINE_RAW_SPINLOCK(pending_lock);

// Called by the controller only, so one static copy suffices; the write itself may sleep. The
// controller flushes where it pauses (after a tick, after a cli command, at exit): one tty write
// per batch instead of one per record, each a virtqueue kick and, under emulation, a host exit.
void kstep_output_flush(void) {
  static char flushing[sizeof(pending)];
  size_t len;

  raw_spin_lock_irq(&pending_lock);
  len = pending_len;
  memcpy(flushing, pending, len);
  pending_len = 0;
  raw_spin_unlock_irq(&pending_lock);
  if (!len)
    return;
  ssize_t ret = kernel_write(output_file, flushing, len, NULL);
  if (ret < 0)
    panic("write to output file failed: %ld", ret);
}

void kstep_json_end(struct kstep_json *json) {
  unsigned long flags;

  if (json->len > 0 && json->buf[json->len - 1] == ',')
    json->len--;
  kstep_json_append_char(json, '}');
  kstep_json_append_char(json, '\n');
  raw_spin_lock_irqsave(&pending_lock, flags);
  if (pending_len + json->len > sizeof(pending))
    panic("pending output overflow");
  memcpy(pending + pending_len, json->buf, json->len);
  pending_len += json->len;
  raw_spin_unlock_irqrestore(&pending_lock, flags);
  if (!irqs_disabled() && pending_len > sizeof(pending) / 2) // a controller printing a lot between pauses
    kstep_output_flush();
}

void kstep_json_print_2kv(const char *key1, const char *val1, const char *key2,
                          const char *val2_fmt, ...) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, key1, val1);

  va_list args;
  va_start(args, val2_fmt);
  kstep_json_field_vfmt(&json, key2, val2_fmt, args);
  va_end(args);

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

void kstep_output_migrate(struct task_struct *p, int src_cpu, int dst_cpu) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "migrate");
  kstep_json_field_u64(&json, "pid", task_pid_nr(p));
  kstep_json_field_u64(&json, "src_cpu", src_cpu);
  kstep_json_field_u64(&json, "dst_cpu", dst_cpu);
  kstep_json_end(&json);
}
