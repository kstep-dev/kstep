#ifndef KSTEP_USER_H
#define KSTEP_USER_H

enum sigcode {
  SIGCODE_WAKEUP,
  SIGCODE_FORK,
  SIGCODE_EXIT,
  SIGCODE_PAUSE,
  SIGCODE_BLOCK,
  SIGCODE_WAIT, // semaphore wait: block in pipe_read() until a token (byte) is there
  SIGCODE_POST, // semaphore post: write a token; pipe_write() wakes one waiter with a sync
                // (WF_SYNC) wakeup issued from this task's CPU
};

// wait/post are a counting semaphore implemented as one FIFO on the root filesystem, created
// by the kernel module at init: bytes are tokens, wait is a blocking read, post a one-byte write.
#define PIPE_PATH "/pipe"

#define TASK_READY_COMM "ready"
#endif
