// https://github.com/torvalds/linux/commit/3ebb1b652239
// Bug: preempt_model_str() returns "PREEMPT(undef)" when
// preempt_dynamic_mode == 0 (none) due to off-by-one (> vs >=).

#include <linux/version.h>
#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)

static void setup(void) {}

static void run(void) {
  int *mode_ptr = kstep_ksym_lookup("preempt_dynamic_mode");
  const char *(*model_str_fn)(void) =
      kstep_ksym_lookup("preempt_model_str");

  if (!mode_ptr || !model_str_fn) {
    TRACE_INFO("Cannot find required symbols");
    kstep_fail("symbols not found");
    return;
  }

  int orig_mode = *mode_ptr;
  TRACE_INFO("Original preempt_dynamic_mode = %d", orig_mode);

  // Set mode to 0 (preempt_dynamic_none)
  *mode_ptr = 0;
  const char *result = model_str_fn();
  TRACE_INFO("preempt_dynamic_mode=0 -> preempt_model_str() = \"%s\"", result);

  if (strstr(result, "undef")) {
    kstep_fail("got undef for mode=0 (expected none)");
  } else if (strstr(result, "none")) {
    kstep_pass("correctly got none for mode=0");
  } else {
    kstep_fail("unexpected output: %s", result);
  }

  // Restore original mode
  *mode_ptr = orig_mode;
}

KSTEP_DRIVER_DEFINE{
    .name = "preempt_str",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup2(void) {}
static void run2(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "preempt_str", .setup = setup2, .run = run2};
#endif
