#ifndef KSTEP_INVARIANT_H
#define KSTEP_INVARIANT_H

#include <linux/types.h>

struct rq;

enum kstep_invariant_type {
  TEMPORAL_DELTA, // Invariant for comparing rq fields before and after a tick
};
struct kstep_invariant {
  const char *name;                  // Name for logging
  enum kstep_invariant_type type;    // Type of invariant
  s64 (*get_value)(struct rq *rq);   // Extract value from rq
  s64 max_delta; // Max allowed change per tick (absolute value)
};

extern struct kstep_invariant invariant_util_avg;

#endif
