#ifndef SIGCODE_H
#define SIGCODE_H

#define SIGCODE_LIST                                                           \
  X(SIGCODE_UNKNOWN)                                                           \
  X(SIGCODE_FORK)                                                              \
  X(SIGCODE_FORK_PIN)                                                          \
  X(SIGCODE_FORK_PIN_RANGE)                                                    \
  X(SIGCODE_FORK_FF)                                                           \
  X(SIGCODE_CLONE3)                                                            \
  X(SIGCODE_CLONE3_L3_0)                                                       \
  X(SIGCODE_CLONE3_L3_1)                                                       \
  X(SIGCODE_CLONE3_L2_1)                                                       \
  X(SIGCODE_CLONE3_L1_0)                                                       \
  X(SIGCODE_CLONE3_root)                                                       \
  X(SIGCODE_SLEEP)                                                             \
  X(SIGCODE_EXIT)                                                              \
  X(SIGCODE_PAUSE)                                                             \
  X(SIGCODE_CGROUP_CREATE)                                                     \
  X(SIGCODE_SETCPU_CGROUP)                                                     \
  X(SIGCODE_REWEIGHT_CGROUP)                                                   \
  X(SIGCODE_REWEIGHT)                                                          \
  X(SIGCODE_PIN)

enum sigcode {
#define X(name) name,
  SIGCODE_LIST
#undef X
};

static const char *sigcode_to_str[] = {
#define X(name) #name,
    SIGCODE_LIST
#undef X
};

#endif
