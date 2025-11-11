#include "ksym.h"

#include <linux/kprobes.h>

#include "logging.h"

struct ksym_t ksym;

static void *ksym_get_kallsyms_lookup_name(void) {
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

static void *ksym_get_addr(const char *name, void *default_addr) {
  void *addr = (void *)ksym.kallsyms_lookup_name(name);
  if (addr == NULL) {
    TRACE_INFO("Symbol %s not found", name);
    return default_addr;
  } else {
    TRACE_DEBUG("Symbol %-32s -> %px", name, addr);
    return addr;
  }
}

#define X(type, name, ...)                                                     \
  static void stub_##name(void) {                                              \
    panic("Uninitialized ksym " #name " called");                              \
  }
KSYM_FUNC_LIST
#undef X

void ksym_init(void) {
  ksym.kallsyms_lookup_name = ksym_get_kallsyms_lookup_name();

#define X(type, name, ...)                                                     \
  ksym.name = ksym_get_addr(#name, (void *)&stub_##name);
  KSYM_FUNC_LIST
#undef X

#define X(type, name) ksym.name = ksym_get_addr(#name, (void *)0xdeadbeef);
  KSYM_VAR_LIST
#undef X

  TRACE_INFO("ksym initialized");
}
