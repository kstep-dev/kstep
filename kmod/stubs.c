// Stub for symbols that are only present in instrumented kernels.
// When building against a stock kernel, these symbols are not available.
#include <linux/atomic.h>
#include <linux/export.h>

atomic_t __weak kstep_migrate_disable_pi_locked = ATOMIC_INIT(-1);
int __weak kstep_sd_doflags_offset_count;
