# sched/numa: Fix the potential null pointer dereference in task_numa_work()

- **Commit:** 9c70b2a33cd2aa6a5a59c5523ef053bd42265209
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA

## Bug Description

When running stress-ng-vm-segv test, a null pointer dereference crash occurs in task_numa_work(). The crash happens because after a process unmaps its entire address space via munmap, a pending task_numa_work() callback executes and attempts to scan VMAs. Since all VMAs are gone, vma_next() returns NULL, but the old do-while loop did not check for NULL before dereferencing the VMA pointer in the loop body.

## Root Cause

The original do-while loop structure (`do { ... } for_each_vma(vmi, vma)`) did not validate that vma was non-NULL before entering the loop body. When the VMA iterator reaches the end of the VMA list (or when there are no VMAs), vma_next() returns NULL. The loop attempted to access vma->vm_mm and other fields without first checking if vma was NULL, causing a kernel crash.

## Fix Summary

The fix converts the do-while loop to a for-loop with an explicit NULL check on each iteration: `for (; vma; vma = vma_next(&vmi))`. This ensures vma is verified to be non-NULL before the loop body executes, preventing the null pointer dereference when all VMAs have been unmapped.

## Triggering Conditions

The bug requires NUMA balancing to be enabled and a race between munmap() and task_numa_work(). Specifically:
- NUMA balancing is active (sysctl_numa_balancing_scan_size > 0)
- A process has pending task_numa_work() scheduled for execution  
- Before the callback executes, the process calls munmap() to unmap its entire address space
- The task_numa_work() callback runs after munmap() but before returning to userspace
- When vma_next() is called on an empty VMA list, it returns NULL
- The original do-while loop enters the body and dereferences the NULL vma pointer in vma_migratable()
- This causes a kernel NULL pointer dereference crash in the NUMA balancer code path

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs (CPU 0 reserved for driver). Enable NUMA balancing and create a child task that will trigger the race:
- In setup(): Enable NUMA balancing via kstep_sysctl_write("kernel.numa_balancing", "1")  
- Create a child task with kstep_task_create() and pin it to CPU 1 with kstep_task_pin()
- In run(): Let the child accumulate some runtime to potentially queue task_numa_work()
- Use kstep_tick_repeat() to advance time and allow NUMA balancing work to be scheduled
- Simulate munmap of entire address space by manipulating task's mm_struct (may require direct kernel manipulation)
- Use callbacks like on_tick_end() to detect when task_numa_work() executes  
- Monitor for kernel crashes or NULL pointer access attempts in task_numa_work()
- Success is detected by observing the crash pattern or logging when vma_migratable() is called with NULL vma
