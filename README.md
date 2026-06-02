# kSTEP: Kernel Scheduler Test and Evaluation Platform 
[![v5.15](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v5.15)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.1](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.1)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.6](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.6)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.12](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.12)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.18](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.18)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v7.0](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v7.0)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)

kSTEP is a framework for reproducing and testing Linux kernel scheduler bugs.

## 🚀 Getting Started

#### 📦 Clone the repository

```sh
git clone --recurse-submodules https://github.com/kstep-dev/kstep
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

| kSTEP&nbsp;Driver,&nbsp;Fix,&nbsp;and&nbsp;Output | Figure |
|-----------------------| :--------: |
| **[sync_wakeup.c](kmod/drivers/sync_wakeup.c)** <br> **Official Fix**: [linux@aa3ee4f](https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042) <br> **Our Fix**: [sync_wakeup.patch](linux/sync_wakeup.patch)  <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_sync_wakeup/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_sync_wakeup/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_sync_wakeup/plot.png) |
| **[vruntime_overflow.c](kmod/drivers/vruntime_overflow.c)** <br> **Fix**: [linux@bbce3de](https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_vruntime_overflow/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_vruntime_overflow/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_vruntime_overflow/plot.png) |
| **[freeze.c](kmod/drivers/freeze.c)** <br> **Fix**: [linux@cd9626e](https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_freeze/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_freeze/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_freeze/plot.png) |
| **[extra_balance.c](kmod/drivers/extra_balance.c)** <br> **Fix**: [linux@6d7e478](https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_extra_balance/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_extra_balance/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_extra_balance/plot.png) |
| **[driver_util_avg.c](kmod/drivers/util_avg.c)** <br> **Fix**: [linux@17e3e88](https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg/plot.png" style="width: 50%;"> |
| **[long_balance.c](kmod/drivers/long_balance.c)** <br> **Fix**: [linux@2feab24](https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_long_balance/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_long_balance/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_long_balance/plot.png" style="width: 50%;"> |
| **[lag_vruntime.c](kmod/drivers/lag_vruntime.c)** <br> **Fix**: [linux@5068d84](https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_lag_vruntime/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_lag_vruntime/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_lag_vruntime/plot.png" style="width: 50%;"> |
| **[even_idle_cpu.c](kmod/drivers_new_bugs/even_idle_cpu.c)** <br> **Fix**: [even_idle_cpu.patch](linux/even_idle_cpu.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/plot.png) |
| **[local_group_imbalance.c](kmod/drivers_new_bugs/local_group_imbalance.c)** <br> **Fix**: [fix_local_group_imbalanced.patch](linux/fix_local_group_imbalanced.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/plot.png) |
| **[util_avg_jump.c](kmod/drivers_new_bugs/util_avg_jump.c)** <br> **Fix**: [fix_util_avg_jump.patch](linux/fix_util_avg_jump.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/plot.png" style="width: 50%;"> |
| **[rt_runtime_toggle.c](kmod/drivers/rt_runtime_toggle.c)** <br> **Fix**: [linux@9b58e97](https://github.com/torvalds/linux/commit/9b58e976b3b391c0cf02e038d53dd0478ed3013c) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/plot.png) |
| **[uclamp_inversion.c](kmod/drivers/uclamp_inversion.c)** <br> **Fix**: [linux@0213b70](https://github.com/torvalds/linux/commit/0213b7083e81f4acd69db32cb72eb4e5f220329a) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/plot.png" style="width: 50%;"> |
| **[h_nr_runnable.c](kmod/drivers/h_nr_runnable.c)** <br> **Fix**: [linux@3429dd5](https://github.com/torvalds/linux/commit/3429dd57f0deb1a602c2624a1dd7c4c11b6c4734) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/plot.png" style="width: 50%;"> |

## 💻 Running Your Own Drivers

For driver development, please refer to [AGENTS.md](AGENTS.md) for recommended workflow and tips.

#### 🐧 Checkout Linux source code

```sh
./checkout.py <version> [<name>] [--tar | --git]
```

- `<version>`: Linux tag (e.g., `v6.14`) or commit hash (e.g., `6d7e478`, `5068d84~1`).
- `--tar` (default): download tarball from kernel.org / GitHub (fast, one-shot).
- `--git`: add a worktree from `build/master` (multi-version dev, supports `git log`/`git diff`).

- **Example:** `./checkout.py v6.14 foo_buggy` checks out Linux v6.14 under `build/foo_buggy/linux/` and points `build/current` at `build/foo_buggy/`.

#### 🛠️ Build kSTEP
```sh
make [KERNEL=<name>]  # Build kSTEP rootfs (kmod + user). Trigger `make linux` on first build.
make linux [KERNEL=<name>]  # Full kernel build. Run this after Linux file changes.
```

- `[KERNEL=<name>]`: build directory name under `build/`; defaults to whatever `build/current` points to.

#### 🏃‍♂️ Run kSTEP

```sh
./run.py <name> [--num_cpus <n>] [--mem_mb <mb>] [--kernel <name>] [--label <dir>]
```

- `<name>`: Driver to run (see `*.c` files in [`kmod/drivers/`](kmod/drivers/) and [`kmod/drivers_new_bugs/`](kmod/drivers_new_bugs/)).
- `[--kernel <name>]`: kernel build to run against (defaults to `build/current`).
- `[--label <dir>]`: subdir under `results/` for output; defaults to a timestamped `tmp_*` dir. `results/latest` symlinks to it.
- See `./run.py --help` for `--topology`, `--frequency`, `--capacity`, `--debug`, etc.

- **Example:** `./run.py sync_wakeup` runs the `sync_wakeup` driver with default parameters.

## 📁 Directory Structure

- **kmod/**: Kernel module (`kmod.ko`) loaded at boot
  - `drivers/`, `drivers_new_bugs/`: bug-specific drivers (one `.c` per bug)
  - `fuzz/`: fuzz executor, op handlers, coverage, sanity checks
  - `cpu.c`: topology, capacity, frequency setup
  - `driver.h`: public API for drivers (task creation, ticking, cgroups, etc.)
  - `internal.h` and other top-level `*.c`: framework primitives

- **user/**: Minimal userspace (`user.c`) that mounts filesystems and loads `kmod.ko`

- **linux/**: Project-static kernel files (committed to git)
  - `config.kstep*`: Kconfig fragments merged into the build
  - `cov.c`, `Kconfig.kstep`, `Makefile.kstep`: scheduler-coverage instrumentation
  - `*.patch`: Fixes for specific bugs

- **build/**: Per-kernel build artifacts. See [`build/README.md`](https://github.com/kstep-dev/build) for the full layout. Top-level: `current` (symlink), `user` (kernel-agnostic userspace binary), `<KERNEL>/` per-checkout dirs containing the boot `kernel`, `rootfs.cpio`, and `linux/` source tree.

- **results/**: Run outputs. See [`results/README.md`](https://github.com/kstep-dev/results). `repro_<bug>/` and `fuzz_<bug>/` are tracked; `tmp_*` are gitignored.

- **scripts/**: Python utilities for fuzz orchestration, coverage parsing, and plotting.
