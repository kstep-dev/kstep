#ifndef KSTEP_CHECKER_H
#define KSTEP_CHECKER_H

#include "internal.h"
#include "handler.h"

struct kstep_check_state {
  s64 cfs_util_avg[NR_CPUS];
  s64 rt_util_avg[NR_CPUS];
};

void kstep_check_before_op(struct kstep_check_state *check);
void kstep_check_after_op(struct kstep_check_state *check,
                          enum kstep_op_type type, int a, int b, int c);
void kstep_check_work_conserve(void);
void kstep_check_extra_balance(int cpu, struct sched_domain *sd);

#endif
