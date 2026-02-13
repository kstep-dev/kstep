#include "internal.h"

#define COV_BUFFER_SIZE (32 * 1024)

static u64 cov_buffer[COV_BUFFER_SIZE];
static int cov_counter = 0; // no need to be atomic with serialized sched calls
static struct file *cov_file = NULL;

static void kstep_cov_record(u64 ip) {
  if (smp_processor_id() == 0)
    return;
  if (cov_counter >= COV_BUFFER_SIZE)
    return;
  cov_buffer[cov_counter++] = ip;
}

KSYM_IMPORT_TYPED(typeof(&kstep_cov_record), sanitizer_cov_trace_pc);

void kstep_cov_init(void) {
  if (KSYM_sanitizer_cov_trace_pc == NULL)
    panic("sanitizer_cov_trace_pc not found");

  cov_file = filp_open(KSTEP_CONSOLE(2), O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(cov_file))
    panic("Failed to open %s: %ld", KSTEP_CONSOLE(2), PTR_ERR(cov_file));

  // Pre-fault each page in the buffer
  for (int i = 0; i < COV_BUFFER_SIZE; i += PAGE_SIZE / sizeof(cov_buffer[0]))
    READ_ONCE(cov_buffer[i]);
}
void kstep_cov_enable(void) { *KSYM_sanitizer_cov_trace_pc = kstep_cov_record; }
void kstep_cov_disable(void) { *KSYM_sanitizer_cov_trace_pc = NULL; }

void kstep_cov_dump(void) {
  pr_info("cov_counter: %d\n", cov_counter);
  kernel_write(cov_file, cov_buffer, cov_counter * sizeof(cov_buffer[0]), 0);
}
