# Core: sched_switch tracepoint argument reordering breaks BPF programs

**Commit:** `9c2136be0878c88c53dea26943ce40bb03ad8d8d`
**Affected files:** kernel/sched/core.c, include/trace/events/sched.h
**Fixed in:** v5.18-rc7
**Buggy since:** v5.18-rc1 (introduced by `fa2c3254d7cf` "sched/tracing: Don't re-read p->state when emitting sched_switch event")

## Bug Description

Commit `fa2c3254d7cf` was a legitimate fix for a race condition in the `sched_switch` tracepoint: due to concurrent `ttwu()` execution, `p->__state` could be overwritten to `TASK_WAKING` between `deactivate_task()` and `trace_sched_switch()` inside `__schedule()`. The fix added a new `prev_state` argument to the `sched_switch` tracepoint so that the state captured earlier in `__schedule()` (before the race window) would be passed directly to the trace event, rather than re-reading from the live `p->__state` field.

However, the new `prev_state` argument (`unsigned int`) was inserted as the **second** positional parameter in `TP_PROTO()`, placed *before* the `struct task_struct *prev` pointer. This changed the raw tracepoint's ABI: what was previously the second argument (a `struct task_struct *prev`) became the third argument, and the second argument became an `unsigned int prev_state`.

This reordering broke all existing BPF programs that attach to the raw `sched_switch` tracepoint (e.g., `tp_btf/sched_switch` programs). These programs access tracepoint arguments by positional index. A BPF program that previously accessed `ctx[1]` expecting a `struct task_struct *prev` would now get an `unsigned int prev_state` value instead. The BPF verifier would reject such programs because the type of the second argument no longer matches `struct task_struct *`, causing `bpf_task_storage` access and other task-struct-based operations to fail verification.

The fix commit (`9c2136be0878`) moves the `prev_state` argument to the **end** of the argument list instead, preserving backward compatibility. Existing BPF programs that only access the original positional arguments (`preempt`, `prev`, `next`) continue to work without modification, while new programs can optionally extract `prev_state` as an additional trailing argument on supported kernel versions.

## Root Cause

The root cause is a tracepoint ABI break introduced by the argument ordering choice in commit `fa2c3254d7cf`. The `sched_switch` tracepoint definition in `include/trace/events/sched.h` was changed from:

```c
TP_PROTO(bool preempt,
         struct task_struct *prev,
         struct task_struct *next)
```

to:

```c
TP_PROTO(bool preempt,
         unsigned int prev_state,
         struct task_struct *prev,
         struct task_struct *next)
```

The insertion of `prev_state` before `prev` shifted the positional indices of all subsequent arguments. BPF raw tracepoints (specifically `tp_btf` programs) bind to tracepoint arguments by position and type. The BPF verifier checks argument types at load time — a program that expects `ctx->args[1]` to be a `struct task_struct *` would now see `unsigned int prev_state` at that position, causing a type mismatch.

The call site in `kernel/sched/core.c` in `__schedule()` was:

```c
trace_sched_switch(sched_mode & SM_MASK_PREEMPT, prev_state, prev, next);
```

Every consumer of the `sched_switch` tracepoint — including `ftrace_graph_probe_sched_switch()`, `ftrace_filter_pid_sched_switch_probe()`, `event_filter_pid_sched_switch_probe_pre()`, `event_filter_pid_sched_switch_probe_post()`, `trace_sched_switch_callback()` in osnoise, `probe_sched_switch()`, `probe_wakeup_sched_switch()`, and the custom trace event sample — all had their function signatures updated to match the new argument order. This means the in-kernel consumers worked correctly, but the external BPF consumer interface was broken.

The fundamental issue is that Linux tracepoints serve as a stable ABI for BPF programs. While the kernel-internal tracepoint consumer signatures can be updated atomically in a single commit, BPF programs are compiled and loaded separately. Inserting a new argument in the middle of an existing tracepoint's argument list violates the implicit stability contract for BPF raw tracepoint consumers.

## Consequence

The primary consequence is that all existing BPF programs using the `tp_btf/sched_switch` raw tracepoint fail to load on kernels with the buggy argument order. Specifically:

1. **BPF verification failure**: Programs accessing `ctx->args[1]` as `struct task_struct *` (previously the `prev` pointer) would be rejected by the BPF verifier because the argument is now `unsigned int prev_state`. This prevents any `bpf_task_storage_get()`, `bpf_task_storage_delete()`, or any other BPF helper that takes a `task_struct` pointer from working with what was previously the `prev` argument.

2. **Silent data misinterpretation**: BPF programs using `BPF_PROG()` or `SEC("tp_btf/sched_switch")` that were compiled against the old argument layout but manage to bypass type checking (e.g., through raw `bpf_probe_read`) would silently interpret the `unsigned int prev_state` value as a pointer, leading to incorrect data or potential crashes in the BPF program (not the kernel, since BPF memory access is verified).

3. **Tooling breakage**: Production BPF-based tracing tools (such as those built with libbpf, bpftrace, or BCC) that hook `sched_switch` for context switch analysis, latency profiling, or task state tracking would need to be rewritten to accommodate the new argument ordering. This is particularly disruptive because the `sched_switch` tracepoint is one of the most commonly used tracepoints in BPF-based observability tools.

There is no scheduler correctness impact — the scheduler continues to function identically. The bug is purely in the tracepoint ABI interface layer.

## Fix Summary

The fix in commit `9c2136be0878` moves the `prev_state` argument from the second position to the **last** position in the tracepoint's `TP_PROTO`:

