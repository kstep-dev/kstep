#include <linux/string.h>

#include "internal.h"

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
  u32 pid;
  u32 cmd_id;
  u64 ip;
};

static struct cov_entry cov_buffer[NR_CPUS][COV_BUFFER_SIZE];
static int cov_counter[NR_CPUS] = {0}; // no need to be atomic with serialized sched calls per CPU
static struct file *cov_file = NULL;
static struct file *sig_file = NULL;
static u32 cov_cmd_id = 0;

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
  sig_file = filp_open("/dev/ttyS3", O_WRONLY | O_NOCTTY, 0);
  if (IS_ERR(sig_file))
    panic("Failed to open /dev/ttyS3: %ld", PTR_ERR(sig_file));

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

void kstep_cov_dump_pcs(void) {
  for (int cpu = 0; cpu < num_possible_cpus(); cpu++) {
    kernel_write(cov_file, cov_buffer[cpu], cov_counter[cpu] * sizeof(cov_buffer[0][0]), 0);
    cov_counter[cpu] = 0;
  }
}

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

void kstep_cov_reset(void) {
  for (int cpu = 0; cpu < num_possible_cpus(); cpu++)
    cov_counter[cpu] = 0;
}

#define PID_MAP_SIZE 2048
#define SIG_CHUNK_SIZE 64 // Each signal chunk has 64 signals

// Hash map to track the previous PC and signals for each PID
struct slot_entry {
  u32 pid;
  u64 prev_pc;
  u32 sig_count;
  u64 sigs[SIG_CHUNK_SIZE];
};
static struct slot_entry slot_entries[PID_MAP_SIZE];

// ref: https://github.com/google/syzkaller/blob/17d780d63fb22ce9f5928fb059d6dde009510d37/executor/executor.cc#L1600
static __always_inline u32 kstep_cov_hash(u32 a) {
  a = (a ^ 61) ^ (a >> 16);
  a = a + (a << 3);
  a = a ^ (a >> 4);
  a = a * 0x27d4eb2d;
  a = a ^ (a >> 15);
  return a;
}

// Get the slot ID in the hash map for the given PID
static __always_inline int get_slot_by_pid(u32 pid) {
  u32 idx = pid & (PID_MAP_SIZE - 1);
  for (u32 i = 0; i < PID_MAP_SIZE; i++) {
    u32 pos = (idx + i) & (PID_MAP_SIZE - 1);
    if (slot_entries[pos].sig_count == 0) {
      slot_entries[pos].pid = pid;
      slot_entries[pos].prev_pc = 0;
      return pos;
    }
    if (slot_entries[pos].pid == pid) {
      return pos;
    }
  }
  panic("slot not found for pid %d", pid);
}

static __always_inline void kstep_cov_flush_sigs(u32 slot, u32 cmd_id) {
  u32 count = slot_entries[slot].sig_count;

  if (count == 0)
    return;

  struct {
    u32 pid;
    u32 cmd_id;
    u64 sig;
  } records[SIG_CHUNK_SIZE];

  for (u32 i = 0; i < count; i++) {
    records[i].pid = slot_entries[slot].pid;
    records[i].cmd_id = cmd_id;
    records[i].sig = slot_entries[slot].sigs[i];
  }

  kernel_write(sig_file, records, count * sizeof(records[0]), 0);

  slot_entries[slot].sig_count = 0;
}

void kstep_cov_dump_signal(void) {
  u32 cmd_id = kstep_cov_cmd_id_get();
  if (IS_ERR(sig_file))
    return;

  for (int cpu = 0; cpu < num_possible_cpus(); cpu++) {
    int count = cov_counter[cpu];
    for (int i = 0; i < count; i++) {
      struct cov_entry *e = &cov_buffer[cpu][i];
      u64 prev, pc, sig;
      u32 slot;
      u32 sig_count;

      if (e->cmd_id != cmd_id)
        panic("cov_entry[%d].cmd_id != cmd_id", i);

      slot = get_slot_by_pid(e->pid);

      // ref: https://github.com/google/syzkaller/blob/17d780d63fb22ce9f5928fb059d6dde009510d37/executor/executor.cc#L1226
      prev = slot_entries[slot].prev_pc;
      pc = e->ip;
      sig = pc ^ (kstep_cov_hash((u32)(prev & 0xfff)) & 0xfff);
      slot_entries[slot].prev_pc = pc;

      sig_count = slot_entries[slot].sig_count;
      slot_entries[slot].sigs[sig_count] = sig;
      slot_entries[slot].sig_count++;
      if (slot_entries[slot].sig_count == SIG_CHUNK_SIZE)
        kstep_cov_flush_sigs(slot, cmd_id);
    }
  }

  for (u32 slot = 0; slot < PID_MAP_SIZE; slot++) {
    if (slot_entries[slot].sig_count == 0)
      continue;
    kstep_cov_flush_sigs(slot, cmd_id);
  }
}
