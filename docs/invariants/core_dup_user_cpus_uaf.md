# Use-after-free in dup_user_cpus_ptr
**Source bug:** `87ca4f9efbd7cc649ff43b87970888f2812945b8`

No generic invariant applicable. This is a memory-safety race condition (use-after-free of `user_cpus_ptr` due to missing lock protection), not a scheduler state consistency violation — no checkable predicate over scheduler structs can detect freed-pointer dereferences.
