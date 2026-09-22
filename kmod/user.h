#ifndef KSTEP_USER_H
#define KSTEP_USER_H

// What the controller asks of a task, carried by the control-file read: kmod/task.c queues it and
// user.c makes the syscall, so the scheduler sees the same path with no signal in front of it, and
// the asks arrive in order. SIGUSR1 stays for the one thing a read cannot do: return a task from
// whatever it is asleep in. It carries nothing.
enum kstep_ctrl {
  KSTEP_CTRL_NONE = 0, // nothing asked: the read halts (kmod-side only)
  KSTEP_CTRL_EXIT,     // _exit(0)
  KSTEP_CTRL_PAUSE,    // pause(): interruptible, not freezable
  KSTEP_CTRL_BLOCK,    // nanosleep(): do_nanosleep() adds TASK_FREEZABLE, which is the difference
  KSTEP_CTRL_CHAN_READ,  // read the channel: sleeps in pipe_read() until there is a byte
  KSTEP_CTRL_CHAN_WRITE, // write it: pipe_write() sync-wakes one reader from this CPU
  KSTEP_CTRL_YIELD,      // sched_yield(): give the CPU up, staying runnable -- rr goes to the tail of its list, fair skips the entity once
  KSTEP_CTRL_PARK, // a new task's first read: sleeps in the read itself (kmod-side only)
};

// The channel the tasks synchronise through: one FIFO on the root filesystem, created by init
// before the module loads. A read blocks until there is a byte; a write never blocks and leaves
// the byte behind if no one is reading, so the channel counts. The write is the point: pipe_write()
// wakes a reader through wake_up_interruptible_sync_poll(), the WF_SYNC wakeup a real waker makes.
#define PIPE_PATH "/pipe"

#define KSTEP_CTRL_FD 3
#endif
