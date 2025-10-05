// Usage:
//   #define KERNEL_SYMBOL_LIST X(ret_type, func_name, args)
//   #include "kernel_sym.h"
// Then the symbol can be accessed via "kernel_{func_name}".

#include <linux/kprobes.h>

#include "logging.h"

#ifndef KERNEL_SYMBOL_LIST
#define KERNEL_SYMBOL_LIST
#error "KERNEL_SYMBOL_LIST is not defined"
#endif

static void uninitialized_stub(void) {
  TRACE_ERROR("Uninitialized kernel symbol\n");
}

// Define function pointers
#define X(ret_type, name, args)                                                \
  ret_type(*kernel_##name) args = (void *)&uninitialized_stub;
KERNEL_SYMBOL_LIST
#undef X

static void *get_kallsyms_lookup_name(void) {
  struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
  int ret = register_kprobe(&kp);
  if (ret < 0) {
    TRACE_ERROR("Failed to register kprobe: %d. Using hardcoded address.\n",
                ret);
    // From `nm linux/current/linux | grep kallsyms_lookup_name`
    return (void *)0x600ad515;
  }
  unregister_kprobe(&kp);
  return (void *)kp.addr;
}

// Initialize function pointers
static void init_kernel_symbols(void) {
  void *(*kallsyms_lookup_name)(const char *name) = get_kallsyms_lookup_name();

#define X(ret_type, name, args)                                                \
  do {                                                                         \
    kernel_##name = kallsyms_lookup_name(#name);                               \
    if (kernel_##name == NULL) {                                               \
      TRACE_ERROR("lookup_func_addr: %s not found\n", #name);                  \
    }                                                                          \
  } while (0);
  KERNEL_SYMBOL_LIST
#undef X
}
