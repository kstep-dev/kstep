# kSTEP: Kernel Scheduler Test and Evaluation Platform 
[![v5.15](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v5.15)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.1](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.1)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.6](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.6)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.12](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.12)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)
[![v6.18](https://img.shields.io/badge/github-passing-34D058?logo=github&label=v6.18)](https://github.com/ShawnZhong/kSTEP/actions/workflows/ci.yml)


## 🚀 Getting Started

#### 📦 Clone the repository

```sh
git clone --recursive https://github.com/ShawnZhong/kSTEP
```

#### 💾 Install dependencies

```sh
./install_deps.py
```


#### 🐞 Reproduce known bugs

```sh
./reproduce.py <name|all> [--run <buggy|fixed|plot>]
```

- `<name|all>`: Name of the bug to reproduce (see [`reproduce.py`](reproduce.py)), or `all` to reproduce all bugs.

- `--run`: Choose which version or action to run (`buggy`, `fixed`, or generate a `plot`), default to all.

- **Example:** `./reproduce.py sync_wakeup` checks out both the buggy and fixed kernels, builds kSTEP, runs the `sync_wakeup` driver, and plots the results.

## 📊 Results

#### Bug 1 & 8

[Official Fix: aa3ee4f](https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042) |
[Our Fix: sync_wakeup.patch](linux/sync_wakeup.patch) |
[buggy.log](https://github.com/SchedStep/results/blob/main/sync_wakeup_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/sync_wakeup_fixed.log)

![](https://github.com/SchedStep/results/blob/main/sync_wakeup.png)

#### Bug 2

[bbce3de](https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8) |
[buggy.log](https://github.com/SchedStep/results/blob/main/vruntime_overflow_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/vruntime_overflow_fixed.log)

![](https://github.com/SchedStep/results/blob/main/vruntime_overflow.png)

#### Bug 3

[cd9626e](https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d) |
[buggy.log](https://github.com/SchedStep/results/blob/main/freeze_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/freeze_fixed.log)

![](https://github.com/SchedStep/results/blob/main/freeze.png)

#### Bug 4

[6d7e478](https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3) |
[buggy.log](https://github.com/SchedStep/results/blob/main/extra_balance_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/extra_balance_fixed.log)

![](https://github.com/SchedStep/results/blob/main/extra_balance.png)

#### Bug 5

[17e3e88](https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5) |
[buggy.log](https://github.com/SchedStep/results/blob/main/util_avg_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/util_avg_fixed.log)

#### Bug 6

[2feab24](https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a) |
[buggy.log](https://github.com/SchedStep/results/blob/main/long_balance_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/long_balance_fixed.log)

#### Bug 7

[5068d84](https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1) |
[buggy.log](https://github.com/SchedStep/results/blob/main/lag_vruntime_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/lag_vruntime_fixed.log)

<p align="center">
<img src="https://github.com/SchedStep/results/raw/main/lag_vruntime.png" style="width: 50%;">
</p>

#### Bug 9

[even_idle_cpu.patch](linux/even_idle_cpu.patch) |
[buggy.log](https://github.com/SchedStep/results/blob/main/even_idle_cpu_buggy.log) |
[fixed.log](https://github.com/SchedStep/results/blob/main/even_idle_cpu_fixed.log)

![](https://github.com/SchedStep/results/blob/main/even_idle_cpu.png)

## 💻 Developing Your Own Drivers

#### 📂 Checkout Linux source code

```sh
./checkout_linux.py <tag|commit> [<name>] [--tarball]
```

- `<tag|commit>`: Linux branch or commit hash to checkout (e.g., `v6.17`, `6d7e478`).

- **Example:** `./checkout_linux.py v6.14` checks out Linux v6.14 under `linux/v6.14`, and symlinks `linux/current` to it.

#### 🛠️ Build Linux and kSTEP
```sh
make linux kstep
```
- The default target is `kstep`. See [`Makefile`](Makefile) for a full list of build targets.

#### 🏃‍♂️ Run kSTEP

```sh
./run.py <name> [--smp <num_cpus>] [--mem_mb <mem_mb>] [--log_file <path>] [--debug]
```

- `<name>`: Driver to run (see [`kmod/driver.c`](kmod/driver.c)).

- `--debug`: Enables kernel debugging. Run the command in two separate terminals.

- **Example:** `./run.py sync_wakeup` runs the `sync_wakeup` driver with default settings.
