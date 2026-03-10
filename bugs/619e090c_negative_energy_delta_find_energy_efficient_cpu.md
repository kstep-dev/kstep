# sched/fair: Fix negative energy delta in find_energy_efficient_cpu()

- **Commit:** 619e090c8e409e09bd3e8edcd5a73d83f689890c
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** EXT (Energy-Aware Scheduling)

## Bug Description

The energy-aware CPU selection function `find_energy_efficient_cpu()` computes energy deltas to determine the best CPU for task placement. Due to concurrent updates of utilization signals while evaluating a performance domain, the computed energy delta can become negative (placing the task appears to reduce total energy). This invalid state causes subsequent energy comparisons to be biased and unreliable, leading to incorrect task placement decisions.

## Root Cause

Utilization signals (cpu_util_next, task utilization) are concurrently updated by other CPUs while the function evaluates energy impacts. In rare cases, this race condition causes the computed energy of placing the task on a CPU to be less than the baseline energy (computed without the task), resulting in a negative delta. The original code did not check for this invalid state, allowing the biased delta to influence downstream placement logic.

## Fix Summary

The fix detects when a negative delta occurs (cur_delta < base_energy_pd or prev_delta < base_energy_pd) and immediately returns prev_cpu as a safe fallback. This avoids using invalid energy comparisons for placement decisions. The function also changes the return path to consistently use a `target` variable initialized to `prev_cpu`, eliminating the error return code path (-1) that would have caused suboptimal placement via `select_idle_sibling()`.

## Triggering Conditions

This bug occurs in the energy-aware scheduling (EAS) code path when `find_energy_efficient_cpu()` computes energy deltas. The race condition requires:
- Energy-aware scheduling enabled (`SD_ASYM_CPUCAPACITY` in topology)
- Asymmetric CPU capacity system (big.LITTLE or similar multi-cluster topology)
- Task wakeup triggering energy evaluation across performance domains
- Concurrent utilization updates by other CPUs while energy computation is in progress
- Timing where `compute_energy()` returns a value less than `base_energy_pd` due to stale utilization signals
- The original code lacks negative delta detection, allowing biased energy comparisons

## Reproduce Strategy (kSTEP)

Create asymmetric topology with 4+ CPUs (big.LITTLE configuration) to enable EAS. Setup requires `kstep_topo_set_cls()` and `kstep_cpu_set_capacity()` to create performance domains with different capacities. In `run()`, create multiple tasks on different CPUs to generate utilization load, then repeatedly call `kstep_task_wakeup()` on a task to trigger `find_energy_efficient_cpu()` calls. Use `on_tick_begin()` callback to log energy computation results and detect negative deltas. Monitor for cases where computed energy with task placement is less than baseline energy. The bug manifests as incorrect CPU selection due to invalid negative energy deltas corrupting the energy comparison logic.
