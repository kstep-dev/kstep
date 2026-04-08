#include <linux/fs.h> // filp_open, filp_close
#include <linux/kernel.h> // printk
#include <linux/string.h> // strstr, strchr, strpbrk
#include <linux/types.h> // ssize_t
#include <linux/ctype.h> // isdigit or alpha
#include "driver.h"
#include "internal.h"
#include "op_handler.h"

#define MAX_LINE_LENGTH 1024
#define CHECKER_MSG_PREFIX "CHECKER,"
struct console_parse_state {
  char line_buf[MAX_LINE_LENGTH];
  size_t line_len;
};
static struct file *console;
static struct file *sock;

static void kstep_write_checker_status(void) {
  char buf[128];
  loff_t pos = 0;
  int len = scnprintf(buf, sizeof(buf), CHECKER_MSG_PREFIX "%d,%d,%d\n",
                      kstep_work_conserving_broken()? 1: 0, 
                      kstep_checker_result().cfs_util_avg_decay,
                      kstep_checker_result().rt_util_avg_decay);

  kernel_write(sock, buf, len, &pos);
}

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

  bool executed = kstep_execute_op(fields[0], fields[1], fields[2], fields[3]);
  kstep_write_state(sock, executed);
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

  kstep_topo_param_apply();
  kstep_freq_param_apply();
  kstep_cov_init();
}

static void run(void) {
  loff_t pos = 0;
  struct console_parse_state state = {};

  if (IS_ERR(sock))
    panic("Failed to open /dev/ttyS3");

  /* Signal to Python that the kmod is ready. The executed bit is ignored for
   * the initial handshake, so keep it non-zero to avoid an all-zero payload. */
  kstep_write_state(sock, true);

  while (true) {
    char buf[256];
    ssize_t nread = kernel_read(sock, buf, sizeof(buf), &pos);
    if (nread <= 0)
      continue;

    // Checker: whether work conserving broken after 100 ticks
    if (process_console_chunk(buf, nread, &state)) {
      kstep_tick_repeat(100);
      kstep_write_checker_status();
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
  .step_interval_us = 1000,
};
