// Reproducer for setup_resched_latency_warn_ms() returning 1 on parse failure.
// The __setup convention is: return 1 = handled, return 0 = not handled.
// setup_resched_latency_warn_ms() returns 1 even when kstrtol() fails,
// incorrectly telling the kernel the parameter was handled, suppressing
// the "Unknown kernel command line parameters" warning.
//
// Test: boot with "resched_latency_warn_ms=notanumber" on the command line.
// On buggy kernel: __setup returns 1 on failure -> param "handled" -> not
//   passed to init's envp -> module param stays NULL -> kstep_fail
// On fixed kernel: __setup returns 0 on failure -> param "unhandled" ->
//   passed to init's envp -> init passes it to module -> kstep_pass

#include <linux/version.h>
#include <linux/moduleparam.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

// Module parameter: on the fixed kernel, the invalid boot parameter
// "resched_latency_warn_ms=notanumber" will be passed through to the module
// because __setup returns 0 (not handled). On the buggy kernel it stays NULL.
static char *resched_latency_warn_ms_val = NULL;
module_param_named(resched_latency_warn_ms, resched_latency_warn_ms_val, charp, 0);

KSYM_IMPORT_TYPED(int, sysctl_resched_latency_warn_ms);

static void setup(void) {}

static void run(void) {
int sysctl_val = *KSYM_sysctl_resched_latency_warn_ms;

TRACE_INFO("sysctl_resched_latency_warn_ms = %d (default=100)", sysctl_val);
TRACE_INFO("module param resched_latency_warn_ms = %s",
   resched_latency_warn_ms_val ? resched_latency_warn_ms_val : "(null)");

if (sysctl_val != 100) {
kstep_fail("sysctl_resched_latency_warn_ms = %d, expected 100", sysctl_val);
return;
}

if (resched_latency_warn_ms_val == NULL) {
// The parameter was "handled" by __setup (returned 1) even though
// parsing failed. This is the bug.
kstep_fail("__setup returned 1 on parse failure: param silently swallowed");
} else {
// The parameter was correctly reported as "not handled" (returned 0).
kstep_pass("__setup returned 0 on parse failure: param forwarded (val=%s)",
   resched_latency_warn_ms_val);
}
}

KSTEP_DRIVER_DEFINE{
.name = "core_preempt_dynamic_return_value",
.setup = setup,
.run = run,
.step_interval_us = 1000,
};
#endif
