#define _GNU_SOURCE

#include <limits.h>    // INT_MAX
#include <signal.h>    // sigaction
#include <sys/prctl.h> // PR_SET_NAME
#include <time.h>      // nanosleep, struct timespec
#include <unistd.h>    // fork, pause, _exit

#include "../kmod/user.h"
#include "utils.h"

static void do_fork(int n) {
  for (int i = 0; i < n; i++) {
    int pid = fork();
    if (pid < 0)
      panic("fork failed at i == %d", i);
    if (pid == 0) // child process
      return;
  }
}

static void handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  if (code == SIGCODE_WAKEUP)
    return;
  else if (code == SIGCODE_FORK)
    do_fork(val);
  else if (code == SIGCODE_EXIT)
    _exit(0);
  else if (code == SIGCODE_PAUSE)
    pause();
  else if (code == SIGCODE_BLOCK)
    nanosleep(&(struct timespec){.tv_sec = INT_MAX}, NULL);
  else
    panic("Unknown signal code: %d\n", code);
}

__attribute__((noreturn)) static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

int task_main(int argc, char **argv) {
  struct sigaction sa = {.sa_sigaction = handler,
                         .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigaction(SIGUSR1, &sa, NULL);
  prctl(PR_SET_NAME, TASK_READY_COMM);

  pause();
  loop();
}
