#ifndef KSTEP_USER_H
#define KSTEP_USER_H

enum sigcode {
  SIGCODE_WAKEUP,
  SIGCODE_FORK,
  SIGCODE_USLEEP,
  SIGCODE_EXIT,
  SIGCODE_PAUSE,
};

#define TASK_READY_COMM "ready"
#endif
