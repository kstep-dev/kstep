## Bug Reproduction

To reproduce a bug fixed in commit `[hash]`, follow these steps:

#### Planning Stage
- Check out the Linux source code just before the fix: `./checkout.py [hash]~1 [name]_buggy`.
- Read the commit message and patch carefully: `git -C linux/master show -U32 [hash]`.
- Carefully analyze the changes introduced in the patch, focusing on modifications in `linux/master/kernel/sched`. Trace the call chain that leads to the bug, ensuring you understand the purpose of each function and how they interact. Clearly identify the conditions and system state required to trigger the bug.
- If the commit message contains links, carefully review the content of ALL links as they may provide the additional information about the bug (especially the reproduction conditions). For LKML links (e.g., `lore.kernel.org`, `lkml.kernel.org`, `patch.msgid.link`), extract the email ID and retrieve the message with:  
  `curl -sL https://lore.kernel.org/lkml/<email_id>/t.mbox.gz | gunzip`
- Thoughtfully analyze the conditions and mechanisms that trigger the bug, and plan an effective method to reproduce it within the kernel.

#### Development Stage

- Create a driver in `kmod/drivers_generated/[driver_name].c` that triggers the bug.
- Do not leave the driver broken. There could be other agents running with you.
- Guard the driver with `#if LINUX_VERSION_CODE` to the compatible kernel version. if unsure, just to the current version you are using.
- Try not to directly manipulate internal scheduler state.
- Add detailed logging in your driver for all relevant fields to aid debugging. If necessary, add kernel-side logging with `printk()`.
- Build and execute the driver on the buggy kernel with:
  `./checkout.py [hash]~1 [name]_buggy && make linux LINUX_NAME=[name]_buggy && ./run.py [driver_name] --linux_name [name]_buggy`
- Determine whether the bug is reproduced by examining the output logs:
  `cat data/logs/latest.log`
- If you do not observe the bug, refine your implementation and repeat the process.
- After confirming that your driver triggers the bug, rerun the same driver on the fixed kernel:
  `./checkout.py [hash] [name]_fixed && make linux LINUX_NAME=[name]_fixed && ./run.py [driver_name] --linux_name [name]_fixed`
- Review the logs to ensure the bug no longer occurs. In rare cases where the issue persists despite the fix, provide clear documentation and report your findings.

#### Refinement Stage

- If you manually altered internal scheduler state to trigger the bug during the triggering stage, refine your driver to reproduce the bug using only public kernel APIs or kSTEP interfaces (`kmod/driver.h`). If needed, consider extending kSTEP to provide the required functionality.
- Make sure that the driver is deterministic, and produce the same trace on the same kernel. If not, investigate the root cause of the non-determinism.
- After confirming your driver can trigger the bug, clearly demonstrate its impact through externally observable behavior, such as changes in task scheduling, rather than relying solely on kernel-internal state.
- Once complete, add your driver to BUGS_EXTRA in `reproduce.py` to enable automated testing.

## Coding Style

- Write clear, concise code. Focus on simple, minimal reproducers that isolate the bug’s core behavior.
- If a new feature would be useful for multiple drivers, add it to the framework itself rather than a specific driver.
- Do not use macros for constant values that are only used once.

## Caveats

- Do not pin tasks to CPU 0, as it is reserved for running the driver. Make sure QEMU is configured with at least 2 CPUs.
