#define _GNU_SOURCE
#include <linux/module.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <file> [param1=value1] [param2=value2] ...\n",
            argv[0]);
    return 1;
  }

  int fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    perror("open");
    return 1;
  }

  struct stat sb;
  if (fstat(fd, &sb) == -1) {
    perror("Error getting file size");
    close(fd);
    return 1;
  }
  off_t file_size = sb.st_size;

  void *addr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return 1;
  }
  close(fd);

  char params[1024] = {};
  int params_len = 0;
  for (int i = 2; i < argc; i++) {
    strncat(params + params_len, argv[i], sizeof(params) - params_len);
    params_len += strlen(argv[i]);
    strncat(params + params_len, " ", sizeof(params) - params_len);
    params_len++;
  }

  int ret = syscall(SYS_init_module, addr, file_size, params);
  if (ret == -1) {
    perror("init_module");
    munmap(addr, file_size);
    return 1;
  }
  munmap(addr, file_size);
  return 0;
}
