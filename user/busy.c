#define _GNU_SOURCE

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#define NUM_PROCS 2

void child(int proc_idx) {
  // Set process name
  char name[16];
  snprintf(name, sizeof(name), "test-proc-%d", proc_idx);
  prctl(PR_SET_NAME, name);

  // Set affinity
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(1, &cpuset);
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }

  printf("Test process `%s` (pid %d) running on CPU %d\n", name, getpid(),
         sched_getcpu());

  while (1) {
    __asm__("" : : : "memory");
  }
}

int main() {
  for (int i = 0; i < NUM_PROCS; ++i) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    } else if (pid == 0) {
      // Child process
      child(i);
      exit(0);
    }
  }

  return 0;
}
