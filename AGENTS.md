# Agents

## Bug Reproduction

To reproduce a bug that is fixed in commit `[hash]`, follow these steps:

- **Read the commit message and discussion:**
   ```sh
   cd linux/master && git show -U32 [hash]
   ```
   If the commit message contains a "Link: <url>", open the URL for further details. 
   For LKML links (e.g., `lore.kernel.org` or `lkml.kernel.org`), extract the email ID from the URL and access the email with the following command:
   ```sh
   curl -sL https://lore.kernel.org/lkml/<email_id>/t.mbox.gz | gunzip
   ```

- **Check out the Linux source code before the fix:**
   ```sh
   ./checkout_linux.py [hash]~1
   cd linux/[hash]~1
   ```
   Carefully review the context of the bug fix.

- **Implement a driver:**
   - Create a driver in `kmod/driver_<name>.c` that reproduces the bug. Use existing drivers as examples for structure and implementation.

   - Begin by triggering the bug in the kernel. Prefer to rely on public APIs in the kernel, available in userspace, or provided by kSTEP (`kmod/driver.h`). If it’s difficult to reproduce the bug naturally, you may temporarily manipulate the kernel internal state and then figure out the proper triggering condition. Always test your driver on both the buggy and fixed kernels to verify the presence of the bug.

   - After successfully triggering the bug, demonstrate its impact. Rather than relying on internal state alone, focus on observable symptoms, such as differences in task scheduling behavior between the buggy and fixed kernels.

   - Write concise, straightforward code. Focus on creating simple reproducers for the bug, avoiding unnecessary complexity.

   - If a new feature could benefit a wider range of drivers, implement it within the framework rather than in a specific driver.

- **Build and run the driver:**
   ```sh
   make linux
   make kstep
   ./run.py [driver_name]
   ```

- **Analyze the logs to understand the bug:**
   ```sh
   cat data/logs/latest.log
   ```

- **Check the fix:**
   - Switch to the Linux source code with the fix by running `./checkout_linux.py [hash]`.
   - Run your driver again.
   - Review the logs to confirm the bug is resolved.

- **Integrate into Python scripts:**
   - Register the driver in `reproduce.py`, and ensure it is included in the plotting workflow.

## Caveats

- Avoid pinning tasks to CPU 0, as it is reserved for running the driver. Therefore, QEMU must be configured with at least 2 CPUs.
