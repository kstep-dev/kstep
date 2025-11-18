#include <linux/kprobes.h>

#include "kstep.h"

struct ksym_t ksym;
static void *(*kallsyms_lookup_name_fn)(const char *name);

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
  void *addr = (void *)kallsyms_lookup_name_fn(name);
  if (addr == NULL) {
    TRACE_ERR("Symbol %s not found", name);
    return default_addr;
  } else {
    TRACE_DEBUG("Symbol %-32s -> %px", name, addr);
    return addr;
  }
}

#define KSYM_VAR(type, name)
#define KSYM_FUNC(type, name, ...)                                             \
  static void stub_##name(void) {                                              \
    panic("Uninitialized ksym " #name " called");                              \
  }
#include "ksym.h"
#undef KSYM_FUNC
#undef KSYM_VAR

void ksym_init(void) {
  kallsyms_lookup_name_fn = ksym_get_kallsyms_lookup_name();

#define KSYM_FUNC(type, name, ...)                                             \
  ksym.name = ksym_get_addr(#name, (void *)&stub_##name);
#define KSYM_VAR(type, name)                                                   \
  ksym.name = ksym_get_addr(#name, (void *)0xdeadbeef);
#include "ksym.h"
#undef KSYM_FUNC
#undef KSYM_VAR

  TRACE_INFO("ksym initialized");
}
