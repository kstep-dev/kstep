#include <linux/kprobes.h>

#include "internal.h"

struct ksym ksym;
static void *(*kallsyms_lookup_name_fn)(const char *name);

static void *get_kallsyms_lookup_name(void) {
  struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
  int ret = register_kprobe(&kp);
  if (ret < 0)
    panic("Failed to register kprobe: %d", ret);
  unregister_kprobe(&kp);
  return (void *)kp.addr;
}

void *kstep_ksym_get_addr(const char *name) {
  void *addr = (void *)kallsyms_lookup_name_fn(name);
  if (addr == NULL)
    panic("Symbol %s not found", name);
  return addr;
}

void kstep_ksym_init(void) {
  kallsyms_lookup_name_fn = get_kallsyms_lookup_name();

#define KSYM_FUNC(type, name, ...) ksym.name = kstep_ksym_get_addr(#name);
#define KSYM_VAR(type, name) ksym.name = kstep_ksym_get_addr(#name);
#include "ksym.h"
}
