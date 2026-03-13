# Topology: Redundant Variable and Incorrect RCU Type in build_sched_domains

**Commit:** `7f434dff76215af00c26ba6449eaa4738fe9e2ab`
**Affected files:** kernel/sched/topology.c
**Fixed in:** v5.18-rc1
**Buggy since:** v5.17-rc1 (introduced by commit `e496132ebedd` "sched/fair: Adjust the allowed NUMA imbalance when SD_NUMA spans multiple LLCs")

## Bug Description

Commit `e496132ebedd` ("sched/fair: Adjust the allowed NUMA imbalance when SD_NUMA spans multiple LLCs") introduced new logic in `build_sched_domains()` to calculate the allowed NUMA imbalance when a `SD_NUMA` sched domain spans multiple LLCs. This code added a loop that walks upward through the sched domain hierarchy to find the first NUMA domain, using two local variables `top` and `top_p` to track the current node and its parent, respectively.

The variable `top` was redundant. The loop's purpose is to find the topmost `SD_NUMA` domain (stored in `top_p`), and after the loop only `top_p` is used to read `span_weight`. The `top` variable shadows the outer `sd` variable's role without providing any additional value. Furthermore, the `top_p` variable was declared as `struct sched_domain *top_p`, but `sd->parent` is typed as `struct sched_domain __rcu *parent`, so assigning `top_p = top->parent` triggers a sparse warning about incorrect address space assignment (missing `__rcu` annotation).

The Linux Kernel Test Robot (LKP bot) detected these sparse warnings during automated builds and reported them. The key warnings were at `topology.c:2284` and `topology.c:2286`, flagging the assignment of an `__rcu`-annotated pointer to a plain pointer variable.

## Root Cause

The root cause is a code quality issue in the NUMA imbalance calculation loop within `build_sched_domains()`. The original code introduced by commit `e496132ebedd` used two variables to traverse the sched domain hierarchy:

```c
struct sched_domain *top, *top_p;
/* ... */
/* Set span based on the first NUMA domain. */
top = sd;
top_p = top->parent;
while (top_p && !(top_p->flags & SD_NUMA)) {
    top = top->parent;
    top_p = top->parent;
}
imb_span = top_p ? top_p->span_weight : sd->span_weight;
```

Tracing through this code step by step reveals the redundancy:
- Initial state: `top = sd`, `top_p = sd->parent`
- First iteration: `top = sd->parent` (same as current `top_p`), `top_p = sd->parent->parent` (i.e., `top_p->parent`)
- Second iteration: `top = top->parent` (now `sd->parent->parent`, same as current `top_p`), `top_p = top->parent` (i.e., `top_p->parent`)

The variable `top` always ends up equal to `top_p` from before the second assignment in each iteration. After the loop, only `top_p` is used (to read `top_p->span_weight`). The `top` variable is never read after the loop body. Therefore `top` is entirely superfluous.

The second issue is that `struct sched_domain` declares `parent` with an `__rcu` annotation (`struct sched_domain __rcu *parent`), because sched domains can be updated under RCU. When `top_p` is declared as plain `struct sched_domain *top_p`, the assignment `top_p = top->parent` triggers sparse to report: "incorrect type in assignment (different address spaces) — expected `struct sched_domain *top_p`, got `struct sched_domain [noderef] __rcu *parent`."

It is important to note that the runtime behavior of the old (buggy) code and the new (fixed) code is **identical**. Both versions correctly traverse the domain hierarchy upward from `sd` to find the first domain with `SD_NUMA` set in its flags, and both produce the same `imb_span` value. The "bug" is purely a code quality and type correctness issue, not a functional scheduling defect.

## Consequence

There is no observable runtime consequence of this bug. The scheduling behavior, NUMA imbalance calculations, and resulting `imb_numa_nr` values are identical on the buggy and fixed kernels. Tasks are placed and load-balanced identically in both cases.

The only observable consequences are:
1. **Sparse static analysis warnings** during kernel builds with `C=1` (sparse checking enabled). The warnings about incorrect address space assignment (`__rcu` mismatch) clutter build output and may mask other, more serious sparse issues.
2. **Code maintainability concern**: the redundant `top` variable makes the code harder to read and reason about. A future developer might incorrectly believe `top` serves a purpose and introduce a real bug when modifying this loop.

No crashes, hangs, data corruption, priority inversions, starvation, or performance degradation occur due to this issue. There are no stack traces or error messages at runtime—only compile-time/static-analysis-time warnings.

