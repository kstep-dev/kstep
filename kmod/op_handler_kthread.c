#include <linux/kernel.h>

#include "linux/cpumask.h"
#include "op_handler_internal.h"

enum kstep_kthread_state kstep_op_kthread_state(int id) {
  if (!kstep_op_is_valid_kthread_id(id) || !kstep_kthreads[id].p)
    return KSTEP_KTHREAD_DEAD;
  return kstep_kthread_get_state(kstep_kthreads[id].p);
}

u8 kstep_op_kthread_create(int a, int b, int c) {
  char name[16];

  (void)b;
  (void)c;
  if (!kstep_op_is_valid_kthread_id(a) || kstep_kthreads[a].p)
    return 0;

  scnprintf(name, sizeof(name), "kt%d", a);
  kstep_kthreads[a].p = kstep_kthread_create(name);
  return 1;
}

u8 kstep_op_kthread_bind(int a, int b, int c) {
  struct cpumask mask;
  enum kstep_kthread_state state = kstep_op_kthread_state(a);

  if (!kstep_op_is_valid_kthread_id(a) || !kstep_kthreads[a].p ||
      state == KSTEP_KTHREAD_DEAD)
    return 0;
  if (b > c || b < 1 || c > num_online_cpus() - 1)
    return 0;

  cpumask_clear(&mask);
  for (int cpu = b; cpu <= c; cpu++)
    cpumask_set_cpu(cpu, &mask);
  kstep_kthread_bind(kstep_kthreads[a].p, &mask);
  return 1;
}

u8 kstep_op_kthread_start(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  if (kstep_op_kthread_state(a) != KSTEP_KTHREAD_CREATED)
    return 0;
  kstep_kthread_start(kstep_kthreads[a].p);
  return 1;
}

u8 kstep_op_kthread_yield(int a, int b, int c) {
  enum kstep_kthread_state state;

  (void)b;
  (void)c;
  if (!kstep_op_is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  state = kstep_op_kthread_state(a);
  if (state != KSTEP_KTHREAD_SPIN && state != KSTEP_KTHREAD_YIELD)
    return 0;
  kstep_kthread_yield(kstep_kthreads[a].p);
  return 1;
}

u8 kstep_op_kthread_block(int a, int b, int c) {
  enum kstep_kthread_state state;

  (void)b;
  (void)c;
  if (!kstep_op_is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  state = kstep_op_kthread_state(a);
  if (state != KSTEP_KTHREAD_SPIN && state != KSTEP_KTHREAD_YIELD)
    return 0;
  kstep_kthread_block(kstep_kthreads[a].p);
  return 1;
}

u8 kstep_op_kthread_syncwake(int a, int b, int c) {
  enum kstep_kthread_state waker_state;
  enum kstep_kthread_state wakee_state;

  (void)c;
  if (!kstep_op_is_valid_kthread_id(a) || !kstep_op_is_valid_kthread_id(b))
    return 0;
  if (a == b)
    return 0;

  waker_state = kstep_op_kthread_state(a);
  wakee_state = kstep_op_kthread_state(b);
  if (!kstep_kthreads[a].p || !kstep_kthreads[b].p)
    return 0;
  if (waker_state != KSTEP_KTHREAD_SPIN &&
      waker_state != KSTEP_KTHREAD_YIELD)
    return 0;
  if (wakee_state != KSTEP_KTHREAD_BLOCKED)
    return 0;

  kstep_kthread_syncwake(kstep_kthreads[a].p, kstep_kthreads[b].p);
  return 1;
}
