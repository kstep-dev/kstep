#include <linux/sched.h>
#include <linux/sched/signal.h>

#include "kstep.h"

void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (val1=%d, val2=%d, val3=%d) to pid %d",
             sigcode_to_str[code], val1, val2, val3, p->pid);
  kstep_sleep();
  yield(); // yield to let the task (e.g. busy during its init, cgroup
           // controller uthread) run
}

struct task_struct *poll_task(const char *comm) {
  struct task_struct *p;
  for (int i = 0; i < 1000; i++) {
    for_each_process(p) {
      if (strcmp(p->comm, comm) == 0)
        return p;
    }
    kstep_sleep();
    yield(); // busy might be blocked by the busy controller, yield to let it
             // run
    TRACE_INFO("Waiting for process %s to be created", comm);
  }
  panic("Failed to find process %s", comm);
}

static char *sys_kthread_comms[] = {
    "cpuhp/",
    "migration/",
    "ksoftirqd/",
};

int is_sys_kthread(struct task_struct *p) {
  for (int i = 0; i < ARRAY_SIZE(sys_kthread_comms); i++) {
    if (strstarts(p->comm, sys_kthread_comms[i]))
      return 1;
  }
  return 0;
}