## Fix Summary

The fix commit makes two changes to the NUMA imbalance calculation loop in `build_sched_domains()`:

1. **Removes the redundant `top` variable entirely.** The declaration changes from `struct sched_domain *top, *top_p;` to `struct sched_domain __rcu *top_p;`. The three lines that assigned to or used `top` (`top = sd;`, `top = top->parent;`) are removed.

2. **Annotates `top_p` with `__rcu`** to match the address space of `sd->parent`, silencing the sparse warning.

3. **Simplifies the loop** from using `top` as an intermediary to directly advancing `top_p`:
   ```c
   /* Before (buggy): */
   top = sd;
   top_p = top->parent;
   while (top_p && !(top_p->flags & SD_NUMA)) {
       top = top->parent;
       top_p = top->parent;
   }

   /* After (fixed): */
   top_p = sd->parent;
   while (top_p && !(top_p->flags & SD_NUMA)) {
       top_p = top_p->parent;
   }
   ```

The fix is correct and complete because: (a) `top` was never read outside the loop, so removing it has no effect on the output; (b) `top_p = top_p->parent` produces exactly the same traversal as the original two-line update; and (c) the `__rcu` annotation correctly reflects the RCU-protected nature of `sd->parent`. Valentin Schneider reviewed and confirmed the patch's correctness, noting that the loop indeed requires only one variable.

## Triggering Conditions

This is not a runtime bug, so there are no triggering conditions in the traditional sense. The sparse warnings are triggered under the following compile-time conditions:

- **Build with sparse enabled**: Running `make C=1 CF='-fdiagnostic-prefix -D__CHECK_ENDIAN__'` on the kernel source at or after commit `e496132ebedd` but before the fix.
- **CONFIG_NUMA=y and CONFIG_SCHED_MC=y**: The affected code path in `build_sched_domains()` is compiled when NUMA and multi-level sched domain support are enabled.
- The sparse tool (version 0.6.4 or later) must be installed on the build system.

There are no runtime conditions (number of CPUs, topology, workload, timing, race conditions, etc.) that would cause different scheduling behavior between the buggy and fixed kernels. The code is functionally identical.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

1. **No runtime behavioral difference.** The fundamental reason this bug cannot be reproduced with kSTEP is that there is no runtime scheduling behavior difference between the buggy and fixed kernels. The `top` variable is redundant—both the old and new code compute identical `imb_span` and `imb_numa_nr` values for every sched domain on every CPU. A kSTEP driver that reads `sd->imb_numa_nr` would observe the exact same values on both kernel versions, making it impossible to distinguish buggy from fixed.

2. **The "bug" is a static analysis warning, not a scheduling defect.** The sparse tool (a compile-time semantic checker) reports type annotation mismatches between `__rcu` and non-`__rcu` pointer types. kSTEP tests runtime kernel behavior through a kernel module—it cannot detect compile-time type annotation issues.

3. **The redundant variable is invisible at runtime.** The extra `top` variable is an implementation detail of the `build_sched_domains()` function. It exists only on the stack during sched domain construction (called during boot or CPU hotplug topology rebuilds). There is no exported symbol, sysfs file, tracepoint, or scheduler state field that reflects whether `top` exists or not.

4. **kSTEP cannot invoke sparse or other static analysis tools.** kSTEP operates by loading a kernel module and exercising scheduler paths through task creation, migration, blocking, waking, and ticking. It has no mechanism to perform source code analysis or check for type annotation correctness.

### What would need to be added to kSTEP

No reasonable extension to kSTEP could reproduce this issue. The bug is inherently a compile-time code quality problem:
- Adding a `kstep_check_sparse_warnings()` API makes no sense—sparse operates on source code, not running kernels.
- Adding a `kstep_read_imb_numa_nr(cpu)` API would allow reading the NUMA imbalance values, but these values are identical on both buggy and fixed kernels, so it would not help distinguish them.

### Alternative reproduction methods

The bug can be trivially "reproduced" (i.e., the sparse warning can be observed) by:
1. Checking out the kernel at commit `e496132ebedd`.
2. Running `make C=1 kernel/sched/` with sparse installed.
3. Observing the warnings at `topology.c:2284` and `topology.c:2286`.
4. Applying the fix commit and running sparse again to confirm the warnings are gone.

This is a build-time verification, not a runtime test.
