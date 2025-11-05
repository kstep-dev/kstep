#include "sigcode.h"

// Forward declarations
struct task_struct;
struct cpumask;

#define send_sigcode(p, code, val) send_sigcode3(p, code, val, 0, 0)
#define send_sigcode2(p, code, val1, val2) send_sigcode3(p, code, val1, val2, 0)
void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3);
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
