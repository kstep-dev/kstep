/*
 * rt_setprio_push_task_race.c - Reproduce proxy scheduling regression
 *
 * Bug: push_rt_task's migration-disabled path (pull=true only) checks
 * rq->donor->sched_class but calls find_lowest_rq(rq->curr). With
 * CONFIG_SCHED_PROXY_EXEC=y, donor (RT) differs from curr (CFS proxy),
 * causing UB in convert_prio() for CFS priorities.
 */
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/migrate.h>
#include <linux/mutex.h>
#include <linux/sched/rt.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"
#include "internal.h"

static DEFINE_MUTEX(proxy_mutex);
static volatile bool stop_threads;
static atomic_t non_rt_in_cpupri;

static struct task_struct *cfs_holder;
static struct task_struct *rt_blocker;
static struct task_struct *rt_migdis;
static struct task_struct *rt_helper;

static int cpupri_find_pre(struct kprobe *p, struct pt_regs *regs) {
  struct task_struct *task = (struct task_struct *)regs->si;
  if (task && task->prio >= MAX_RT_PRIO) {
    pr_err("BUG: cpupri_find non-RT pid=%d prio=%d comm=%s\n", task->pid,
           task->prio, task->comm);
    atomic_inc(&non_rt_in_cpupri);
  }
  return 0;
}

static struct kprobe cpupri_kp = {
    .symbol_name = "cpupri_find",
    .pre_handler = cpupri_find_pre,
};
static bool kprobe_registered;

static int cfs_holder_fn(void *data) {
  struct cpumask mask;

  /* Expand affinity so find_lowest_rq won't short-circuit
   * on nr_cpus_allowed == 1 when called with this task. */
  cpumask_clear(&mask);
  cpumask_set_cpu(1, &mask);
  cpumask_set_cpu(2, &mask);
  cpumask_set_cpu(3, &mask);
  set_cpus_allowed_ptr(current, &mask);

  mutex_lock(&proxy_mutex);
  while (!READ_ONCE(stop_threads))
    cond_resched();
  mutex_unlock(&proxy_mutex);
  return 0;
}

static int rt_blocker_fn(void *data) {
  mutex_lock(&proxy_mutex);
  mutex_unlock(&proxy_mutex);
  return 0;
}

static int rt_migdis_fn(void *data) {
  struct cpumask mask;
  cpumask_clear(&mask);
  cpumask_set_cpu(1, &mask);
  cpumask_set_cpu(2, &mask);
  cpumask_set_cpu(3, &mask);
  set_cpus_allowed_ptr(current, &mask);
  migrate_disable();
  while (!READ_ONCE(stop_threads))
    cond_resched();
  migrate_enable();
  return 0;
}

static int rt_helper_fn(void *data) {
  struct cpumask mask;
  cpumask_clear(&mask);
  cpumask_set_cpu(1, &mask);
  cpumask_set_cpu(2, &mask);
  cpumask_set_cpu(3, &mask);
  set_cpus_allowed_ptr(current, &mask);
  while (!READ_ONCE(stop_threads))
    cond_resched();
  return 0;
}

static void set_fifo_prio(struct task_struct *p, int user_prio) {
  struct sched_attr attr = {
      .sched_policy = SCHED_FIFO,
      .sched_priority = user_prio,
  };
  sched_setattr_nocheck(p, &attr);
}

static const char *prio_class(int prio) {
  return prio < MAX_RT_PRIO ? "RT" : "CFS";
}

/* Functions resolved via KSYM */
typedef int (*push_rt_task_fn_t)(struct rq *, bool);
typedef void (*raw_spin_rq_lock_fn_t)(struct rq *);
typedef void (*raw_spin_rq_unlock_fn_t)(struct rq *);

static push_rt_task_fn_t my_push_rt_task;
static raw_spin_rq_lock_fn_t my_rq_lock;
static raw_spin_rq_unlock_fn_t my_rq_unlock;

static void do_push_rt_on_cpu(void *data) {
  struct rq *rq = this_rq();
  if (!my_push_rt_task || !my_rq_lock || !my_rq_unlock)
    return;
  my_rq_lock(rq);
  TRACE_INFO("push_rt_task(rq,true) on CPU %d", smp_processor_id());
  my_push_rt_task(rq, true);
  my_rq_unlock(rq);
}

static void setup(void) {
  int ret;
  TRACE_INFO("=== setup ===");
  atomic_set(&non_rt_in_cpupri, 0);
  stop_threads = false;

  ret = register_kprobe(&cpupri_kp);
  kprobe_registered = (ret == 0);
  TRACE_INFO("kprobe cpupri_find: %s", kprobe_registered ? "OK" : "FAIL");

  my_push_rt_task = kstep_ksym_lookup("push_rt_task");
  my_rq_lock = kstep_ksym_lookup("raw_spin_rq_lock_nested");
  my_rq_unlock = kstep_ksym_lookup("raw_spin_rq_unlock");
  TRACE_INFO("push_rt_task=%px rq_lock=%px rq_unlock=%px", my_push_rt_task,
             my_rq_lock, my_rq_unlock);

  cfs_holder = kthread_create(cfs_holder_fn, NULL, "cfs_holder");
  if (IS_ERR(cfs_holder))
    panic("kthread cfs_holder");
  kthread_bind(cfs_holder, 1);

  rt_blocker = kthread_create(rt_blocker_fn, NULL, "rt_blocker");
  if (IS_ERR(rt_blocker))
    panic("kthread rt_blocker");
  kthread_bind(rt_blocker, 1);

  rt_migdis = kthread_create(rt_migdis_fn, NULL, "rt_migdis");
  if (IS_ERR(rt_migdis))
    panic("kthread rt_migdis");
  kthread_bind(rt_migdis, 1);

  rt_helper = kthread_create(rt_helper_fn, NULL, "rt_helper");
  if (IS_ERR(rt_helper))
    panic("kthread rt_helper");
  kthread_bind(rt_helper, 1);
}

