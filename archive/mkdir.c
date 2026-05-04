#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: mkdir <directory>\n");
    return 1;
  }
  if (mkdir(argv[1], 0777) != 0) {
    perror("mkdir");
    return 1;
  }
  return 0;
}
