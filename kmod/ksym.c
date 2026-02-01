#include <linux/kprobes.h>
#include <linux/module.h>

#include "internal.h"

#define MAX_SYM_NAME_LENGTH 64

KSYM_IMPORT_RAW(struct rq, runqueues);

static void *(*kallsyms_lookup_name_fn)(const char *name);

static void *get_kallsyms_lookup_name(void) {
  struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
  int ret = register_kprobe(&kp);
  if (ret < 0)
    panic("Failed to register kprobe: %d", ret);
  unregister_kprobe(&kp);
  return (void *)kp.addr;
}

static void init_sym(const char *name, void **ptr) {
  if (strncmp(name, "KSYM_", 5) != 0)
    return;

  if (strlen(name) > MAX_SYM_NAME_LENGTH)
    panic("Symbol name %s too long", name);

  char buf[MAX_SYM_NAME_LENGTH];
  strncpy(buf, name + 5, sizeof(buf));
  char *dot = strchr(buf, '.');
  if (dot)
    *dot = '\0';

  *ptr = kallsyms_lookup_name_fn(buf);
  if (*ptr == NULL)
    panic("Symbol %s not found", buf);
}

void kstep_ksym_init(void) {
  kallsyms_lookup_name_fn = get_kallsyms_lookup_name();

  struct mod_kallsyms *kallsyms = THIS_MODULE->kallsyms;
  for (int i = 0; i < kallsyms->num_symtab; i++) {
    Elf_Sym *sym = &kallsyms->symtab[i];
    const char *name = kallsyms->strtab + sym->st_name;
    init_sym(name, (void **)sym->st_value);
  }
}
