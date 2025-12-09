kSTEP
==============

- Install dependencies: `./install_deps.sh`

- Reproduce bugs: `./reproduce.py <bug_name|all> [--run <buggy|fixed|plot>]`

- Checkout Linux source: `./checkout_linux.py <branch|tag|commit> [<name>] [--tarball]`

- Build Linux kernel and kSTEP: `make linux kstep`

- Run: `./run.py [--driver <name>] [--params <param1=value1>...]` (also triggers `make`)

    - `driver`: Driver name to run, see `kmod/driver.c` for available drivers
    - `params`: Additional parameters to pass to the kernel module, see `kmod/main.c` for available parameters
    - Example: `./run.py --driver sync_wakeup`

- Debug: `./run.py --debug` in two different terminals starts QEMU and GDB
