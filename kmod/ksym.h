// Usage:
//   #define KSYM_FUNC_LIST X(ret_type, func_name, args)
//   #define KSYM_VAR_LIST X(type, var_name)
//   #include "ksym.h"
// Then the symbol can be accessed via "ksym_{name}".

#include <linux/kprobes.h>

#include "logging.h"

#ifndef KSYM_NAME
#define KSYM_NAME(name) ksym_##name
#endif

#ifndef KSYM_FUNC_LIST
#define KSYM_FUNC_LIST
#endif

#ifndef KSYM_VAR_LIST
#define KSYM_VAR_LIST
#endif

static void uninitialized_stub(void) {
  TRACE_ERR("Uninitialized kernel symbol\n");
}

// Define function pointers
#define X(ret_type, name, args)                                                \
  static ret_type(*KSYM_NAME(name)) args = (void *)&uninitialized_stub;
KSYM_FUNC_LIST
#undef X

// Define variables
#define X(type, name) static type *KSYM_NAME(name);
KSYM_VAR_LIST
#undef X

static void *get_kallsyms_lookup_name(void) {
  struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
  int ret = register_kprobe(&kp);
  if (ret < 0) {
    TRACE_ERR("Failed to register kprobe: %d. Using hardcoded address.\n", ret);
    // From `nm linux/current/linux | grep kallsyms_lookup_name`
    return (void *)0x600ad515;
  }
  unregister_kprobe(&kp);
  return (void *)kp.addr;
}

// Initialize function pointers
static void init_kernel_symbols(void) {
  void *(*kallsyms_lookup_name)(const char *name) = get_kallsyms_lookup_name();

#define X(type, name, ...)                                                     \
  do {                                                                         \
    KSYM_NAME(name) = kallsyms_lookup_name(#name);                             \
    if (KSYM_NAME(name) == NULL) {                                             \
      TRACE_ERR("kallsyms_lookup_name: %s not found\n", #name);                \
    }                                                                          \
  } while (0);
  KSYM_FUNC_LIST
  KSYM_VAR_LIST
#undef X
}
