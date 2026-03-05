#include <linux/string.h>

#include "internal.h"

#define COV_BUFFER_SIZE (32 * 1024)

// COV_DISABLED: the coverage is not recorded
// COV_ENABLED: the coverage is recorded on CPU 1-N
// COV_ENABLED_WITH_CONTROLLER: the coverage is recorded on CPU 1-N, and on CPU 0 if the current task is the controller
enum kstep_cov_mode {
  COV_DISABLED = 0,
  COV_ENABLED = 1,
  COV_ENABLED_WITH_CONTROLLER = 2,
};

static unsigned int cov_mode = COV_DISABLED;


// Record the coverage for each command and each pid
struct cov_entry {
  u32 cmd_id;
  u32 pid;
  u64 ip;
};

static struct cov_entry cov_buffer[NR_CPUS][COV_BUFFER_SIZE];
static int cov_counter[NR_CPUS] = {0}; // no need to be atomic with serialized sched calls per CPU
static struct file *cov_file = NULL;
static u32 cov_cmd_id = 0;


// Hash map to track the previous PC and signals for each PID under this command
#define PID_MAP_SIZE 2048
#define SIG_CHUNK_SIZE 64 // Each signal chunk has 64 signals

struct slot_entry {
  u32 pid;
  u32 pc_count;
  u64 pcs[SIG_CHUNK_SIZE];
};
static struct slot_entry slot_entries[PID_MAP_SIZE];

static void kstep_cov_reset(void) {
  for (int cpu = 0; cpu < num_possible_cpus(); cpu++)
    cov_counter[cpu] = 0;
  for (u32 slot = 0; slot < PID_MAP_SIZE; slot++) {
    slot_entries[slot].pid = 0xffffffff;
    slot_entries[slot].pc_count = 0;
  }
}

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

  cov_buffer[cpu][cov_counter[cpu]] = (struct cov_entry){
      .pid = current->pid,
      .cmd_id = READ_ONCE(cov_cmd_id),
      .ip = ip,
  };
  barrier();
  cov_counter[cpu]++;
}

KSYM_IMPORT_TYPED(typeof(&kstep_cov_record), sanitizer_cov_trace_pc);

void kstep_cov_init(void) {
  if (KSYM_sanitizer_cov_trace_pc == NULL)
    panic("sanitizer_cov_trace_pc not found");

  cov_file = filp_open("/dev/ttyS2", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(cov_file))
    panic("Failed to open /dev/ttyS2: %ld", PTR_ERR(cov_file));

  // Pre-fault each page in the buffer
  for (int cpu = 0; cpu < NR_CPUS; cpu++)
    for (int i = 0; i < COV_BUFFER_SIZE; i += PAGE_SIZE / sizeof(cov_buffer[0][0])) {
      // touch 1 read per element is sufficient to pre-fault the page
      READ_ONCE(cov_buffer[cpu][i].ip);
    }

  kstep_cov_reset();
}

// Cov mode control functions
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

// Command ID management functions
void kstep_cov_cmd_id_inc(void) {
  u32 next = READ_ONCE(cov_cmd_id) + 1;
  barrier();
  WRITE_ONCE(cov_cmd_id, next);
  barrier();
}

static u32 kstep_cov_cmd_id_get(void) {
  u32 current_cmd_id = READ_ONCE(cov_cmd_id);
  barrier();
  return current_cmd_id;
}


// Get an slot in the hash map for the given PID
static __always_inline int get_slot_by_pid(u32 pid) {
  u32 idx = pid & (PID_MAP_SIZE - 1);
  for (u32 i = 0; i < PID_MAP_SIZE; i++) {
    u32 pos = (idx + i) & (PID_MAP_SIZE - 1);
    if (slot_entries[pos].pid == 0xffffffff) {
      slot_entries[pos].pid = pid;
      slot_entries[pos].pc_count = 0;
      return pos;
    }
    if (slot_entries[pos].pid == pid) {
      return pos;
    }
  }
  panic("slot not found for pid %d", pid);
}

static __always_inline void kstep_cov_flush_pcs(u32 slot, u32 cmd_id) {
  u32 count = slot_entries[slot].pc_count;

  if (count == 0)
    return;

  struct {
    u32 cmd_id;
    u32 pid;
    u64 pc;
  } records[SIG_CHUNK_SIZE];

  for (u32 i = 0; i < count; i++) {
    records[i].cmd_id = cmd_id;
    records[i].pid = slot_entries[slot].pid;
    records[i].pc = slot_entries[slot].pcs[i];
  }

  kernel_write(cov_file, records, count * sizeof(records[0]), 0);

  slot_entries[slot].pc_count = 0;
}

void kstep_cov_dump(void) {
  u32 cmd_id = kstep_cov_cmd_id_get();
  if (IS_ERR(cov_file))
    return;

  for (int cpu = 0; cpu < num_possible_cpus(); cpu++) {
    int count = cov_counter[cpu];
    for (int i = 0; i < count; i++) {
      struct cov_entry *e = &cov_buffer[cpu][i];
      u32 slot, pc_count;

      if (e->cmd_id != cmd_id)
        panic("cov_entry[%d].cmd_id != cmd_id", i);

      slot = get_slot_by_pid(e->pid);

      pc_count = slot_entries[slot].pc_count;
      slot_entries[slot].pcs[pc_count] = e->ip;
      slot_entries[slot].pc_count++;
      if (slot_entries[slot].pc_count == SIG_CHUNK_SIZE)
        kstep_cov_flush_pcs(slot, cmd_id);
    }
  }

  for (u32 slot = 0; slot < PID_MAP_SIZE; slot++) {
    if (slot_entries[slot].pid != 0xffffffff)
      kstep_cov_flush_pcs(slot, cmd_id);
  }

  kstep_cov_reset();
}
