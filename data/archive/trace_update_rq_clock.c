//
// Trace update_rq_clock
//

static void update_rq_clock_cb(unsigned long ip, unsigned long parent_ip,
    struct ftrace_ops *op,
    struct ftrace_regs *fregs) {
if (smp_processor_id() == 0)
return;

struct rq *rq = (void *)regs_get_kernel_argument((void *)fregs, 0);
u64 clock = sched_clock();
s64 delta = (s64)clock - rq->clock;
TRACE_INFO("update_rq_clock called on CPU %d, clock=%llu, rq->clock=%llu, "
"delta=%lld",
smp_processor_id(), clock, rq->clock, delta);
}

void kstep_trace_rq_clock(void) {
kstep_trace_function("update_rq_clock", &update_rq_clock_cb);
TRACE_INFO("Traced update_rq_clock");
}
