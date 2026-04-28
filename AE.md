# Artifact of kSTEP
This repository contains the source code for

**kSTEP: Characterization and Controlled Testing of Linux CPU Scheduler Bugs (OSDI '26)**

## Overview

The instructions will reproduce the key results in Figure 14 (reproduced bugs by kSTEP) and Figure 15 (a new bug found by kSTEP).

The entire artifact evaluation process can take ~30 minutes.

## Environment

We reserved three servers ([c220g5](https://docs.cloudlab.us/hardware.html#(part._cloudlab-wisconsin)) in Cloudlab), one for each reviewer. Please add your SSH public key to the [spreadsheet](https://docs.google.com/spreadsheets/d/1HB2LAww1IrGjMe0bNfvnsGPhwDcuICfwcPPICZBJEpE/edit?usp=sharing ) next to the IP address of the server you will use.

Inside each server, you can access the kSTEP repo with

```bash
cd ~/project/kSTEP
```

## Evaluation instructions
### Install dependencies
```bash
./install_deps.sh
```
### Install Python packages
```bash
uv sync
source .venv/bin/activate
```
### Reproduce Figure 14 (reproduced bugs by kSTEP) 
```bash
# Figure 14.1
./reproduce.py sync_wakeup
# Figure 14.2
./reproduce.py vruntime_overflow
# Figure 14.3
./reproduce.py freeze
# Figure 14.4
./reproduce.py extra_balance
# Figure 14.5 
./reproduce.py driver_util_avg
# Figure 14.6 
./reproduce.py long_balance
# Figure 14.7
./reproduce.py lag_vruntime
```
The results are saved at ``~/project/kSTEP/results/repro_{bug_name}/plot.pdf``. You can download the plots to review them.

Note: each command may take a while to finish because it checks out and compiles both the buggy and fixed versions of the Linux source code.

### Reproduce Figure 15 (a new bug found by kSTEP)
```bash
# Figure 15.2
./reproduce.py even_idle_cpu
```
Similarly, the result is saved at ``~/project/kSTEP/results/repro_even_idle_cpu/plot.pdf``.

## Directory Structure

- **kmod/**: Kernel module (`kstep.ko`) loaded at boot
  - `driver.c` + `driver/*.c`: Bug-specific drivers that setup and run test cases
  - `driver.h`: Public API for drivers (task creation, ticking, sleeping, cgroups, etc.)
  - `internal.h` and other `*.c` files: Framework primitives and utilities

- **user/**: Minimal userspace (`init.c`) that mounts filesystems and loads `kstep.ko`

- **scripts/**: Python utilities for parsing logs and plotting results
