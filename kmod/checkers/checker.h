// Checkers: rules over scheduler state and decisions, enabled by name (the cli's `check` verb). A
// rule that fires writes {"type":"warn","check":name,"message":"..."} to the stream. One file per
// rule, each with the commit it is derived from; checker.c holds the table that names them.
//
// A rule is one function -- the one that enables it -- and enabling is registering: the rule signs
// up for the events it watches (event.h) and is called on them from then on. So there is no flag to
// test, no dispatch to add a line to, and nothing is traced for a rule nobody enabled -- a rule
// that is not enabled does not exist as far as the rest of kstep is concerned.
//
// Every event a rule can watch is a point kstep already owns: a traced kernel function, the tick,
// a settled step, or the freeze command. No rule installs a probe of its own. A rule that needs the
// outcome of a decision samples what the decision saw when it is made and gives its verdict on
// settle, once every test CPU has acted on what the step asked of it.
#ifndef KSTEP_CHECKER_H
#define KSTEP_CHECKER_H

#include <linux/string.h>
#include <linux/version.h>

#include "event.h"
#include "internal.h"
#include "shm.h" // KSTEP_SHM_TASKS: the session task cap

#include <kernel/sched/pelt.h> // LOAD_AVG_MAX, after sched.h

// A rule that fires says what it saw: {"type":"warn","check":name,"message":"..."}. The message is
// the rule's own sentence -- the numbers that tripped it, in its own vocabulary -- so nothing has
// to be padded into a shape it does not have.
__printf(2, 3) void kstep_warn(const char *check, const char *fmt, ...);

// The rules, one per file; checker.c names them. Enabling one is calling its entry point.
void kstep_check_sync_wakeup_enable(void);
void kstep_check_vruntime_enable(void);
void kstep_check_pelt_enable(void);
void kstep_check_frozen_enable(void);
void kstep_check_balance_enable(void);
void kstep_check_work_conserve_enable(void);
void kstep_check_util_decay_enable(void);

#endif
