#include "ksym.h"

#include <linux/kprobes.h>

#include "logging.h"

struct ksym_t ksym;

static void *get_kallsyms_lookup_name(void) {
  struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
  int ret = register_kprobe(&kp);
  if (ret < 0) {
    TRACE_ERR("Failed to register kprobe: %d. Using hardcoded address.", ret);
    // From `nm linux/current/linux | grep kallsyms_lookup_name`
    return (void *)0x600ad515;
  }
  unregister_kprobe(&kp);
  return (void *)kp.addr;
}

static void *get_addr(const char *name, void *default_addr) {
  void *addr = (void *)ksym.kallsyms_lookup_name(name);
  if (addr == NULL) {
    TRACE_ERR("Symbol %s not found", name);
    return default_addr;
  } else {
    TRACE_INFO("Symbol %-32s -> %px", name, addr);
    return addr;
  }
}

static void fn_stub(void) { TRACE_ERR("Uninitialized kernel symbol"); }

void ksym_init(void) {
  ksym.kallsyms_lookup_name = get_kallsyms_lookup_name();

#define X(type, name, ...) ksym.name = get_addr(#name, (void *)&fn_stub);
  KSYM_FUNC_LIST
#undef X

#define X(type, name) ksym.name = get_addr(#name, (void *)0xdeadbeef);
  KSYM_VAR_LIST
#undef X
}
