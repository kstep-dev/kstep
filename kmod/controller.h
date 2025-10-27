#include <linux/string.h>

struct controller_ops {
  const char *name;
  int (*init)(void);
  int (*step)(int iter);
  int (*exit)(void);
};

extern struct controller_ops controller_aa3ee4f;
extern struct controller_ops controller_cd9626e;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    &controller_aa3ee4f,
    &controller_cd9626e,
    &controller_noop,
};

static struct controller_ops *get_controller_ops(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(controller_ops_list); i++) {
    if (strcmp(controller_ops_list[i]->name, name) == 0) {
      return controller_ops_list[i];
    }
  }
  return NULL;
}
