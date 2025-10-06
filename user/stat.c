#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
  FILE *f = fopen("/sys/kernel/debug/sched/debug", "r");
  if (!f) {
    perror("fopen");
    return 1;
  }

  char buf[4096];
  int skipping = 0;
  while (fgets(buf, sizeof(buf), f)) {
    if (!skipping) {
      // Skip cpu#0
      if (strncmp(buf, "cpu#0", 5) == 0) {
        skipping = 1;
        continue;
      }
      fputs(buf, stdout);
    } else {
      // Only print cpu#1
      if (strncmp(buf, "cpu#1", 5) == 0) {
        skipping = 0;
        fputs(buf, stdout);
      }
    }
  }

  fclose(f);
  return 0;
}
