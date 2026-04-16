#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include "internal.h"
#include "linux/sched.h"

struct kstep_kthread {
  struct task_struct     *task;

  // The action is the function to be executed by the kthread
  // The action is set to do_spin by default
  // Use the kstep interfaces to change the action
  void            (*action)(struct kstep_kthread *kt, void *arg);
  void            *action_arg;
  atomic_t        action_updated;

  /* The wq and wq_ready is used to pause the kthread */
  wait_queue_head_t wq;
  atomic_t          wq_ready;

  enum kstep_kthread_state state;
  bool allocated;
};

static struct kstep_kthread pool[KSTEP_MAX_KTHREADS];

static struct kstep_kthread *kt_alloc(void) {
  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++) {
    if (!pool[i].allocated) {
      pool[i].allocated = true;
      return &pool[i];
    }
  }
  panic("kstep: kthread pool exhausted (max %d)", KSTEP_MAX_KTHREADS);
  return NULL;
}

static struct kstep_kthread *kt_find(struct task_struct *p) {
  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++)
    if (pool[i].allocated && pool[i].task == p)
      return &pool[i];
  panic("kstep: task %d (%s) is not a managed kthread", p->pid, p->comm);
  return NULL;
}

enum kstep_kthread_state kstep_kthread_get_state(struct task_struct *p) {
  if (!p)
    return KSTEP_KTHREAD_DEAD;

  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++)
    if (pool[i].allocated && pool[i].task == p)
      return pool[i].state;
  return KSTEP_KTHREAD_DEAD;
}

/* ---- thread function ---- */

static int kstep_kthread_fn(void *data) {
  struct kstep_kthread *kt = data;

  while (1) {
    // Continue to execute the action until the action is updated
    while (atomic_read(&kt->action_updated) == 0)
      kt->action(kt, kt->action_arg);
    atomic_set(&kt->action_updated, 0);
    // If the action is NULL, break the loop
    if (kt->action == NULL)
      break;
  }

  // Free the kstep_kthread
  kt->state = KSTEP_KTHREAD_DEAD;
  kt->task = NULL;
  kt->action = NULL;
  kt->action_arg = NULL;
  kt->allocated = false;
  return 0;
}

/* ---- action functions ---- */

static void do_spin(struct kstep_kthread *kt, void *arg) {
  __asm__("" : : : "memory");
}

static void do_yield(struct kstep_kthread *kt, void *arg) {
  yield();
}

static void do_block_on_wq(struct kstep_kthread *kt, void *arg) {
  kt->state = KSTEP_KTHREAD_BLOCKED;
  atomic_set(&kt->wq_ready, 0);
  // Wait for the wq to be woken up and wq_ready to be set to 1
  wait_event(kt->wq, atomic_read(&kt->wq_ready) != 0);

  kt->action = do_spin; // After the wq is woken up, the kthread continues to spin
  kt->state = KSTEP_KTHREAD_SPIN;
  atomic_set(&kt->action_updated, 1);
}

static void do_syncwakeup(struct kstep_kthread *kt, void *arg) {
  __wake_up_sync(arg, TASK_NORMAL);

  struct task_struct *p;
  struct rq * rq = cpu_rq(task_cpu(kt->task));
  int runnable_task = 0;

  for_each_process(p) {
    if (task_cpu(p) == task_cpu(kt->task) && p->__state == TASK_RUNNING) {
      runnable_task ++;
    }
  }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  if (runnable_task == 1)
    pr_info("warn: sync wakeup pick the remote cpu while local cpu will become idle queued_task=%d, delayed_task=%d", 
            rq->nr_running, rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable);
#else
  if (runnable_task == 1)
    pr_info("warn: sync wakeup pick the remote cpu while local cpu will become idle queued_task=%d", 
            rq->nr_running);
#endif

  kt->action = NULL; // After the sync wakeup, the parent kthread should exit
  kt->state = KSTEP_KTHREAD_DEAD;
  atomic_set(&kt->action_updated, 1);
}

