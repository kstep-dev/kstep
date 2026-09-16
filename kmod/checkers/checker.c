// The rules' table: every rule the cli's `check` verb accepts, and nothing else. Enabling a rule is
// the rule registering for the events it watches, so this file never learns what any of them do.
#include <linux/stdarg.h>

#include "checker.h"

static const struct {
  const char *name; // the name the `check` verb takes, and the one a warn is reported under
  void (*enable)(void);
} checks[] = {
    {"sync_wakeup", kstep_check_sync_wakeup_enable},
    {"vruntime", kstep_check_vruntime_enable},
    {"pelt", kstep_check_pelt_enable},
    {"frozen", kstep_check_frozen_enable},
    {"balance", kstep_check_balance_enable},
    {"work_conserve", kstep_check_work_conserve_enable},
    {"util_decay", kstep_check_util_decay_enable},
};

// %pV formats the rule's own message straight into the record, so a warn costs no buffer of its
// own: these run from ftrace callbacks, with interrupts off and runqueue locks held.
void kstep_warn(const char *check, const char *fmt, ...) {
  struct kstep_json json;
  va_list args;
  struct va_format vaf = {.fmt = fmt, .va = &args};

  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "warn");
  kstep_json_field_str(&json, "check", check);
  va_start(args, fmt);
  kstep_json_field_fmt(&json, "message", "\"%pV\"", &vaf);
  va_end(args);
  kstep_json_end(&json);
}

int kstep_check_enable(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(checks); i++)
    if (!strcmp(name, checks[i].name)) {
      checks[i].enable(); // registering twice is harmless, so enabling twice is too
      return 0;
    }
  return -ENOENT;
}
