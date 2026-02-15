#include "internal.h"
#include <linux/tty.h>

#define COV_BUFFER_SIZE (32 * 1024)

enum kstep_cov_mode {
  COV_DISABLED = 0,
  COV_ENABLED = 1,
  COV_ENABLED_WITH_CONTROLLER = 2,
};

static unsigned int cov_mode = COV_DISABLED;

__always_inline bool kstep_cov_mode_check(enum kstep_cov_mode mode) {
  unsigned int mode_current = READ_ONCE(cov_mode);
  barrier();
  return mode_current == mode;
}

__always_inline void kstep_cov_mode_set(enum kstep_cov_mode mode) {
  barrier();
  WRITE_ONCE(cov_mode, mode);
  barrier();
}

struct cov_entry {
  u32 id; // used for check the results
  u32 pid;
  u64 ip;
};

static struct cov_entry cov_buffer[NR_CPUS][COV_BUFFER_SIZE];
static int cov_counter[NR_CPUS] = {0}; // no need to be atomic with serialized sched calls per CPU
static struct file *cov_file = NULL;

static void kstep_cov_record(u64 ip) {
  int cpu = smp_processor_id();

  if (kstep_cov_mode_check(COV_DISABLED))
    return;

  // filter the coverage on CPU 0
  if (cpu == 0 &&
      (current != controller || !kstep_cov_mode_check(COV_ENABLED_WITH_CONTROLLER)))
    return;
  
  if (cov_counter[cpu] >= COV_BUFFER_SIZE)
    panic("cov_buffer[%d] overflow", cpu);

  cov_buffer[cpu][cov_counter[cpu]] = (struct cov_entry){.id = cov_counter[cpu], .pid = current->pid, .ip = ip};
  cov_counter[cpu]++;
}

KSYM_IMPORT_TYPED(typeof(&kstep_cov_record), sanitizer_cov_trace_pc);

// set the console to raw mode to avoid tty translation
// E.g. the default tty will be translated to \n(0x0a) to \r\n(0x0d0a).
static void kstep_cov_console_raw_mode(struct file *file) {
  KSYM_IMPORT(tty_set_termios);

  if (KSYM_tty_set_termios == NULL) {
    pr_warn("tty_set_termios not found; cov stream may be tty-translated\n");
    return;
  }

  struct tty_file_private *tty_file = file->private_data;
  if (!tty_file || !tty_file->tty) {
    pr_warn("cov console is not a tty; skip raw mode\n");
    return;
  }

  struct tty_struct *tty = tty_file->tty;
  struct ktermios termios = tty->termios;

  // Equivalent to "stty raw -echo -opost -onlcr -ocrnl -onlret".
  termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  termios.c_oflag &= ~(OPOST | ONLCR | OCRNL | ONLRET);
  termios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  termios.c_cflag &= ~(CSIZE | PARENB);
  termios.c_cflag |= CS8;
  termios.c_cc[VMIN] = 1;
  termios.c_cc[VTIME] = 0;

  KSYM_tty_set_termios(tty, &termios);
}

void kstep_cov_init(void) {
  if (KSYM_sanitizer_cov_trace_pc == NULL)
    panic("sanitizer_cov_trace_pc not found");

  cov_file = filp_open("/dev/ttyS2", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(cov_file))
    panic("Failed to open /dev/ttyS2: %ld", PTR_ERR(cov_file));

  kstep_cov_console_raw_mode(cov_file);

  // Pre-fault each page in the buffer
  for (int cpu = 0; cpu < NR_CPUS; cpu++)
    for (int i = 0; i < COV_BUFFER_SIZE; i += PAGE_SIZE / sizeof(cov_buffer[0][0])) {
      // touch 1 read per element is sufficient to pre-fault the page
      READ_ONCE(cov_buffer[cpu][i].ip);
    }
}

void kstep_cov_enable(void) {
  *KSYM_sanitizer_cov_trace_pc = kstep_cov_record;
  kstep_cov_mode_set(COV_ENABLED);
}

void kstep_cov_disable(void) {
  *KSYM_sanitizer_cov_trace_pc = NULL;
  kstep_cov_mode_set(COV_DISABLED);
}

void kstep_cov_enable_controller(void) {
  if (kstep_cov_mode_check(COV_ENABLED))
    kstep_cov_mode_set(COV_ENABLED_WITH_CONTROLLER);
}

void kstep_cov_disable_controller(void) {
  if (kstep_cov_mode_check(COV_ENABLED_WITH_CONTROLLER))
    kstep_cov_mode_set(COV_ENABLED);
}

void kstep_cov_dump(void) {
  for (int cpu = 0; cpu < num_possible_cpus(); cpu++) {
    pr_info("cov_counter[%d] = %d\n", cpu, cov_counter[cpu]);
    kernel_write(cov_file, cov_buffer[cpu], cov_counter[cpu] * sizeof(cov_buffer[0][0]), 0);
    cov_counter[cpu] = 0;
  }
}
