#include <linux/bpf.h>
#include <linux/btf.h>

#include "internal.h"

static struct task_struct *tasks[10];
static struct btf *btf;
static int rq_type_id;

static void setup(void) {
  KSYM_IMPORT(bpf_get_btf_vmlinux);
  btf = KSYM_bpf_get_btf_vmlinux();
  if (IS_ERR(btf))
    panic("Failed to get vmlinux BTF");

  KSYM_IMPORT(btf_find_by_name_kind);
  rq_type_id = KSYM_btf_find_by_name_kind(btf, "rq", BTF_KIND_STRUCT);
  if (rq_type_id < 0)
    panic("Failed to find rq in BTF: %d", rq_type_id);

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 5; i++)
    kstep_tick();
}

#define PRINT_BUF_SIZE 16384
static char print_buf[PRINT_BUF_SIZE];

static void on_tick(void) {
  KSYM_IMPORT(btf_type_snprintf_show);
  int len = KSYM_btf_type_snprintf_show(btf, rq_type_id, cpu_rq(1), print_buf,
                                        PRINT_BUF_SIZE,
                                        BTF_SHOW_PTR_RAW | BTF_SHOW_COMPACT);
  kstep_output(print_buf, len);
  kstep_output("\n", 1);
}

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
