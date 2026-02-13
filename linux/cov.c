#include <linux/export.h>
#include <linux/instruction_pointer.h>

void (*sanitizer_cov_trace_pc)(u64) = NULL;
EXPORT_SYMBOL(sanitizer_cov_trace_pc);

void notrace __sanitizer_cov_trace_pc(void);
void notrace __sanitizer_cov_trace_pc(void)
{
	if (sanitizer_cov_trace_pc)
		sanitizer_cov_trace_pc(_RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_pc);
