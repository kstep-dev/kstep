# Agents

## Bug Reproduction

To reproduce a bug that is fixed in commit `[hash]`, follow these steps:

- **Read the commit message:**
   ```sh
   cd linux/master && git show -U32 [hash] | cat
   ```
   If the commit message contains a "Link: <url>", open the URL for further details. For `lore.kernel.org` links, be sure to review the entire discussion (with URL ending in `/T/#u`).

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
   ./run.py [name]
   ```

- **Analyze the logs to understand the bug:**
   ```sh
   cat data/logs/latest.log
   ```

- **Check the fix:**
   - Switch to the Linux source code with the fix.
   - Run your driver again.
   - Review the logs to confirm the bug is resolved.

- **Integrate into Python scripts:**
   - Register the driver in `reproduce.py`, and ensure it is included in the plotting workflow.

## Driver Development

- Prioritize concise, clear code over complex or verbose solutions.

- Fail fast and early: prefer using `panic` to immediately abort the kernel instead of returning an error.

- If a new feature could benefit a wider range of drivers, implement it within the framework rather than in a specific driver.

- To help trace the bug, you may add logging (`printk`) or adapt the kernel source.

- Do not include bug status reporting within the driver code; this will be handled by the Python scripts.
