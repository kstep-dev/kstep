#define SIGCODE_LIST                                                           \
  X(SIGCODE_UNKNOWN)                                                           \
  X(SIGCODE_FORK)

enum sigcode {
#define X(name) name,
  SIGCODE_LIST
#undef X
};

const char *sigcode_to_str[] = {
#define X(name) #name,
    SIGCODE_LIST
#undef X
};
