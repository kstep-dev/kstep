#include <linux/btf.h>

#include "internal.h"

static struct task_struct *tasks[10];

#define MAX_INT_FIELDS 64

struct kstep_btf_field {
  const char *name;
  u32 offset;
  u32 bits;
};

struct kstep_btf_type {
  int count;
  struct kstep_btf_field fields[MAX_INT_FIELDS];
};

static void kstep_btf_type_parse(u8 kind, const char *type_name,
                                 struct kstep_btf_type *result) {
  KSYM_IMPORT(bpf_get_btf_vmlinux);
  struct btf *btf = KSYM_bpf_get_btf_vmlinux();
  if (IS_ERR(btf))
    panic("Failed to get vmlinux BTF");

  KSYM_IMPORT(btf_find_by_name_kind);
  int type_id = KSYM_btf_find_by_name_kind(btf, type_name, kind);
  if (type_id < 0)
    panic("Failed to find %s in BTF: %d", type_name, type_id);

  KSYM_IMPORT(btf_type_by_id);
  const struct btf_type *t = KSYM_btf_type_by_id(btf, type_id);
  if (IS_ERR(t))
    panic("Failed to get BTF type for %s: %ld", type_name, PTR_ERR(t));

  KSYM_IMPORT(btf_name_by_offset);
  const struct btf_member *member = btf_type_member(t);
  for (int i = 0; i < btf_type_vlen(t); i++, member++) {
    const struct btf_type *member_type = KSYM_btf_type_by_id(btf, member->type);
    if (!btf_type_is_int(member_type))
      continue;

    if (result->count >= MAX_INT_FIELDS)
      panic("Too many int fields in %s", type_name);
    struct kstep_btf_field *f = &result->fields[result->count++];
    f->name = KSYM_btf_name_by_offset(btf, member->name_off);
    f->offset = btf_member_bit_offset(t, i) / 8;
    f->bits = btf_int_bits(member_type);
  }
}

static void kstep_btf_type_print(struct kstep_btf_type *type, void *ptr) {
  for (int i = 0; i < type->count; i++) {
    struct kstep_btf_field *f = &type->fields[i];
    u64 mask = f->bits == 64 ? ~0ULL : (1ULL << f->bits) - 1;
    u64 val = *(u64 *)(ptr + f->offset) & mask;
    pr_info("  int%d %s = %llu", f->bits, f->name, val);
  }
}

static struct kstep_btf_type rq_type = {0};

static void setup(void) {
  kstep_btf_type_parse(BTF_KIND_STRUCT, "rq", &rq_type);

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 15; i++)
    kstep_tick();
}

static void on_tick(void) { kstep_btf_type_print(&rq_type, cpu_rq(1)); }

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .on_tick = on_tick,
    .step_interval_us = 1000,
    .print_tasks = true,
    .print_load_balance = true,
};
