#include <linux/fs.h> // filp_open, filp_close
#include <linux/kernel.h> // printk
#include <linux/moduleparam.h> // module_param_string
#include <linux/string.h> // strstr, strchr, strpbrk
#include <linux/types.h> // ssize_t
#include <linux/ctype.h> // isdigit or alpha
#include "driver.h"
#include "internal.h"
#include "checker.h"
#include "handler.h"

static char executor_topology[CPU_SPEC_LEN] = "";
module_param_string(topology, executor_topology, CPU_SPEC_LEN, 0644);

static char executor_capacity[CPU_SPEC_LEN] = "";
module_param_string(capacity, executor_capacity, CPU_SPEC_LEN, 0644);

static char executor_frequency[CPU_SPEC_LEN] = "";
module_param_string(frequency, executor_frequency, CPU_SPEC_LEN, 0644);

#define MAX_LINE_LENGTH 1024
struct console_parse_state {
  char line_buf[MAX_LINE_LENGTH];
  size_t line_len;
};
static struct file *console;
static struct file *sock;

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

  kstep_write_state(sock, kstep_execute_op(fields[0], fields[1], fields[2], fields[3]));
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
  sock = filp_open("/dev/ttyS3", O_RDWR, 0);

  if (executor_capacity[0])
    kstep_cap_set(executor_capacity);
  if (executor_topology[0])
    kstep_topo_set(executor_topology);
  if (executor_frequency[0])
    kstep_freq_set(executor_frequency);
  kstep_cov_init();
}

static void run(void) {
  loff_t pos = 0;
  struct console_parse_state state = {};

  if (IS_ERR(sock))
    panic("Failed to open /dev/ttyS3");

  /* Signal to Python that the kmod is ready. Keep the step count non-zero to
   * avoid an all-zero payload. */
  kstep_write_state(sock, 1);

  while (true) {
    char buf[256];
    ssize_t nread = kernel_read(sock, buf, sizeof(buf), &pos);
    if (nread <= 0)
      continue;

    // Checker: whether work conserving broken after 100 ticks
    if (process_console_chunk(buf, nread, &state)) {
      kstep_tick_repeat(100);
      kstep_check_work_conserve();
      break;
    }
  }

  filp_close(sock, NULL);
}

KSTEP_DRIVER_DEFINE {
  .name = "executor",
  .setup = setup,
  .run = run,
  .on_tick_end = kstep_output_nr_running,
  .on_sched_balance_selected = kstep_check_extra_balance,
  .step_interval_us = 10000,
};
