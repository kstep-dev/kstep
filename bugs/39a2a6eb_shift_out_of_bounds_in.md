# sched/fair: Fix shift-out-of-bounds in load_balance()

- **Commit:** 39a2a6eb5c9b66ea7c8055026303b3aa681b49a5
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When `load_balance()` fails repeatedly, the `sd->nr_balance_failed` counter can grow to very high values (syzbot reported 86, 149). This counter is then used as a right-shift operand in the expression `(load >> env->sd->nr_balance_failed)`. When the shift value exceeds or equals the bit-width of the operand (e.g., 64 for unsigned long), the shift operation invokes undefined behavior according to the C standard, potentially causing unpredictable results.

## Root Cause

The code in `detach_tasks()` performs an unbounded right shift where the exponent (`env->sd->nr_balance_failed`) can grow without limit. When this counter grows larger than BITS_PER_TYPE(unsigned long) - 1, the shift operation violates C semantics and produces undefined behavior. There was no bounds checking on the shift operand before use.

## Fix Summary

Introduces a `shr_bound()` macro in kernel/sched/sched.h that safely bounds the shift exponent to BITS_PER_TYPE(typeof(val)) - 1. The macro is applied to the problematic shift operation in `detach_tasks()`, preventing undefined behavior while allowing the counter to grow for its intended algorithmic purpose.

## Triggering Conditions

The bug triggers during load balancing in `detach_tasks()` when:
- Load balance fails repeatedly, causing `sd->nr_balance_failed` to increment without bounds
- Active balance attempts fail due to CPU affinity restrictions (task can't run on `env->dst_cpu`)
- The `nr_balance_failed` counter grows beyond 63 (for 64-bit systems)
- A subsequent `detach_tasks()` call evaluates `(load >> env->sd->nr_balance_failed)` with shift >= 64
- This occurs specifically in the load threshold check before migrating tasks during load balancing
- Multiple scheduling domains with imbalanced loads and restricted task affinities increase likelihood

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver). Strategy:
- **Setup**: Create asymmetric topology with 2 scheduling domains using `kstep_topo_set_cls()` 
- **Tasks**: Create multiple tasks with restrictive CPU affinity using `kstep_task_pin()` to prevent migration
- **Trigger**: Force repeated load balance failures by creating CPU load imbalance and restricting task movement
- **Sequence**: Use `kstep_tick_repeat()` to repeatedly trigger load balancing cycles 
- **Callbacks**: Monitor with `on_sched_balance_selected()` to observe balance attempts
- **Detection**: Add custom logging in driver to track `sd->nr_balance_failed` growth and catch shift operations
- **Verification**: Log when shift exponent exceeds 63 to confirm undefined behavior condition
