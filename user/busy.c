#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "../kmod/sigcode.h"

static void signal_handler(int signum, siginfo_t *info, void *context) {
  switch (info->si_code) {
  case SIGCODE_FORK:
    fork();
    break;
  case SIGCODE_SLEEP:
    sleep(info->si_int);
    break;
  case SIGCODE_EXIT:
    exit(0);
    break;
  case SIGCODE_PAUSE:
    pause();
    break;
  default:
    printf("Unknown signal code: %d\n", info->si_code);
    break;
  }
}

static void set_proc_affinity(int cpus[], size_t size) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (size_t i = 0; i < size; i++) {
    CPU_SET(cpus[i], &cpuset);
  }
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

void worker() {
  set_proc_affinity((int[]){1, 2}, 2);
  while (1) {
    __asm__("" : : : "memory");
  }
}

int main() {
  struct sigaction sa = {.sa_sigaction = signal_handler,
                         .sa_flags = SA_SIGINFO};
  sigaction(SIGUSR1, &sa, NULL);
  prctl(PR_SET_NAME, "test-proc");
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  } else if (pid == 0) {
    // Child process
    worker();
    exit(0);
  }
  return 0;
}
