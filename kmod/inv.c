#include <linux/ftrace.h>
#include <linux/string.h>
#include "internal.h"

#define MAX_INVARIANTS 64

struct inv_probe {
  struct fgraph_ops          fgops;
  struct kstep_invariant    *inv;
};

static struct inv_probe probes[MAX_INVARIANTS];
static int n_probes;

static void inv_dispatch(struct inv_probe *p, bool entry,
                         struct ftrace_regs *fregs) {
  struct kstep_invariant *inv = p->inv;
  int cpu  = smp_processor_id();
  int slot = inv->per_cpu ? cpu : 0;
  struct kstep_inv_ctx ctx = { .cpu = cpu, .fregs = fregs };

  if (cpu == 0)
    return;

  if (entry) {
    if (inv->capture)
      inv->capture(&ctx, inv->saved_state[slot]);
  } else {
    if (inv->verify && !inv->verify(&ctx, inv->saved_state[slot]))
      kstep_fail("INVARIANT '%s' violated on CPU %d", inv->name, cpu);
  }
}

static int inv_entry_handler(struct ftrace_graph_ent *trace,
                             struct fgraph_ops *gops,
                             struct ftrace_regs *fregs) {
  inv_dispatch(container_of(gops, struct inv_probe, fgops), true, fregs);
  return 1; /* always hook the return */
}

static void inv_return_handler(struct ftrace_graph_ret *trace,
                               struct fgraph_ops *gops,
                               struct ftrace_regs *fregs) {
  inv_dispatch(container_of(gops, struct inv_probe, fgops), false, NULL);
}

typedef int (*register_ftrace_graph_t)(struct fgraph_ops *);

KSYM_IMPORT(register_ftrace_graph);
KSYM_IMPORT(unregister_ftrace_graph);

void kstep_invariants_init(void) {
  struct kstep_invariant **invs = kstep_driver->invariants;
  if (!invs)
    return;

  if (!KSYM_register_ftrace_graph || !KSYM_unregister_ftrace_graph)
    panic("register_ftrace_graph not found — is CONFIG_FUNCTION_GRAPH_TRACER enabled?");

  for (int i = 0; invs[i]; i++) {
    if (n_probes >= MAX_INVARIANTS)
      panic("Too many invariants (max %d)", MAX_INVARIANTS);

    struct kstep_invariant *inv = invs[i];
    struct inv_probe *p = &probes[n_probes++];

    p->inv             = inv;
    p->fgops.entryfunc = inv_entry_handler;
    p->fgops.retfunc   = inv_return_handler;
    p->fgops.ops.flags |= FTRACE_OPS_FL_SAVE_REGS;

    if (ftrace_set_filter(&p->fgops.ops, (unsigned char *)inv->func_name,
                          strlen(inv->func_name), 1))
      panic("Failed to set ftrace filter for '%s'", inv->func_name);

    if (KSYM_register_ftrace_graph(&p->fgops))
      panic("Failed to register ftrace for '%s'", inv->func_name);

    TRACE_INFO("ftrace '%s': invariant '%s'", inv->func_name, inv->name);
  }
}

void kstep_invariants_exit(void) {
  for (int i = 0; i < n_probes; i++)
    KSYM_unregister_ftrace_graph(&probes[i].fgops);
  n_probes = 0;
}
