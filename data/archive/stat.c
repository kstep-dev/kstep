#define _GNU_SOURCE
#include <stdio.h>

int main() {
  FILE *f = fopen("/sys/kernel/debug/sched/debug", "r");
  if (!f) {
    perror("fopen");
    return 1;
  }

  char buf[4096];
  while (fgets(buf, sizeof(buf), f)) {
    fputs(buf, stdout);
  }

  fclose(f);
  return 0;
}
