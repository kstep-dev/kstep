# Fix wrong negative conversion in find_energy_efficient_cpu()

- **Commit:** da0777d35f47892f359c3f73ea155870bb595700
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** EAS (Energy-Aware Scheduling), CFS

## Bug Description

In find_energy_efficient_cpu(), the calculation `spare_cap = cpu_cap - util` can result in a negative value when cpu_cap is less than util due to RT/DL tasks, IRQ, or thermal pressure reducing the capacity. Since spare_cap is an unsigned long, storing a negative result causes integer underflow, wrapping to a large positive value. This corrupted value is then used in subsequent comparisons (e.g., with fits_capacity()), leading to incorrect CPU selection during task placement.

## Root Cause

The code performs subtraction of two unsigned long values without checking if the result would be negative. When cpu_cap < util, the subtraction wraps around due to unsigned integer arithmetic, producing a large garbage value instead of clamping to zero. This happens because the code assumes cpu_cap will always be greater than or equal to util, which is not guaranteed when higher-priority scheduling classes or hardware constraints reduce effective capacity.

## Fix Summary

The fix replaces the unsafe subtraction with a call to lsub_positive(&spare_cap, util), which safely subtracts util from spare_cap and clamps the result to zero if it would go negative. This ensures correct capacity comparisons and proper CPU selection in the energy-aware scheduling path.

## Triggering Conditions

The bug is triggered in find_energy_efficient_cpu() during CFS task wakeup when Energy-Aware Scheduling (EAS) is enabled. The specific conditions are:
- EAS must be active (requires Energy Model support and !overutilized state)  
- A CFS task wakeup must invoke find_energy_efficient_cpu() for CPU selection
- At least one CPU in a performance domain must have cpu_cap < util, where:
  - cpu_cap = capacity_of(cpu) is reduced by RT/DL tasks, IRQ pressure, or thermal throttling
  - util = cpu_util_next(cpu, p, cpu) includes the waking task's utilization
- The unsigned arithmetic cpu_cap - util wraps to a large positive value
- This corrupted spare_cap is used in subsequent energy calculations and CPU comparisons

## Reproduce Strategy (kSTEP)

Create a heterogeneous CPU topology with asymmetric capacities and trigger capacity pressure:
- Setup: 3+ CPUs with different capacities using kstep_cpu_set_capacity() (CPU 0 reserved)
- Create RT/DL tasks to consume capacity on target CPUs using kstep_task_create() + kstep_task_fifo()
- Pin high-priority tasks to specific CPUs with kstep_task_pin()
- Create a CFS task for wakeup testing
- In run(): Start RT tasks to create capacity pressure, then repeatedly wake the CFS task
- Use on_tick_begin() callback to monitor cpu_util vs capacity_of() for each CPU
- Log spare_cap calculations and detect unsigned underflow (spare_cap > ULONG_MAX/2)
- Trigger find_energy_efficient_cpu() path by ensuring EAS conditions and task wakeups
- Verify bug by checking if spare_cap shows impossibly large values when cpu_cap < util
