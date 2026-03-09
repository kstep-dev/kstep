# kSTEP: Kernel Scheduler Test and Evaluation Platform 
[![v5.15](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v5.15)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.1](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.1)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.6](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.6)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.12](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.12)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.18](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.18)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)


kSTEP is a framework for reproducing and testing Linux kernel scheduler bugs.

## 🚀 Getting Started

#### 📦 Clone the repository

```sh
git clone --recursive https://github.com/ShawnZhong/kSTEP
```

#### 💾 Install dependencies

```sh
./install_deps.sh
```


#### 🐞 Reproduce known bugs

```sh
./reproduce.py <name|all> [--run <buggy|fixed|plot>]
```

- `<name|all>`: Name of the bug to reproduce (see [`reproduce.py`](reproduce.py)), or `all` to reproduce all bugs.

- `--run`: Choose which version or action to run (`buggy`, `fixed`, or generate a `plot`), default to all.

- **Example:** `./reproduce.py sync_wakeup` checks out both the buggy and fixed kernels, builds kSTEP, runs the `sync_wakeup` driver, and plots the results.

> [!NOTE]
> Reproducing all bugs will require at least 64GB of available disk space.

## 📊 Results

| kSTEP&nbsp;Driver | Figure |
|-----------------------| :--------: |
| **[sync_wakeup.c](kmod/drivers/sync_wakeup.c)** <br> **Official Fix**: [linux@aa3ee4f](https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042) <br> **Our Fix**: [sync_wakeup.patch](linux/sync_wakeup.patch)  <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/sync_wakeup_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/sync_wakeup_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/sync_wakeup.png) |
| **[vruntime_overflow.c](kmod/drivers/vruntime_overflow.c)** <br> **Fix**: [linux@bbce3de](https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/vruntime_overflow_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/vruntime_overflow_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/vruntime_overflow.png) |
| **[freeze.c](kmod/drivers/freeze.c)** <br> **Fix**: [linux@cd9626e](https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/freeze_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/freeze_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/freeze.png) |
| **[extra_balance.c](kmod/drivers/extra_balance.c)** <br> **Fix**: [linux@6d7e478](https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/extra_balance_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/extra_balance_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/extra_balance.png) |
| **[driver_util_avg.c](kmod/drivers/driver_util_avg.c)** <br> **Fix**: [linux@17e3e88](https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/util_avg_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/util_avg_fixed.jsonl) | <img src="https://github.com/SchedStep/results/blob/main/util_avg.png" style="width: 50%;"> |
| **[long_balance.c](kmod/drivers/long_balance.c)** <br> **Fix**: [linux@2feab24](https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/long_balance_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/long_balance_fixed.jsonl) | <img src="https://github.com/SchedStep/results/blob/main/long_balance.png" style="width: 50%;"> |
| **[lag_vruntime.c](kmod/drivers/lag_vruntime.c)** <br> **Fix**: [linux@5068d84](https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/lag_vruntime_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/lag_vruntime_fixed.jsonl) | <img src="https://github.com/SchedStep/results/blob/main/lag_vruntime.png" style="width: 50%;"> |
| **[even_idle_cpu.c](kmod/drivers/even_idle_cpu.c)** <br> **Fix**: [even_idle_cpu.patch](linux/even_idle_cpu.patch) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/even_idle_cpu_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/even_idle_cpu_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/even_idle_cpu.png) |
| **[rt_runtime_toggle.c](kmod/drivers/rt_runtime_toggle.c)** <br> **Fix**: [linux@9b58e97](https://github.com/torvalds/linux/commit/9b58e976b3b391c0cf02e038d53dd0478ed3013c) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/rt_runtime_toggle_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/rt_runtime_toggle_fixed.jsonl) | ![](https://github.com/SchedStep/results/blob/main/rt_runtime_toggle.png) |
| **[uclamp_inversion.c](kmod/drivers/uclamp_inversion.c)** <br> **Fix**: [linux@0213b70](https://github.com/torvalds/linux/commit/0213b7083e81f4acd69db32cb72eb4e5f220329a) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/uclamp_inversion_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/uclamp_inversion_fixed.jsonl) | <img src="https://github.com/SchedStep/results/blob/main/uclamp_inversion.png" style="width: 50%;"> |
| **[h_nr_runnable.c](kmod/drivers/h_nr_runnable.c)** <br> **Fix**: [linux@3429dd5](https://github.com/torvalds/linux/commit/3429dd57f0deb1a602c2624a1dd7c4c11b6c4734) <br> [buggy.jsonl](https://github.com/SchedStep/results/blob/main/h_nr_runnable_buggy.jsonl), [fixed.jsonl](https://github.com/SchedStep/results/blob/main/h_nr_runnable_fixed.jsonl) | <img src="https://github.com/SchedStep/results/blob/main/h_nr_runnable.png" style="width: 50%;"> |

## 💻 Running Your Own Drivers

For driver development, please refer to [AGENTS.md](AGENTS.md) for recommended workflow and tips.

#### 🐧 Checkout Linux source code

```sh
./checkout_linux.py <version> [<name>] [--tarball]
```

- `<version>`: Linux tag (e.g., `v6.14`) or commit hash (e.g., `6d7e478`, `5068d84~1`).

- **Example:** `./checkout_linux.py v6.14 foo_buggy` checks out Linux v6.14 under `linux/foo_buggy`, and symlinks `linux/current` to it.

#### 🛠️ Build Linux and kSTEP
```sh
make linux [LINUX_NAME=<name>]  # Build kernel
make kstep [LINUX_NAME=<name>]  # Build kSTEP rootfs (default target)
```

- `[LINUX_NAME=<name>]`: Name of the Linux directory under `linux/`, default to what `linux/current` points to.

#### 🏃‍♂️ Run kSTEP

```sh
./run.py <driver_name> [--smp <cpus>] [--mem_mb <mb>] [--log_file <path>]
```

- `<driver_name>`: Driver to run (see `*.c` files in the [`kmod/drivers/`](kmod/drivers/) directory).

- `[--log_file <path>]`: Log file to save the output, default to `data/logs/latest.log`.

- **Example:** `./run.py sync_wakeup` runs the `sync_wakeup` driver with default parameters.

## 📁 Directory Structure

- **kmod/**: Kernel module (`kstep.ko`) loaded at boot
  - `driver.c` + `driver_*.c`: Bug-specific drivers that setup and run test cases
  - `driver.h`: Public API for drivers (task creation, ticking, sleeping, cgroups, etc.)
  - `internal.h` and other `*.c` files: Framework primitives and utilities

- **user/**: Minimal userspace (`init.c`) that mounts filesystems and loads `kstep.ko`

- **linux/**: Git worktrees of Linux source
  - `linux/master`: Main clone of Linux kernel
  - `linux/current`: Symlink to active kernel version
  - `linux/*.patch`: Fixes for specific bugs

- **data/**: Data directory
  - `data/rootfs`: Root filesystem images
  - `data/logs`: QEMU log files

- **scripts/**: Python utilities for parsing logs and plotting results
