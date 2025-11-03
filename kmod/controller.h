#include <linux/string.h>

struct controller_ops {
  const char *name;
  void (*init)(void);
  void (*body)(void);
};

// extern struct controller_ops controller_aa3ee4f;
// extern struct controller_ops controller_cd9626e;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    // &controller_aa3ee4f,
    // &controller_cd9626e,
    &controller_noop,
};

void controller_run(struct controller_ops *ops);
void controller_tick(void);
