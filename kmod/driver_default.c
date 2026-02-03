#include <linux/bpf.h>
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

static int kstep_btf_field_parse(struct btf *btf,
                                 const struct btf_member *member,
                                 struct kstep_btf_field *field) {
  KSYM_IMPORT(btf_type_skip_modifiers);
  const struct btf_type *type =
      KSYM_btf_type_skip_modifiers(btf, member->type, NULL);

  // Skip non-int fields and bitfields
  if (!btf_type_is_int(type) || BTF_MEMBER_BIT_OFFSET(member->offset) % 8 != 0)
    return -1;

  KSYM_IMPORT(btf_name_by_offset);
  field->name = KSYM_btf_name_by_offset(btf, member->name_off);
  field->offset = BTF_MEMBER_BIT_OFFSET(member->offset) / 8;
  u32 int_info = *(u32 *)(type + 1);
  field->bits = BTF_INT_BITS(int_info);
  return 0;
}

static void kstep_btf_field_print(struct kstep_btf_field *field, void *ptr) {
  if (field->bits == 64)
    pr_info("  %s = %lld", field->name, *(u64 *)(ptr + field->offset));
  else if (field->bits == 32)
    pr_info("  %s = %d", field->name, *(u32 *)(ptr + field->offset));
  else if (field->bits == 16)
    pr_info("  %s = %d", field->name, *(u16 *)(ptr + field->offset));
  else if (field->bits == 8)
    pr_info("  %s = %d", field->name, *(u8 *)(ptr + field->offset));
  else
    panic("Invalid bit size: %d", field->bits);
}

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

  const struct btf_member *member = btf_type_member(t);
  for (int i = 0; i < btf_type_vlen(t); i++, member++) {
    if (kstep_btf_field_parse(btf, member, &result->fields[result->count]) < 0)
      continue;
    if (result->count++ >= MAX_INT_FIELDS)
      panic("Too many int fields in %s", type_name);
  }
}

static void kstep_btf_type_print(struct kstep_btf_type *type, void *ptr) {
  for (int i = 0; i < type->count; i++)
    kstep_btf_field_print(&type->fields[i], ptr);
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

  for (int i = 0; i < 5; i++)
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
    .print_sched_debug = true,
};
