#ifndef KSTEP_USER_H
#define KSTEP_USER_H

// Commands a running task picks up from its control-file read: the controller writes them into
// the task's mailbox, and the task sees them the next time it returns from its halt (the next
// tick), like a real process that acts when it runs. Sleeping tasks are still signalled.
enum kstep_cmd {
  KSTEP_CMD_NONE,
  KSTEP_CMD_FORK, // arg: number of children
};

struct kstep_msg {
  int cmd; // enum kstep_cmd
  int arg;
};

enum sigcode {
  SIGCODE_WAKEUP,
  SIGCODE_EXIT,
  SIGCODE_PAUSE,
  SIGCODE_BLOCK,
  SIGCODE_WAIT, // semaphore wait: block in pipe_read() until a token (byte) is there
  SIGCODE_POST, // semaphore post: write a token; pipe_write() wakes one waiter with a sync
                // (WF_SYNC) wakeup issued from this task's CPU
};

// wait/post are a counting semaphore implemented as one FIFO on the root filesystem, created
// by init before the module loads: bytes are tokens, wait is a blocking read, post a one-byte write.
#define PIPE_PATH "/pipe"

#define KSTEP_CTRL_FD 3
#endif
