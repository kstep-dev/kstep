#include "checker.h"

// https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d
// freeze_task() freezes a sleeping task in place: a task in a freezable sleep is off the CPU and
// its state can be swapped to TASK_FROZEN at once; only a running task has to freeze itself when
// it next returns to user space. The freezer took "still on the runqueue" for running, which with
// delayed dequeue includes sleepers whose dequeue is pending, and woke them with a fake signal
// instead. The rule: a freezable sleeper handed to freeze_task() while the freezer is active is
// frozen when it returns.
//
// The freeze command calls freeze_task() itself (kstep_freeze_task), so the rule reads the state
// it passed in and the state it left behind -- nothing has to be intercepted.
static void on_freeze(struct task_struct *p, unsigned int state_before) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  bool sleeper = (state_before & TASK_FREEZABLE) && !(state_before & TASK_FROZEN);

  if (sleeper && !(READ_ONCE(p->__state) & TASK_FROZEN))
    kstep_warn("frozen", "task %d was a freezable sleeper (state %#x) but freeze_task() left it at %#x", p->pid,
               state_before, READ_ONCE(p->__state));
#endif
}

void kstep_check_frozen_enable(void) { kstep_on_freeze(on_freeze); }
