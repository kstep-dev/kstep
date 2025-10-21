#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "../kmod/sigcode.h"

static void signal_handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  if (code == SIGCODE_FORK) {
    for (int i = 0; i < val; i++) {
      int pid = fork();
      if (pid == 0)
        return;
    }
  } else if (code == SIGCODE_SLEEP) {
    sleep(val);
  } else if (code == SIGCODE_EXIT) {
    exit(0);
  } else if (code == SIGCODE_PAUSE) {
    pause();
  } else {
    printf("Unknown signal code: %d\n", code);
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
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  } else if (pid == 0) {
    // Child process
    set_proc_affinity();
    prctl(PR_SET_NAME, "test-proc");
    pause();
    loop();
    exit(0);
  }
  return 0;
}
