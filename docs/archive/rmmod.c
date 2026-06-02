#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <module>\n", argv[0]);
    return 1;
  }
  int ret = syscall(SYS_delete_module, argv[1]);
  if (ret == -1) {
    perror("delete_module");
    return 1;
  }
  return 0;
}
