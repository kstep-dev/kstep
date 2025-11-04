#include "sigcode.h"

// Forward declarations
struct task_struct;
struct cpumask;

void send_sigcode(struct task_struct *p, enum sigcode code, int val);
struct task_struct *poll_task(const char *comm);
#if 0
struct task_struct *find_not_eligible_task(const char *comm,
                                           struct task_struct *skip_task);
#endif
void reset_task_stats(struct task_struct *p);
void print_tasks(void);

extern const struct cpumask *cpu_controlled_mask;
#define for_each_controlled_cpu(cpu) for_each_cpu(cpu, cpu_controlled_mask)

int is_sys_kthread(struct task_struct *p);
