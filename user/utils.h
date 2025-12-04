#include <sched.h>  // sched_setaffinity
#include <stdio.h>  // fprintf
#include <stdlib.h> // exit

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    exit(1);                                                                   \
  } while (0)

static void set_proc_affinity(int begin, int end) { // [begin, end]
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (int i = begin; i <= end; i++)
    CPU_SET(i, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
    panic("Failed to set CPU affinity to %d-%d", begin, end);
}
