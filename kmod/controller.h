#include <linux/string.h>

struct controller_ops {
  const char *name;
  int (*init)(void);
  int (*step)(int iter);
  int (*exit)(void);
};

extern struct controller_ops bug_aa3ee4f_ops;
extern struct controller_ops bug_cd9626e_ops;

static int noop_init(void) { return 0; }
static int noop_step(int iter) { return 0; }
static int noop_exit(void) { return 0; }
static struct controller_ops noop_ops = {
    .name = "noop",
    .init = noop_init,
    .step = noop_step,
    .exit = noop_exit,
};

static struct controller_ops *controller_ops_list[] = {
    &bug_aa3ee4f_ops,
    &bug_cd9626e_ops,
    &noop_ops,
};

static struct controller_ops *get_controller_ops(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(controller_ops_list); i++) {
    if (strcmp(controller_ops_list[i]->name, name) == 0) {
      return controller_ops_list[i];
    }
  }
  return NULL;
}