/* ---- public API ---- */

struct task_struct *kstep_kthread_create(const char *name) {
  struct kstep_kthread *kt = kt_alloc();
  struct cpumask mask;
  init_waitqueue_head(&kt->wq);
  kt->action = do_spin;
  kt->action_arg = NULL;
  atomic_set(&kt->action_updated, 0);
  atomic_set(&kt->wq_ready, 0);
  kt->state = KSTEP_KTHREAD_CREATED;

  kt->task = kthread_create(kstep_kthread_fn, kt, "%s", name);

  if (IS_ERR(kt->task))
    panic("kstep: failed to create spinner kthread '%s'", name);

  cpumask_clear(&mask);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    cpumask_set_cpu(cpu, &mask);

  set_cpus_allowed_ptr(kt->task, &mask);

  TRACE_INFO("kthread_create: name=%s pid=%d", name, kt->task->pid);
  kstep_sleep();
  return kt->task;
}

/* Set the allowed CPU mask (can be called before kstep_kthread_start). */
void kstep_kthread_bind(struct task_struct *p, const struct cpumask *mask) {
  TRACE_INFO("kthread_bind: pid=%d mask=%*pbl", p->pid, cpumask_pr_args(mask));
  set_cpus_allowed_ptr(p, mask);
  kstep_sleep();
}

/* Start a kthread created by kstep_kthread_sleeper(). */
void kstep_kthread_start(struct task_struct *p) {
  struct kstep_kthread *kt = kt_find(p);
  TRACE_INFO("kthread_start: pid=%d", p->pid);
  kt->state = KSTEP_KTHREAD_SPIN;
  wake_up_process(p);
  kstep_sleep();
}

/* Make the kthread yield */
void kstep_kthread_yield(struct task_struct *p) {
  struct kstep_kthread *kt = kt_find(p);
  TRACE_INFO("kthread_yield: pid=%d", p->pid);
  kt->action = do_yield;
  kt->state = KSTEP_KTHREAD_YIELD;
  atomic_set(&kt->action_updated, 1);
  printk("here");
  kstep_sleep();
}

/*
 * Create a kthread that blocks on an internal wait queue until
 * kstep_kthread_syncwake() wakes it, then spins.
 */
 void kstep_kthread_block(struct task_struct *p) {
  struct kstep_kthread *kt = kt_find(p);
  TRACE_INFO("kthread_block: pid=%d", p->pid);
  kt->action = do_block_on_wq;
  kt->state = KSTEP_KTHREAD_BLOCK_REQUESTED;
  atomic_set(&kt->action_updated, 1);
  kstep_sleep();
}

/*
 * Trigger waker to call __wake_up_sync() targeting wakee's
 * internal wait queue from waker's pinned CPU.
 */
void kstep_kthread_syncwake(struct task_struct *waker, struct task_struct *wakee) {
  struct kstep_kthread *waker_kt = kt_find(waker);
  struct kstep_kthread *wakee_kt = kt_find(wakee);
  struct cpumask mask;

  if ((waker_kt->action != do_spin && waker_kt->action != do_yield) || 
      wakee_kt->action != do_block_on_wq)
    panic("kstep: waker or wakee is not in the correct state");
  
  cpumask_clear(&mask);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    cpumask_set_cpu(cpu, &mask);

  set_cpus_allowed_ptr(wakee, & mask);
  kstep_sleep();

  TRACE_INFO("kthread_syncwake: waker_pid=%d wakee_pid=%d", waker->pid, wakee->pid);
  // satisfy wakee's wait_event condition before the wakeup signal arrives
  atomic_set(&wakee_kt->wq_ready, 1);

  // Set the action to do_syncwakeup
  waker_kt->action = do_syncwakeup;
  waker_kt->action_arg = &wakee_kt->wq;
  waker_kt->state = KSTEP_KTHREAD_SYNCWAKE_REQUESTED;
  atomic_set(&waker_kt->action_updated, 1);
  kstep_sleep();
  
}
