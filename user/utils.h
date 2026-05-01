#define _GNU_SOURCE

#include <stdio.h>      // fprintf
#include <sys/reboot.h> // reboot

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    reboot(RB_AUTOBOOT);                                                       \
    __builtin_unreachable();                                                   \
  } while (0)

