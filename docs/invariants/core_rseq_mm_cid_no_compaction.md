# RSEQ mm_cid Compaction Invariant
**Source bug:** `02d954c0fdf91845169cdacc7405b120f90afe01`

No generic invariant applicable. The bug is specific to the RSEQ mm_cid userspace subsystem (`max_nr_cid` high-water mark not shrinking, `recent_cid` reused without bounds check); it does not violate a general scheduler runqueue/entity invariant and is not observable from kernel scheduling state.
