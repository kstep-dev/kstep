#define _GNU_SOURCE

#include <fcntl.h>      // open
#include <linux/kcov.h> // KCOV ioctls
#include <stdint.h>     // uint64_t
#include <stdio.h>      // dprintf
#include <stdlib.h>     // qsort
#include <sys/ioctl.h>  // ioctl
#include <sys/mman.h>   // mmap
#include <unistd.h>     // close

#include "utils.h"

#define KCOV_DEVICE "/sys/kernel/debug/kcov"
#define KCOV_ENTRIES (1u << 17)

static int kcov_fd = -1;
static unsigned long *kcov_area = NULL;
static size_t kcov_words = 0;

static int cmp_ulong(const void *a, const void *b) {
  unsigned long av = *(const unsigned long *)a;
  unsigned long bv = *(const unsigned long *)b;
  if (av < bv)
    return -1;
  if (av > bv)
    return 1;
  return 0;
}

void kcov_init_task(void) {
  kcov_fd = open(KCOV_DEVICE, O_RDWR);
  if (kcov_fd < 0)
    panic("Failed to open %s", KCOV_DEVICE);

  kcov_words = KCOV_ENTRIES;
  if (ioctl(kcov_fd, KCOV_INIT_TRACE, kcov_words))
    panic("KCOV_INIT_TRACE failed");

  size_t map_bytes = kcov_words * sizeof(unsigned long);
  kcov_area =
      mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, kcov_fd, 0);
  if (kcov_area == MAP_FAILED)
    panic("mmap(kcov) failed");
}

void kcov_start(void) {
  __atomic_store_n(&kcov_area[0], 0ul, __ATOMIC_RELAXED);
  if (ioctl(kcov_fd, KCOV_ENABLE, KCOV_TRACE_PC))
    panic("KCOV_ENABLE failed");
}

void kcov_stop(void) {
  if (ioctl(kcov_fd, KCOV_DISABLE, 0))
    panic("KCOV_DISABLE failed");
}

void kcov_dump(void) {
  uint64_t n = __atomic_load_n(&kcov_area[0], __ATOMIC_RELAXED);
  if (kcov_words <= 1)
    n = 0;
  else if (n > kcov_words - 1)
    n = kcov_words - 1;

  if (n > 1)
    qsort(&kcov_area[1], n, sizeof(kcov_area[1]), cmp_ulong);

  uint64_t unique = 0;
  for (uint64_t i = 0; i < n; i++) {
    if (i == 0 || kcov_area[i + 1] != kcov_area[i])
      kcov_area[1 + unique++] = kcov_area[i + 1];
  }

  char dump_path[64];
  snprintf(dump_path, sizeof(dump_path), "/kstep_kcov_%d.txt", getpid());

  int out = open(dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    panic("Failed to open %s", dump_path);

  for (uint64_t i = 0; i < unique; i++)
    dprintf(out, "%lx\n", kcov_area[i + 1]);

  dprintf(out, "END\n");
  
  close(out);
}
