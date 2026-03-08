#include <linux/fs.h> // filp_open, filp_close
#include <linux/kernel.h> // printk
#include <linux/string.h> // strstr, strchr, strpbrk
#include <linux/types.h> // ssize_t
#include <linux/ctype.h> // isdigit or alpha
#include "driver.h"
#include "internal.h"
#include "op_handler.h"

#define MAX_LINE_LENGTH 1024
struct console_parse_state {
  char line_buf[MAX_LINE_LENGTH];
  size_t line_len;
};
static struct file *console;

static void parse_console_input(char *buf) {
  char *cursor;
  int fields[4];

  if (!buf)
    return;

  buf = strim(buf);
  if (!*buf)
    return;

  // parse 4 integers separated by commas: TYPE,ARG1,ARG2,ARG3
  cursor = buf;
  for (int i = 0; i < 4; i++) {
    char *token = strsep(&cursor, ",");
    if (!token || !*token)
      return;
    if (kstrtoint(token, 10, &fields[i]) != 0)
      return;
  }

  if (cursor && *cursor)
    return;
  if (fields[0] < OP_TASK_CREATE || fields[0] >= OP_TYPE_NR)
    return;

  TRACE_INFO("EXECOP %d %d %d %d\n", fields[0], fields[1], fields[2], fields[3]);
  kstep_execute_op(fields[0], fields[1], fields[2], fields[3]);
}

static bool process_console_chunk(const char *buf, ssize_t nread,
                                  struct console_parse_state *state) {
  int i;
  for (i = 0; i < nread; i++) {
    char ch = buf[i];
    if (ch == '\n') {
      if (state->line_len + 1 < MAX_LINE_LENGTH) {
        state->line_buf[state->line_len] = '\0';
        if (strcmp(state->line_buf, "EXIT") == 0)
          return true;
        parse_console_input(state->line_buf);
      }
      state->line_len = 0;
    } else if (state->line_len + 1 < MAX_LINE_LENGTH && 
               (isdigit(ch) || isalpha(ch) || ch == ',' || ch == '-')) {
      state->line_buf[state->line_len++] = ch;
    }
  }

  return false;
}

static void setup(void) {
  console = filp_open("/dev/ttyS1", O_RDONLY, 0);
  kstep_cov_init();
}

static void run(void) {
  loff_t pos = 0;
  struct console_parse_state state = {};
  if (IS_ERR(console))
    panic("Failed to open /dev/console");
  while (true) {
    char buf[256];
    ssize_t nread = kernel_read(console, buf, sizeof(buf), &pos);
    if (nread <= 0)
      continue;
    if (process_console_chunk(buf, nread, &state))
      break;
  }

  // A sequence of operations for debugging
  // tasks[0] = kstep_task_create();
  // kstep_tick_repeat(2);
  // kstep_tick();
  // kstep_cgroup_create("cgroot");
  // kstep_cgroup_create("cgroot/cg0");
  // kstep_cgroup_set_cpuset("cgroot/cg0", "2-9");
  // tasks[1] = kstep_task_create();
  // kstep_tick();
  // kstep_tick_repeat(1);
  // kstep_cgroup_set_weight("cgroot/cg0", 4364);
  // kstep_cgroup_add_task("cgroot/cg0", tasks[0]->pid);
  // kstep_task_wakeup(tasks[0]);
  // tasks[2] = kstep_task_create();
  // tasks[3] = kstep_task_create();
  // kstep_cgroup_set_cpuset("cgroot/cg0", "7-9");
  // kstep_cgroup_add_task("cgroot/cg0", tasks[1]->pid);
  // kstep_tick_repeat(4);

  filp_close(console, NULL);
}

KSTEP_DRIVER_DEFINE {
  .name = "executor",
  .setup = setup,
  .run = run,
  .step_interval_us = 1000,
};