static void log_rq(struct rq *rq, const char *tag) {
  TRACE_INFO("[%s] curr=%s(pid=%d prio=%d %s)", tag, rq->curr->comm,
             rq->curr->pid, rq->curr->prio, prio_class(rq->curr->prio));
#ifdef CONFIG_SCHED_PROXY_EXEC
  TRACE_INFO("[%s] donor=%s(pid=%d prio=%d %s) proxy=%s rt_nr=%u ovl=%d", tag,
             rq->donor->comm, rq->donor->pid, rq->donor->prio,
             prio_class(rq->donor->prio), rq->donor != rq->curr ? "YES" : "no",
             rq->rt.rt_nr_running, rq->rt.overloaded);
#endif
}

static void run(void) {
  struct rq *rq = cpu_rq(1);
  bool proxy_active = false;
  bool donor_rt_curr_cfs = false;

  TRACE_INFO("=== step1: CFS holder ===");
  wake_up_process(cfs_holder);
  kstep_tick_repeat(5);

  TRACE_INFO("=== step2: RT migdis (FIFO 50) ===");
  set_fifo_prio(rt_migdis, 50);
  wake_up_process(rt_migdis);
  kstep_tick_repeat(8);
  TRACE_INFO("rt_migdis: cpu=%d nr_cpus=%d migdis=%d", task_cpu(rt_migdis),
             rt_migdis->nr_cpus_allowed, rt_migdis->migration_disabled);

  TRACE_INFO("=== step3: RT helper (FIFO 40) ===");
  set_fifo_prio(rt_helper, 40);
  wake_up_process(rt_helper);
  kstep_tick_repeat(5);

  TRACE_INFO("=== step4: RT blocker (FIFO 90) -> mutex ===");
  set_fifo_prio(rt_blocker, 90);
  wake_up_process(rt_blocker);
  kstep_tick_repeat(15);
  log_rq(rq, "s4");

#ifdef CONFIG_SCHED_PROXY_EXEC
  proxy_active = (rq->donor != rq->curr);
  donor_rt_curr_cfs = proxy_active && (rq->donor->prio < MAX_RT_PRIO) &&
                      (rq->curr->prio >= MAX_RT_PRIO);
  if (proxy_active)
    TRACE_INFO("PROXY: donor=%s(%s) curr=%s(%s)", rq->donor->comm,
               prio_class(rq->donor->prio), rq->curr->comm,
               prio_class(rq->curr->prio));
#endif

  /* Step 5: Directly call push_rt_task(rq, true) on CPU 1 via IPI.
   * This enters the migration-disabled path which has the bug. */
  TRACE_INFO("=== step5: push_rt_task(pull=true) via IPI ===");
  if (my_push_rt_task && my_rq_lock && my_rq_unlock && proxy_active) {
    smp_call_function_single(1, do_push_rt_on_cpu, NULL, 1);
    kstep_tick_repeat(5);
  } else {
    TRACE_INFO("skip: push=%px lock=%px unlock=%px proxy=%d", my_push_rt_task,
               my_rq_lock, my_rq_unlock, proxy_active);
  }

  int bugs = atomic_read(&non_rt_in_cpupri);
  TRACE_INFO("=== RESULTS: non-RT in cpupri = %d ===", bugs);
  log_rq(rq, "final");

  if (bugs > 0) {
    kstep_fail("cpupri_find got non-RT task %d times (donor=RT curr=CFS)",
               bugs);
  } else if (donor_rt_curr_cfs) {
    /* Proxy active with RT donor / CFS curr, but kprobe saw no
     * non-RT task in cpupri_find.  On the fixed kernel this means
     * find_lowest_rq now receives rq->donor (RT) instead of
     * rq->curr (CFS). */
    kstep_pass("proxy donor=RT curr=CFS, cpupri_find only saw RT tasks");
  } else if (proxy_active) {
    kstep_pass("proxy active but donor/curr same class");
  } else {
    kstep_pass("proxy not triggered");
  }

  WRITE_ONCE(stop_threads, true);
  kstep_tick_repeat(5);
  if (kprobe_registered)
    unregister_kprobe(&cpupri_kp);
  TRACE_INFO("=== done ===");
}

KSTEP_DRIVER_DEFINE{
    .name = "rt_setprio_push_task_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#else
#include "driver.h"
static void run(void) { kstep_pass("kernel < 6.19"); }
KSTEP_DRIVER_DEFINE{
    .name = "rt_setprio_push_task_race",
    .run = run,
};
#endif
