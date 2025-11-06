#include <linux/string.h>

struct controller_ops {
  const char *name;
  void (*init)(void);
  void (*body)(void);
};

extern struct controller_ops controller_aa3ee4f;
extern struct controller_ops controller_bbce3de;
extern struct controller_ops controller_cd9626e;
extern struct controller_ops controller_2feab24;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    &controller_aa3ee4f,
    &controller_bbce3de,
    &controller_cd9626e,
    &controller_2feab24,
    &controller_noop,
};

void controller_run(struct controller_ops *ops);
void call_tick_once(bool print_tasks_flag);
