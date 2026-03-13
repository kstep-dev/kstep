#define _GNU_SOURCE

#include <signal.h>       // sigaction
#include <sys/prctl.h>    // PR_SET_NAME
#include <time.h>         // nanosleep, struct timespec
#include <unistd.h>       // fork, pause, _exit

#include "../kmod/user.h"
#include "utils.h"

static void handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  // int val2 = info->si_pid;
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
  } else if (code == SIGCODE_EXIT) {
    _exit(0);
  } else if (code == SIGCODE_PAUSE) {
    pause();
  } else if (code == SIGCODE_BLOCK) {
    struct timespec ts = {.tv_sec = __INT_MAX__};
    nanosleep(&ts, NULL);
  } else {
    panic("Unknown signal code: %d\n", code);
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

int main(int argc, char **argv) {
  struct sigaction sa = {.sa_sigaction = handler,
                         .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigaction(SIGUSR1, &sa, NULL);
  set_proc_affinity(1, sysconf(_SC_NPROCESSORS_ONLN) - 1);
  prctl(PR_SET_NAME, TASK_READY_COMM);

  pause();
  loop();
}
