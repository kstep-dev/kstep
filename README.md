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
