#ifndef KSTEP_EVENT_H
#define KSTEP_EVENT_H

#include <linux/sched.h>

struct sched_domain;

// What happens during a session, and how to watch it. Some events are traced in the kernel
// (trace.c), some are raised by kstep itself: the tick (tick.c), a command (the cli driver), the
// freeze command (kernel.c). A callback cannot tell the difference and does not have to.
//
// Register for an event and you are called on it. There is one registry, and everyone uses it --
// the driver, the shared-memory log, the checkers -- so nothing carries an event on behalf of
// anyone else, and adding a watcher is a line where the watching happens rather than a field in
// someone else's struct.
//
// Registering also arms: the kernel function behind a traced event is hooked when its first
// callback registers, so a session traces nothing that nobody watches. Register whenever you like
// -- in a driver's setup(), or mid-session as `check <name>` does when it enables a rule.
// Registering the same callback twice does nothing.
typedef void (*kstep_void_fn)(void);
typedef void (*kstep_select_task_rq_fn)(struct task_struct *p, int prev_cpu, int wake_flags);
typedef void (*kstep_balance_fn)(int cpu, struct sched_domain *sd);
typedef void (*kstep_task_migrate_fn)(struct task_struct *p, int src_cpu, int dst_cpu);
typedef void (*kstep_freeze_fn)(struct task_struct *p, unsigned int state_before);

// Traced in the kernel (trace.c)
void kstep_on_select_task_rq(kstep_select_task_rq_fn fn); // a wakeup is being placed, before it picks a CPU
void kstep_on_balance_selected(kstep_balance_fn fn);      // should_we_balance() has elected this cpu
void kstep_on_task_migrate(kstep_task_migrate_fn fn);     // a task moved to another CPU (set_task_cpu)

// Raised by kstep. The tick's callbacks run with the isolated CPUs held, as do a command's, so the
// runqueues can be read directly from either.
void kstep_on_tick_begin(kstep_void_fn fn);
void kstep_on_tick_end(kstep_void_fn fn);
void kstep_on_softirq_begin(kstep_void_fn fn); // around the SCHED_SOFTIRQ the tick drains synchronously
void kstep_on_softirq_end(kstep_void_fn fn);
void kstep_on_settle(kstep_void_fn fn);      // a step has settled: every test CPU has acted on what was asked
void kstep_on_freeze(kstep_freeze_fn fn);    // freeze_task() has been called on p, which it saw in state_before

// The lists behind the events. Raising one is a matter of calling everyone on it, so the emit is
// open-coded where it happens rather than hidden: kstep_emit(tick_end, kstep_void_fn).
#define KSTEP_EVENT_SUBS 8 // callbacks on one event
struct kstep_event {
  void *fn[KSTEP_EVENT_SUBS];
  int n; // published after the callback it counts, for a tracer running on another CPU
};
bool kstep_event_add(struct kstep_event *ev, void *fn); // true if it was not already there

#define kstep_emit(name, type, ...)                                                                                    \
  do {                                                                                                                 \
    struct kstep_event *_ev = &kstep_event_##name;                                                                     \
    int _n = smp_load_acquire(&_ev->n);                                                                                \
                                                                                                                       \
    for (int _i = 0; _i < _n; _i++)                                                                                    \
      ((type)_ev->fn[_i])(__VA_ARGS__);                                                                                \
  } while (0)

extern struct kstep_event kstep_event_tick_begin, kstep_event_tick_end, kstep_event_softirq_begin,
    kstep_event_softirq_end, kstep_event_settle, kstep_event_freeze;

#endif
