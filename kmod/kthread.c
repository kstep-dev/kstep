#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include "internal.h"

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

  bool allocated;
};

#define MAX_KTHREADS 16
static struct kstep_kthread pool[MAX_KTHREADS];

static struct kstep_kthread *kt_alloc(void) {
  for (int i = 0; i < MAX_KTHREADS; i++) {
    if (!pool[i].allocated) {
      pool[i].allocated = true;
      return &pool[i];
    }
  }
  panic("kstep: kthread pool exhausted (max %d)", MAX_KTHREADS);
  return NULL;
}

static struct kstep_kthread *kt_find(struct task_struct *p) {
  for (int i = 0; i < MAX_KTHREADS; i++)
    if (pool[i].allocated && pool[i].task == p)
      return &pool[i];
  panic("kstep: task %d (%s) is not a managed kthread", p->pid, p->comm);
  return NULL;
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
  atomic_set(&kt->wq_ready, 0);
  // Wait for the wq to be woken up and wq_ready to be set to 1
  wait_event(kt->wq, atomic_read(&kt->wq_ready) != 0);

  kt->action = do_spin; // After the wq is woken up, the kthread continues to spin
  atomic_set(&kt->action_updated, 1);
}

static void do_syncwakeup(struct kstep_kthread *kt, void *arg) {
  __wake_up_sync(arg, TASK_NORMAL);
  kt->action = NULL; // After the sync wakeup, the parent kthread should exit
  atomic_set(&kt->action_updated, 1);
}

static void do_wake_continue(struct kstep_kthread *kt, void *arg) {
  __wake_up_sync(arg, TASK_NORMAL);
  kt->action = do_spin;
  atomic_set(&kt->action_updated, 1);
}

/* ---- public API ---- */

struct task_struct *kstep_kthread_create(const char *name) {
  struct kstep_kthread *kt = kt_alloc();
  init_waitqueue_head(&kt->wq);
  kt->action = do_spin;
  atomic_set(&kt->action_updated, 0);

  kt->task = kthread_create(kstep_kthread_fn, kt, "%s", name);

  if (IS_ERR(kt->task))
    panic("kstep: failed to create spinner kthread '%s'", name);

  return kt->task;
}

/* Set the allowed CPU mask (can be called before kstep_kthread_start). */
void kstep_kthread_bind(struct task_struct *p, const struct cpumask *mask) {
  set_cpus_allowed_ptr(p, mask);
}

/* Start a kthread created by kstep_kthread_sleeper(). */
void kstep_kthread_start(struct task_struct *p) {
  wake_up_process(p);
}

/* Make the kthread yield */
void kstep_kthread_yield(struct task_struct *p) {
  struct kstep_kthread *kt = kt_find(p);
  kt->action = do_yield;
  atomic_set(&kt->action_updated, 1);
}

/*
 * Create a kthread that blocks on an internal wait queue until
 * kstep_kthread_syncwake() wakes it, then spins.
 */
 void kstep_kthread_block(struct task_struct *p) {
  struct kstep_kthread *kt = kt_find(p);
  kt->action = do_block_on_wq;
  atomic_set(&kt->action_updated, 1);
}

/*
 * Trigger waker to call __wake_up_sync() targeting wakee's
 * internal wait queue from waker's pinned CPU.
 */
void kstep_kthread_syncwake(struct task_struct *waker, struct task_struct *wakee) {
  struct kstep_kthread *waker_kt = kt_find(waker);
  struct kstep_kthread *wakee_kt = kt_find(wakee);

  if ((waker_kt->action != do_spin && waker_kt->action != do_yield) || 
      wakee_kt->action != do_block_on_wq)
    panic("kstep: waker or wakee is not in the correct state");

  // satisfy wakee's wait_event condition before the wakeup signal arrives
  atomic_set(&wakee_kt->wq_ready, 1);

  // Set the action to do_syncwakeup
  waker_kt->action = do_syncwakeup;
  waker_kt->action_arg = &wakee_kt->wq;
  atomic_set(&waker_kt->action_updated, 1);
}

/*
 * Like kstep_kthread_syncwake but the waker continues spinning
 * instead of exiting after the wakeup.
 */
void kstep_kthread_wake_continue(struct task_struct *waker, struct task_struct *wakee) {
  struct kstep_kthread *waker_kt = kt_find(waker);
  struct kstep_kthread *wakee_kt = kt_find(wakee);

  if ((waker_kt->action != do_spin && waker_kt->action != do_yield) ||
      wakee_kt->action != do_block_on_wq)
    panic("kstep: waker or wakee is not in the correct state");

  atomic_set(&wakee_kt->wq_ready, 1);

  waker_kt->action = do_wake_continue;
  waker_kt->action_arg = &wakee_kt->wq;
  atomic_set(&waker_kt->action_updated, 1);
}
