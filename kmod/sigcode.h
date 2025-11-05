#ifndef SIGCODE_H
#define SIGCODE_H

#define SIGCODE_LIST                                                           \
  X(SIGCODE_UNKNOWN)                                                           \
  X(SIGCODE_FORK)                                                              \
  X(SIGCODE_CLONE3_L3_0)                                                       \
  X(SIGCODE_CLONE3_L3_1)                                                       \
  X(SIGCODE_CLONE3_L2_1)                                                       \
  X(SIGCODE_SLEEP)                                                             \
  X(SIGCODE_EXIT)                                                              \
  X(SIGCODE_PAUSE)                                                             \
  X(SIGCODE_CGROUP_CREATE)                                                     \
  X(SIGCODE_RECORD_CGROUP)                                                     \
  X(SIGCODE_SETCPU_CGROUP)                                                      \
  X(SIGCODE_UNRIGISTER_CGROUP)                                                \
  X(SIGCODE_REWEIGHT_CGROUP_20)                                               \
  X(SIGCODE_REWEIGHT_CGROUP_100)                                              \
  X(SIGCODE_SETCPU_CGROUP_1)                                                   \
  X(SIGCODE_FORK_PIN)

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
