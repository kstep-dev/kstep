#define _GNU_SOURCE

#include <signal.h>       // signal
#include <sys/prctl.h>    // PR_SET_NAME
#include <sys/resource.h> // setpriority
#include <unistd.h>       // sleep, exit, pause

#include "../kmod/user.h"
#include "utils.h"

static void handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  int val2 = info->si_pid;
  // int val3 = info->si_uid;
  if (code == SIGCODE_WAKEUP) {
    return; // do nothing
  } else if (code == SIGCODE_FORK) {
    for (int i = 0; i < val; i++) {
      int pid = fork();
      if (pid < 0)
        panic("fork failed at i == %d", i);
      if (pid == 0) // child process
        return;
    }
  } else if (code == SIGCODE_USLEEP) {
    usleep(val);
  } else if (code == SIGCODE_EXIT) {
    _exit(0);
  } else if (code == SIGCODE_PAUSE) {
    pause();
  } else if (code == SIGCODE_SET_PRIO) {
    if (setpriority(PRIO_PROCESS, 0, val) < 0)
      panic("setpriority failed");
  } else if (code == SIGCODE_PIN) {
    set_proc_affinity(val, val2);
  } else if (code == SIGCODE_FIFO) {
    struct sched_param sp = {.sched_priority = 80};
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
      panic("sched_setscheduler failed");
  } else {
    panic("Unknown signal code: %d\n", code);
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

int main() {
  struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
  sigaction(SIGUSR1, &sa, NULL);
  set_proc_affinity(1, sysconf(_SC_NPROCESSORS_ONLN) - 1);
  prctl(PR_SET_NAME, TASK_READY_COMM);
  pause();
  loop();
}
