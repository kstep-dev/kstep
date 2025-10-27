#include <linux/types.h>

int sched_trace_init(void);
void sched_trace_exit(void);

void sched_clock_init(void);
void sched_clock_exit(void);
void sched_clock_set(u64 value);
void sched_clock_inc(u64 delta);
