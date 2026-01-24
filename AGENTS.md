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
   - Write a driver in `kmod/driver_<name>.c` to trigger the bug.
   - Review examples of existing drivers in the `kmod/` directory and consult the "Driver Development" section below for additional guidance.

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

## Driver Development

- Write concise, straightforward code. Focus on creating the simplest possible reproducer for the bug, avoiding unnecessary complexity.

- Fail fast and early: prefer using `panic` to immediately abort the kernel instead of returning an error.

- If a new feature could benefit a wider range of drivers, implement it within the framework rather than in a specific driver.

- To help trace the bug, you may add logging (`printk`) or adapt the kernel source.

- Do not include bug status reporting within the driver code; this will be handled by the Python scripts.

- Checking only internal kernel state (e.g., `rq->cfs.h_nr_running`) is insufficient to confirm a bug. You MUST also verify observable symptoms, such as changes in task scheduling behavior between the buggy and fixed kernels.

## Caveats

- Avoid pinning tasks to CPU 0, as it is reserved for running the driver. Therefore, QEMU must be configured with at least 2 CPUs.
