#ifndef SIGCODE_H
#define SIGCODE_H

#define SIGCODE_LIST                                                           \
  X(SIGCODE_UNKNOWN)                                                           \
  X(SIGCODE_FORK)                                                              \
  X(SIGCODE_SLEEP)                                                             \
  X(SIGCODE_EXIT)                                                              \
  X(SIGCODE_PAUSE)

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
