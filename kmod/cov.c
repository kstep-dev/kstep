// Coverage: an AFL-style edge map over kernel/sched, fed by the kernel's __sanitizer_cov_trace_pc
// stub (linux/cov.c, CONFIG_KSTEP_COV). It is a region of its own, in the linear map so its
// physical address is exact and one range covers it all, and the cli reports that address on its
// ready line; the host reads the map with no guest I/O at all. Keeping it out of the state region
// (shm.h) means a new state field never moves it. An edge is two consecutive PCs on a CPU, hashed
// to a byte with a saturating count. Test CPUs are always recorded; CPU 0 only while the controller (pid 1) makes a
// scheduler call on a task's behalf (task.c brackets those with kstep_cov_controller).
#include <linux/gfp.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <asm/io.h>

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
  slot = &cov_map[(u32)(((*prev ^ ip) * 0x9E3779B97F4A7C15ULL) >> 48) % KSTEP_COV_SIZE];
  *prev = ip;
  if (*slot != 255)
    (*slot)++;
}

// Returns the guest-physical address of the map for the ready line.
phys_addr_t kstep_cov_init(void) {
  void (**hook)(u64) = kstep_ksym_lookup("sanitizer_cov_trace_pc");

  if (!hook) {
    TRACE_INFO("No coverage: the kernel was built without linux/config.kstep.cov");
    return 0;
  }
  cov_map = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(KSTEP_COV_SIZE));
  if (!cov_map)
    panic("Failed to allocate the coverage map");
  WRITE_ONCE(*hook, cov_trace_pc);
  return virt_to_phys(cov_map);
}

void kstep_cov_controller(bool on) { WRITE_ONCE(cov_controller, on); }
