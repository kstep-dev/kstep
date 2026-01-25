## Bug Reproduction

To reproduce a bug fixed in commit `[hash]`, follow these steps:

#### Planning Stage
- Check out the Linux source code just before the fix: `./checkout_linux.py [hash]~1`.
- Read the commit message and patch carefully: `cd linux/current && git show -U32 [hash]`.
- Be sure to thoroughly understand the bug before writing a driver. Study changed files in the patch, especially those under `linux/current/kernel/sched`. Focus on the changes introduced by the commit. Figure out the call chain that triggers the bug. Understand what each function does and how they interact with each other.
- If the commit message contains links, carefully review the content of ALL links as they may provide the additional information about the bug (especially the reproduction conditions). For LKML links (such as `lore.kernel.org` or `lkml.kernel.org`), extract the email ID and retrieve the message with:  
  `curl -sL https://lore.kernel.org/lkml/<email_id>/t.mbox.gz | gunzip`
- Thoughtfully analyze the conditions and mechanisms that trigger the bug, and plan an effective method to reproduce it within the kernel.

#### Implementation Stage

- Write a driver in `kmod/driver_<name>.c` that reproduces the bug.
- Focus on triggering the bug using the public APIs of the kernel or kSTEP (`kmod/driver.h`). You may add new interfaces to kSTEP if necessary. If this proves difficult, you may temporarily manipulate internal kernel state, but aim to establish an appropriate triggering condition.
- After successfully triggering the bug, demonstrate its impact using observable behavior (such as differences in task scheduling) rather than relying only on internal kernel state.

#### Testing Stage

- Build and run the driver on the buggy kernel:  
  `make linux && make kstep && ./run.py [driver_name]`
- Check if the bug is reproduced by inspecting the logs:  
  `cat data/logs/latest.log`  
  If it is not, revisit your implementation. Incorporate detailed logging in the driver. If necessary, add logging within the kernel itself (e.g., `printk`).
- Once the bug is reliably reproduced, test on the fixed kernel:  
  `./checkout_linux.py [hash] && make linux && make kstep && ./run.py [driver_name]`
- Review the logs to confirm that the bug has been resolved. In rare cases, the bug may persist despite the fix. If you are confident the issue remains, document and report these instances.

#### Final Stage

- Register your driver in `reproduce.py` and ensure it is included in the plotting and analysis workflow.

## Coding Style

- Write clear, concise code. Focus on simple, minimal reproducers that isolate the bug’s core behavior.
- If a new feature would be useful for multiple drivers, add it to the framework itself rather than a specific driver.
- Do not use macros for constant values that are only used once.

## Caveats

- Do not pin tasks to CPU 0, as it is reserved for running the driver. Make sure QEMU is configured with at least 2 CPUs.
