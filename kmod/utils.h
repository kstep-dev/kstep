#include "sigcode.h"

// Forward declarations
struct task_struct;
struct cpumask;

void send_sigcode(struct task_struct *p, enum sigcode code, int val);
struct task_struct *poll_task(const char *comm);
struct task_struct *find_not_eligible_task(const char *comm,
                                           struct task_struct *skip_task);
void reset_task_stats(struct task_struct *p);

extern const struct cpumask *cpu_controlled_mask;
void cpu_controlled_mask_init(void);
