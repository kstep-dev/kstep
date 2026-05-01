#define _GNU_SOURCE

#include <sched.h>      // sched_setaffinity
#include <stdio.h>      // fprintf
#include <unistd.h>     // getpid
#include <sys/reboot.h> // reboot

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    reboot(RB_AUTOBOOT);                                                       \
    __builtin_unreachable();                                                   \
  } while (0)

