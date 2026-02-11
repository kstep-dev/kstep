// https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5

#include <linux/fs.h>

#include "driver.h"

// static struct kstep_op kstep_seq[] = {
//   { TICK, 0, 0, 0 },
// };
// static const int kstep_seq_len = 1;

static void setup(void) {
  struct file *console = filp_open("/dev/console", O_RDONLY, 0);
  if (IS_ERR(console))
    panic("Failed to open /dev/console");

  char buf[1024];
  ssize_t n = kernel_read(console, buf, sizeof(buf) - 1, NULL);
  if (n < 0)
    panic("Failed to read /dev/console: %zd", n);
  buf[n] = '\0';
  printk("test_spec: %s", buf);
  filp_close(console, NULL);
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
