#include <linux/fs.h> // filp_open, filp_close
#include <linux/kernel.h> // printk
#include <linux/slab.h> // kmalloc, kfree, krealloc
#include <linux/types.h> // ssize_t
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
  int a, b, c;
};

#define KSTEP_SEQ_MAX 1024
static struct kstep_op kstep_seq[KSTEP_SEQ_MAX];
static int kstep_seq_len;
// static struct task_struct **kstep_tasks;
// static int kstep_tasks_len;
static struct file *console;

struct console_parse_state {
  char line_buf[1024];
  size_t line_len;
  bool line_too_long;
};

static void execute_kstep_seq(void) {
  for (int i = 0; i < kstep_seq_len; i++) {
    struct kstep_op *op = &kstep_seq[i];
    pr_info("Execute op %d: %d %d %d %d\n", i, op->type, op->a, op->b, op->c);
  }
  kstep_seq_len = 0;
}

static bool append_kstep_op(const struct kstep_op *op) {
  kstep_seq[kstep_seq_len] = *op;
  kstep_seq_len++;
  pr_info("Batch op %d: %d %d %d %d\n", kstep_seq_len, op->type, op->a, op->b, op->c);

  if (kstep_seq_len == KSTEP_SEQ_MAX) {
    pr_info("executor: kstep_seq reached max %d, auto execute\n", KSTEP_SEQ_MAX);
    execute_kstep_seq();
  }

  return true;
}

static bool parse_int_token(char *token, int *out) {
  if (!token || !*token)
    return false;
  return kstrtoint(token, 10, out) == 0;
}

static void parse_console_input(char *buf) {
  char *cursor;
  int fields[4];
  struct kstep_op op;

  if (!buf) return;

  buf = strim(buf);
  if (!*buf) return;

  cursor = buf;
  for (int i = 0; i < 4; i++) {
    char *token = strsep(&cursor, ",");
    if (!parse_int_token(token, &fields[i])) {
      pr_warn("executor: ignore invalid line `%s`\n", buf);
      return;
    }
  }

  if (cursor && *cursor) {
    pr_warn("executor: ignore invalid line `%s`\n", buf);
    return;
  }

  if (fields[0] < OP_TASK_CREATE || fields[0] > OP_CPU_SET_CAPACITY) {
    pr_warn("executor: ignore unknown op type %d in `%s`\n", fields[0], buf);
    return;
  }

  op.type = fields[0];
  op.a = fields[1];
  op.b = fields[2];
  op.c = fields[3];
  if (!append_kstep_op(&op))
    panic("Failed to append kstep op");

}

static bool process_console_chunk(const char *buf, ssize_t nread, struct console_parse_state *state) {
  int i;
  for (i = 0; i < nread; i++) {
    char ch = buf[i];
    if (ch == '\r') continue;

    if (ch == '\n') {
      if (state->line_too_long) {
        pr_warn("executor: ignore overlong input line\n");
        state->line_too_long = false;
        state->line_len = 0;
        continue;
      }

      state->line_buf[state->line_len] = '\0';
      state->line_len = 0;
      if (strcmp(state->line_buf, "EXECUTE") == 0) {
        execute_kstep_seq();
      } else if (strcmp(state->line_buf, "FINISH") == 0) {
        return true;
      } else {
        parse_console_input(state->line_buf);
      }
      continue;
    }

    if (state->line_too_long)
      continue;
    if (state->line_len + 1 >= sizeof(state->line_buf)) {
      state->line_too_long = true;
      continue;
    }
    state->line_buf[state->line_len++] = ch;
  }

  return false;
}

static void setup(void) {
  console = filp_open("/dev/console", O_RDONLY, 0);
}

static void run(void) {
  loff_t pos = 0;
  kstep_seq_len = 0;
  struct console_parse_state state = {};
  if (IS_ERR(console))
    panic("Failed to open /dev/console");
  while (true) {
    char buf[256];
    ssize_t nread = kernel_read(console, buf, sizeof(buf), &pos);
    if (nread <= 0)
      continue;
    bool finished = process_console_chunk(buf, nread, &state);
    if (finished) 
      break;
  }

  filp_close(console, NULL);

}


KSTEP_DRIVER_DEFINE{
    .name = "executor",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = false,
};
