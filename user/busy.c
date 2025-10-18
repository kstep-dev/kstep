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

static void set_proc_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  int nproc = sysconf(_SC_NPROCESSORS_ONLN);
  for (int i = 1; i < nproc; i++) { // skip cpu 0
    CPU_SET(i, &cpuset);
  }
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
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
    set_proc_affinity();
    loop();
    exit(0);
  }
  return 0;
}
