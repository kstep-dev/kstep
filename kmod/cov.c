#include "internal.h"

#define COV_BUFFER_SIZE (32 * 1024)

u64 cov_buffer[COV_BUFFER_SIZE];
int cov_counter = 0; // no need to be atomic with serialized sched calls

static void kstep_cov_record(u64 ip) {
  if (smp_processor_id() == 0)
    return;
  if (cov_counter >= COV_BUFFER_SIZE)
    return;
  cov_buffer[cov_counter++] = ip;
}

KSYM_IMPORT_TYPED(typeof(kstep_cov_record), sanitizer_cov_trace_pc);
void kstep_cov_init(void) {
  if (KSYM_sanitizer_cov_trace_pc == NULL)
    panic("sanitizer_cov_trace_pc not found");
  for (int i = 0; i < COV_BUFFER_SIZE; i += PAGE_SIZE / sizeof(cov_buffer[0]))
    READ_ONCE(cov_buffer[i]);
}
void kstep_cov_enable(void) { KSYM_sanitizer_cov_trace_pc = kstep_cov_record; }
void kstep_cov_disable(void) { KSYM_sanitizer_cov_trace_pc = NULL; }

void kstep_cov_dump(void) {
  pr_info("cov_counter: %d\n", cov_counter);
  for (int i = 0; i < ARRAY_SIZE(cov_buffer) && i < cov_counter; i++)
    pr_info("%d: %llx\n", i, cov_buffer[i]);
}