Before (buggy):
```c
TP_PROTO(bool preempt,
         unsigned int prev_state,
         struct task_struct *prev,
         struct task_struct *next)
```

After (fixed):
```c
TP_PROTO(bool preempt,
         struct task_struct *prev,
         struct task_struct *next,
         unsigned int prev_state)
```

This preserves backward compatibility: existing BPF programs that access `ctx->args[0]` (preempt), `ctx->args[1]` (prev), and `ctx->args[2]` (next) continue to see the same types at the same positions. The new `prev_state` argument is appended at `ctx->args[3]` and can be optionally accessed by programs compiled for newer kernels.

The fix updates all eight in-kernel tracepoint consumers (`ftrace_graph_probe_sched_switch`, `ftrace_filter_pid_sched_switch_probe`, `event_filter_pid_sched_switch_probe_pre`, `event_filter_pid_sched_switch_probe_post`, `trace_sched_switch_callback`, `probe_sched_switch`, `probe_wakeup_sched_switch`, and the custom trace event sample) to match the new argument order. The call site in `__schedule()` is updated accordingly:

```c
trace_sched_switch(sched_mode & SM_MASK_PREEMPT, prev, next, prev_state);
```

This fix is correct and complete because it restores the original argument positions while still carrying the `prev_state` value that was needed to fix the original race condition. The appended argument approach is the standard pattern for extending tracepoints in a backward-compatible manner.

## Triggering Conditions

The bug is triggered under the following conditions:

1. **Kernel version**: Any kernel between v5.18-rc1 (which contains `fa2c3254d7cf`) and v5.18-rc6 (just before this fix was applied).

2. **BPF program loading**: A BPF program of type `BPF_PROG_TYPE_TRACING` with attach type `BPF_TRACE_RAW_TP` targeting the `sched_switch` tracepoint must be loaded. The program must reference the second argument as a `struct task_struct *` (which was the convention before `fa2c3254d7cf`).

3. **BPF verifier**: The BPF verifier must perform BTF-based type checking on the raw tracepoint arguments. When the program attempts to use the second argument as `struct task_struct *`, the verifier will reject it because BTF now declares the second argument as `unsigned int`.

4. **No special hardware or topology**: The bug is triggered purely by loading a BPF program; no special workload, CPU count, or configuration is needed beyond having BPF support enabled (`CONFIG_BPF_SYSCALL=y`, `CONFIG_BPF_JIT=y`, `CONFIG_DEBUG_INFO_BTF=y`).

The bug is 100% deterministic — every attempt to load an affected BPF program will fail verification on the buggy kernel. There is no race condition or timing dependency for reproducing the BPF breakage itself.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

The bug is a **tracepoint ABI compatibility issue** affecting BPF programs, not a scheduler logic or behavioral bug. The scheduler itself functions identically regardless of tracepoint argument ordering — no scheduling decision, task state, timing, fairness, or performance metric is affected.

To reproduce this bug, one must:
- Load a BPF program that attaches to the `tp_btf/sched_switch` raw tracepoint
- Verify that the BPF verifier rejects the program due to argument type mismatch at position 2
- Or demonstrate that a BPF program misinterprets argument data due to the shifted positions

kSTEP operates as a kernel module within QEMU. It controls tasks, triggers ticks, and observes scheduler state through direct access to kernel data structures. It has **no capability** to:
- Load BPF programs via the `bpf()` syscall
- Interact with the BPF verifier
- Attach BPF probes to tracepoints
- Verify tracepoint argument type matching
- Observe tracepoint ABI changes from within the kernel

The bug has zero impact on any observable scheduler behavior that kSTEP can measure (task placement, vruntime, load, fairness, context switches, etc.).

### 2. What would need to be added to kSTEP

Reproducing this bug would require fundamentally new capabilities:
- **BPF program loading**: A `kstep_bpf_load(prog_bytes, prog_len)` API that invokes the kernel's BPF syscall path to load a BPF program, including BTF validation and verifier execution.
- **BPF tracepoint attachment**: A `kstep_bpf_attach_tp(prog_fd, "sched_switch")` API to attach a loaded BPF program to a raw tracepoint.
- **BPF verifier result inspection**: A way to check whether the BPF verifier accepted or rejected the program, and with what error message.
- **Userspace syscall capability**: The `bpf()` syscall is a userspace interface; kSTEP would need a way to issue syscalls from within the kernel module context, which is architecturally contrary to how kernel modules operate.

These are **fundamental** additions far outside kSTEP's architecture, which is designed for scheduler behavior observation, not BPF/tracing subsystem testing.

### 3. Alternative reproduction methods

The bug can be reproduced outside kSTEP using:

1. **Direct BPF program loading**: Write a minimal `tp_btf/sched_switch` BPF program using libbpf that accesses the second argument as `struct task_struct *`. Compile it against a pre-`fa2c3254d7cf` kernel's BTF, then attempt to load it on a kernel with `fa2c3254d7cf` applied but without `9c2136be0878`. The BPF verifier will reject the program.

2. **bpftrace one-liner**: Use bpftrace to attach to `rawtracepoint:sched_switch` and attempt to access the arguments by position. On the buggy kernel, the second positional argument will be `unsigned int` instead of `struct task_struct *`.

3. **Kernel self-test**: The kernel BPF selftests (`tools/testing/selftests/bpf/`) include programs that attach to `sched_switch`. Running these tests on a buggy kernel would show verification failures.

4. **Manual BTF inspection**: Use `bpftool btf dump` to inspect the kernel's BTF and verify that the `sched_switch` tracepoint's second argument type changed from `struct task_struct *` to `unsigned int`.
