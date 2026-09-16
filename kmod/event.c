// The events kstep raises itself (event.h): no kernel function stands behind them, so registering
// is only a matter of joining the list. The traced events are in trace.c, where registering also
// arms the hook.
#include "event.h"
#include "internal.h"

struct kstep_event kstep_event_tick_begin, kstep_event_tick_end, kstep_event_softirq_begin, kstep_event_softirq_end,
    kstep_event_settle, kstep_event_freeze;

bool kstep_event_add(struct kstep_event *ev, void *fn) {
  for (int i = 0; i < ev->n; i++)
    if (ev->fn[i] == fn)
      return false;
  if (WARN_ON(ev->n == KSTEP_EVENT_SUBS))
    return false;
  ev->fn[ev->n] = fn;
  smp_store_release(&ev->n, ev->n + 1); // the entry before the count: a tracer may be reading
  return true;
}

void kstep_on_tick_begin(kstep_void_fn fn) { kstep_event_add(&kstep_event_tick_begin, fn); }
void kstep_on_tick_end(kstep_void_fn fn) { kstep_event_add(&kstep_event_tick_end, fn); }
void kstep_on_softirq_begin(kstep_void_fn fn) { kstep_event_add(&kstep_event_softirq_begin, fn); }
void kstep_on_softirq_end(kstep_void_fn fn) { kstep_event_add(&kstep_event_softirq_end, fn); }
void kstep_on_settle(kstep_void_fn fn) { kstep_event_add(&kstep_event_settle, fn); }
void kstep_on_freeze(kstep_freeze_fn fn) { kstep_event_add(&kstep_event_freeze, fn); }
