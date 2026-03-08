// Invariant: util_avg should not change by more than max_delta per tick.
// A cliff change indicates a scheduler bug (e.g., sudden utilization drop/spike).

#include "internal.h"

static s64 get_util_avg(struct rq *rq) {
  return rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg;
}

struct kstep_invariant invariant_util_avg = {
  .name = "util_avg_cliff",
  .type = TEMPORAL_DELTA,
  .get_value = get_util_avg,
  .max_delta = 100,
};
