#include <linux/fs.h> // filp_open, filp_close
#include <linux/kernel.h> // printk
#include <linux/slab.h> // kmalloc, kfree, krealloc
#include <linux/string.h> // strstr, strchr, strpbrk

#include "driver.h"

enum kstep_op_type {
  OP_TASK_CREATE,
  OP_TASK_FORK,
  OP_TASK_PIN,
  OP_TASK_FIFO,
  OP_TASK_PAUSE,
  OP_TASK_WAKEUP,
  OP_TASK_SET_PRIO,
  OP_TICK,
  OP_TICK_REPEAT,
  OP_CGROUP_CREATE,
  OP_CGROUP_SET_CPUSET,
  OP_CGROUP_SET_WEIGHT,
  OP_CGROUP_ADD_TASK,
  OP_CPU_SET_FREQ,
  OP_CPU_SET_CAPACITY,
};

struct kstep_op {
  enum kstep_op_type type;
  int a;
  int b;
  int c;
};

static struct kstep_op *kstep_seq;
static int kstep_seq_len;

static enum kstep_op_type parse_op_type(const char *name) {
  if (!strcmp(name, "TASK_CREATE")) return OP_TASK_CREATE;
  if (!strcmp(name, "TASK_FORK")) return OP_TASK_FORK;
  if (!strcmp(name, "TASK_PIN")) return OP_TASK_PIN;
  if (!strcmp(name, "TASK_FIFO")) return OP_TASK_FIFO;
  if (!strcmp(name, "TASK_PAUSE")) return OP_TASK_PAUSE;
  if (!strcmp(name, "TASK_WAKEUP")) return OP_TASK_WAKEUP;
  if (!strcmp(name, "TASK_SET_PRIO")) return OP_TASK_SET_PRIO;
  if (!strcmp(name, "TICK")) return OP_TICK;
  if (!strcmp(name, "TICK_REPEAT")) return OP_TICK_REPEAT;
  if (!strcmp(name, "CGROUP_CREATE")) return OP_CGROUP_CREATE;
  if (!strcmp(name, "CGROUP_SET_CPUSET")) return OP_CGROUP_SET_CPUSET;
  if (!strcmp(name, "CGROUP_SET_WEIGHT")) return OP_CGROUP_SET_WEIGHT;
  if (!strcmp(name, "CGROUP_ADD_TASK")) return OP_CGROUP_ADD_TASK;
  if (!strcmp(name, "CPU_SET_FREQ")) return OP_CPU_SET_FREQ;
  if (!strcmp(name, "CPU_SET_CAPACITY")) return OP_CPU_SET_CAPACITY;
  panic("Unknown op type: %s", name);
  return OP_TICK;
}

static char *read_console_buf(struct file *console) {
  size_t cap = 1024;
  size_t len = 0;
  char *buf = kmalloc(cap, GFP_KERNEL);
  if (!buf)
    panic("Failed to allocate console buffer");

  while (true) {
    if (len + 256 + 1 > cap) {
      size_t new_cap = cap * 2;
      char *new_buf = krealloc(buf, new_cap, GFP_KERNEL);
      if (!new_buf) {
        kfree(buf);
        panic("Failed to grow console buffer");
      }
      buf = new_buf;
      cap = new_cap;
    }

    ssize_t n = kernel_read(console, buf + len, cap - len - 1, NULL);
    if (n < 0) {
      kfree(buf);
      panic("Failed to read /dev/console: %zd", n);
    }
    if (n == 0)
      break;
    len += n;
    buf[len] = '\0';
    if (strchr(buf, '!') || strpbrk(buf, "\n\r"))
      break;
  }

  return buf;
}

// test_spec=TASK_CREATE,1,2,3|TASK_FORK,4,5,6|...|...!
// | is the separator between ops
// ! is the end of the command sequence string
static void parse_test_spec(char *buf) {
  char *spec = strstr(buf, "test_spec=");
  if (!spec)
    panic("Missing test_spec in console input");
  spec += strlen("test_spec=");

  char *end = strchr(spec, '!');
  if (!end)
    panic("Empty test_spec");
  *end = '\0';

  int count = 1;
  for (char *p = spec; *p; p++) {
    if (*p == '|')
      count++;
  }
  kstep_seq = kcalloc(count, sizeof(*kstep_seq), GFP_KERNEL);
  if (!kstep_seq)
    panic("Failed to allocate kstep_seq");

  char *cursor = spec;
  int idx = 0;
  while (cursor && *cursor) {
    char *op_str = strsep(&cursor, "|");
    if (!op_str || !*op_str)
      panic("Empty op in test_spec");

    char *name = strsep(&op_str, ",");
    char *a_str = strsep(&op_str, ",");
    char *b_str = strsep(&op_str, ",");
    char *c_str = strsep(&op_str, ",");
    if (!name || !a_str || !b_str || !c_str)
      panic("Malformed op in test_spec");

    int a, b, c;
    if (kstrtoint(a_str, 10, &a) || kstrtoint(b_str, 10, &b) ||
        kstrtoint(c_str, 10, &c))
      panic("Invalid op args in test_spec");

    kstep_seq[idx].type = parse_op_type(name);
    kstep_seq[idx].a = a;
    kstep_seq[idx].b = b;
    kstep_seq[idx].c = c;
    printk("parsed op %d: %s %d %d %d", idx, name, a, b, c);
    idx++;
  }
  kstep_seq_len = idx;
  printk("test_spec parsed ops: %d", kstep_seq_len);
}

static void setup(void) {
  struct file *console = filp_open("/dev/console", O_RDONLY, 0);
  if (IS_ERR(console))
    panic("Failed to open /dev/console");

  char *buf = read_console_buf(console);
  parse_test_spec(buf);
  filp_close(console, NULL);
  kfree(buf);
}

static void run(void) {
  // TODO: Implement the test driver
}


KSTEP_DRIVER_DEFINE{
    .name = "executor",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
};
