#include "internal.h"

void kstep_disable_workqueue(void) {
  KSYM_IMPORT_TYPED(void, workqueue_offline_cpu);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    work_on_cpu(cpu, KSYM_workqueue_offline_cpu, (void *)(uintptr_t)cpu);
    TRACE_INFO("Disabled workqueue on CPU %d", cpu);
  }
}

// Prealloc kworkers for workqueue to avoid non-deterministic behavior.
static DECLARE_COMPLETION(dummy_start);
static DECLARE_COMPLETION(dummy_done);
static void dummy_work_fn(struct work_struct *work) {
  complete(&dummy_start);
  wait_for_completion(&dummy_done);
}

static void prealloc_kworker(struct workqueue_struct *wq, int num_kworkers) {
  reinit_completion(&dummy_start);
  reinit_completion(&dummy_done);
  int num_cpus = num_online_cpus();
  int num_works = num_kworkers * num_cpus;
  struct work_struct *dummy_works =
      kcalloc(num_works, sizeof(struct work_struct), GFP_KERNEL);
  for (int i = 0; i < num_works; i++) {
    INIT_WORK(&dummy_works[i], dummy_work_fn);
    int cpu = i / num_kworkers;
    queue_work_on(cpu, wq, &dummy_works[i]);
    wait_for_completion(&dummy_start);
  }
  complete_all(&dummy_done);
  for (int i = 0; i < num_works; i++) {
    flush_work(&dummy_works[i]);
  }

  kfree(dummy_works);
}

void kstep_prealloc_kworkers(void) {
  prealloc_kworker(system_wq, 2);
  prealloc_kworker(system_highpri_wq, 2);
  prealloc_kworker(system_unbound_wq, 2);
}

static const char *sys_kthread_names[] = {
    "cpuhp/",
    "migration/",
    "ksoftirqd/",
};

bool kstep_is_sys_kthread(struct task_struct *p) {
  for (int i = 0; i < ARRAY_SIZE(sys_kthread_names); i++)
    if (strstarts(p->comm, sys_kthread_names[i]))
      return true;
  return false;
}

void kstep_move_kthreads(void) {
  struct task_struct *p;
  for_each_process(p) {
    // Skip if 0 is the only allowed cpu
    if (cpumask_test_cpu(0, &p->cpus_mask) &&
        cpumask_weight(&p->cpus_mask) == 1) {
      continue;
    }
    // skip non-kthreads and sys kthreads
    if (!(p->flags & PF_KTHREAD) || kstep_is_sys_kthread(p))
      continue;

    set_cpus_allowed_ptr(p, cpumask_of(0));
    wake_up_process(p);
  }
  TRACE_INFO("Moved kthreads to CPU 0");
}
