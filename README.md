# kSTEP: Kernel Scheduler Test and Evaluation Platform 
[![v5.15](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v5.15)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.1](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.1)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.6](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.6)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.12](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.12)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.18](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.18)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v7.0](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v7.0)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)

Source code for **kSTEP: Characterization and Deterministic Testing of Linux CPU Scheduler Bugs.** (OSDI '26). 

*Tingjia Cao, Shawn Wanxiang Zhong, Caeden Whitaker, Ke Han, Andrea Arpaci-Dusseau, and Remzi Arpaci-Dusseau*

📄 [Paper](https://kstep-dev.github.io/assets/paper-osdi26.pdf) &nbsp;·&nbsp; 💻 [Code (`osdi26`)](https://github.com/kstep-dev/kstep/tree/osdi26) &nbsp;·&nbsp; 🌐 [Website](https://kstep-dev.github.io/) &nbsp;·&nbsp; 📚 [Study](https://github.com/kstep-dev/study) &nbsp;·&nbsp; 📊 [Results](https://github.com/kstep-dev/results)

## 🚀 Getting Started

```sh
# 📦 Clone the repository (add `--branch osdi26` to reproduce the paper exactly)
git clone --recurse-submodules https://github.com/kstep-dev/kstep && cd kstep
```

```sh
# 💾 Install dependencies
./install_deps.sh
```

```sh
# 🐞 Reproduce bugs
# ./kstep.sh reproduce <name|all|extra> [--steps buggy fixed plot]
#   1. Checks out the buggy and fixed kernels
#   2. Builds and runs the bug's driver on each
#   3. Plots the two traces
./kstep.sh reproduce sync_wakeup
```

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
| **[even_idle_cpu.c](kmod/drivers/new_even_idle_cpu.c)** <br> **Fix**: [even_idle_cpu.patch](linux/even_idle_cpu.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_even_idle_cpu/plot.png) |
| **[local_group_imbalance.c](kmod/drivers/new_local_group_imbalance.c)** <br> **Fix**: [fix_local_group_imbalanced.patch](linux/fix_local_group_imbalanced.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_local_group_imbalance/plot.png) |
| **[util_avg_jump.c](kmod/drivers/new_util_avg_jump.c)** <br> **Fix**: [fix_util_avg_jump.patch](linux/fix_util_avg_jump.patch) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_util_avg_jump/plot.png" style="width: 50%;"> |
| **[rt_runtime_toggle.c](kmod/drivers/rt_runtime_toggle.c)** <br> **Fix**: [linux@9b58e97](https://github.com/torvalds/linux/commit/9b58e976b3b391c0cf02e038d53dd0478ed3013c) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/fixed/kstep.jsonl) | ![](https://raw.githubusercontent.com/kstep-dev/results/main/repro_rt_runtime_toggle/plot.png) |
| **[uclamp_inversion.c](kmod/drivers/uclamp_inversion.c)** <br> **Fix**: [linux@0213b70](https://github.com/torvalds/linux/commit/0213b7083e81f4acd69db32cb72eb4e5f220329a) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_uclamp_inversion/plot.png" style="width: 50%;"> |
| **[h_nr_runnable.c](kmod/drivers/h_nr_runnable.c)** <br> **Fix**: [linux@3429dd5](https://github.com/torvalds/linux/commit/3429dd57f0deb1a602c2624a1dd7c4c11b6c4734) <br> [buggy.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/buggy/kstep.jsonl), [fixed.jsonl](https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/fixed/kstep.jsonl) | <img src="https://raw.githubusercontent.com/kstep-dev/results/main/repro_h_nr_runnable/plot.png" style="width: 50%;"> |

## 💻 Running Your Own Drivers

For driver development, please refer to [AGENTS.md](AGENTS.md) for recommended workflow and tips.

#### 🐧 Checkout Linux source code

```sh
./kstep.sh checkout <ref> [<name>] [--git] [--patch <file>] [--keep-current]
```

- `<ref>`: Linux tag (e.g., `v6.14`) or commit hash (e.g., `6d7e478`, `5068d84~1`).
- Default: download a tarball from kernel.org / GitHub (fast, one-shot). `--git`: add a worktree of `build/master` (multi-version dev, supports `git log`/`git diff`).
- **Example:** `./kstep.sh checkout v6.14 foo_buggy` checks out Linux v6.14 under `build/foo_buggy/linux/` and points `build/current` at `build/foo_buggy/`.

#### 🛠️ Build kSTEP
```sh
./kstep.sh build [<name>]                      # kmod + user + rootfs.cpio; builds the kernel first if needed
./kstep.sh build [<name>] --linux [--config F]  # reconfigure and rebuild the kernel; run after Linux file changes
```

- `[<name>]`: build directory under `build/`; defaults to whatever `build/current` points to.

#### 🏃‍♂️ Run kSTEP

```sh
./kstep.sh run [<name>] [<driver>] [--num-cpus <n>] [--mem-mb <mb>] [-o <dir>] [-i <file>]
```

- `[<name>]`: kernel build to run against (defaults to `build/current`). A bug's build, `<bug>_buggy` or `<bug>_fixed`, brings the bug's driver and machine from `bugs.yaml`.
- `[<driver>]`: driver to run (see `*.c` files in [`kmod/drivers/`](kmod/drivers/)); defaults to `cli`, an interactive session: type commands (listed at the top of [`kmod/cli.c`](kmod/cli.c)), see the machine after each. `-i <file>` or a pipe scripts one.
- `[-o <dir>]`: subdir under `results/` for output; defaults to a timestamped `tmp_*` dir. `results/latest` symlinks to it.
- `--debug` starts the guest stopped with a gdb stub; `./kstep.sh gdb [<name>]` attaches.

- **Example:** `./kstep.sh run sync_wakeup_buggy` runs the `sync_wakeup` driver on its buggy kernel.

## 📁 Directory Structure

- **kmod/**: Kernel module (`kmod.ko`) loaded at boot
  - `drivers/`: bug-specific drivers (one `.c` per bug)
  - `checkers/`: rules over scheduler state and decisions, enabled by the cli's `check` verb
  - `cli.c`: interactive driver behind the website playground (text commands in and JSON replies out on the virtio console port)
  - `cpu.c`: topology, capacity and frequency setup, behind the `cpu-topo`/`cpu-cap`/`cpu-freq` cli verbs
  - `driver.h`: public API for drivers (task creation, ticking, cgroups, etc.)
  - `internal.h` and other top-level `*.c`: framework primitives

- **user/**: Minimal userspace (`user.c`) that mounts filesystems and loads `kmod.ko`

- **linux/**: Project-static kernel files (committed to git)
  - `config.kstep*`: Kconfig fragments merged into the build
  - `cov.c`, `Kconfig.kstep`, `Makefile.kstep`: scheduler-coverage instrumentation
  - `*.patch`: Fixes for specific bugs

- **build/**: Per-kernel build artifacts (gitignored, regenerable)
  - `current`: symlink to the active `<name>/` (set by `kstep checkout`)
  - `master/`: kernel clone reused by `kstep checkout --git`
  - `user`: statically linked userspace binary
  - `<name>/`: `kernel` (the image QEMU boots: the bzImage on x86, the Image on arm64; plus `vmlinux` for gdb/addr2line) and `rootfs.cpio` (kmod.ko + user); `linux/` source tree; `kmod/` module build dir with `kmod.ko` and the clangd `compile_commands.json` (the project root symlinks to it)

- **results/**: Run outputs. See [`results/README.md`](https://github.com/kstep-dev/results). `repro_<bug>/` is tracked; `tmp_*` are gitignored. The fuzzer's per-client runs land in `fuzz_<build>_<n>/`.

- **crates/**: the Rust tooling. `core/` is what the website's wasm decoder shares with the binaries (the `kmod/shm.h` decoder, generated from the header, and the QEMU command line); `kstep/` is the `kstep` command (`./kstep.sh`): checkout, build, run, reproduce, web; `fuzz/` is the LibAFL fuzzer (`./kstep-fuzz.sh <bug>`), a host-side client of the `cli` driver, with corpora and findings under `fuzz/` (gitignored)
  - the earlier in-guest fuzzer is kept for reference in [`docs/archive/old_fuzzer/`](docs/archive/old_fuzzer/)

- **scripts/**: the plot scripts (Python, self-contained: `uv run --script scripts/plot_<format>.py <bug>`), which `kstep reproduce` runs.

- **bugs.yaml**: one entry per bug -- how to build, run, reproduce and fuzz it; read by `kstep run`, `kstep reproduce` and the fuzzer
