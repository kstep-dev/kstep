#include <linux/types.h>

#define SIM_INTERVAL_MS 100
#define TICK_INTERVAL_NS (1000ULL * 1000ULL)               // 1 ms
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// Forward declarations
struct task_struct;
enum sigcode;
struct cpumask;

int sched_trace_init(void);
void sched_trace_exit(void);

void sched_clock_init(void);
void sched_clock_exit(void);
void sched_clock_set(u64 value);
void sched_clock_inc(u64 delta);

void send_sigcode(struct task_struct *p, enum sigcode code, int val);
struct task_struct *poll_task(const char *comm);

extern const struct cpumask *cpu_controlled_mask;
void cpu_controlled_mask_init(void);
