#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

static void sigusr1_handler(int signum) { fork(); }

static void set_proc_affinity(int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

int worker() {
  set_proc_affinity(1);
  while (1) {
    __asm__("" : : : "memory");
  }
}

int main() {
  signal(SIGUSR1, sigusr1_handler);
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
