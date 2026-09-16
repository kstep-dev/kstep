// Coverage: an AFL-style edge map over kernel/sched, fed by the kernel's __sanitizer_cov_trace_pc
// stub (linux/cov.c, CONFIG_KSTEP_COV) and living in the shared region (shm.h), so the host reads
// it with no guest I/O at all. An edge is two consecutive PCs on a CPU, hashed to a byte with a
// saturating count. Test CPUs are always recorded; CPU 0 only while the controller (pid 1) makes a
// scheduler call on a task's behalf (task.c brackets those with kstep_cov_controller).
#include <linux/percpu.h>
#include <linux/sched.h>

#include "internal.h"
#include "shm.h"

static u8 *cov_map;
static DEFINE_PER_CPU(u64, cov_prev);
static bool cov_controller;

static void cov_trace_pc(u64 ip) {
  u64 *prev;
  u8 *slot;

  if (raw_smp_processor_id() == 0 && !(READ_ONCE(cov_controller) && current->pid == 1))
    return;
  prev = raw_cpu_ptr(&cov_prev);
  slot = &cov_map[(u32)(((*prev ^ ip) * 0x9E3779B97F4A7C15ULL) >> 48) % KSTEP_SHM_COV];
  *prev = ip;
  if (*slot != 255)
    (*slot)++;
}

void kstep_cov_init(u8 *map) {
  void (**hook)(u64) = kstep_ksym_lookup("sanitizer_cov_trace_pc");

  if (!hook) {
    TRACE_INFO("No coverage: the kernel was built without linux/config.kstep.cov");
    return;
  }
  cov_map = map;
  WRITE_ONCE(*hook, cov_trace_pc);
}

void kstep_cov_controller(bool on) { WRITE_ONCE(cov_controller, on); }
