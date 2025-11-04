#include <linux/types.h>

// Cannot be larger than DELAY_CONST_MAX
#define SIM_INTERVAL_US (19000ULL)
#define TICK_INTERVAL_NS (1000ULL * 1000ULL)               // 1 ms
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

int sched_trace_init(void);
void sched_trace_exit(void);

void sched_clock_init(void);
void sched_clock_exit(void);
void sched_clock_set(u64 value);
void sched_clock_inc(u64 delta);

void cpu_controlled_mask_init(void);
