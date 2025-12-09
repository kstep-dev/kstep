kSTEP
==============

- Install dependencies: `./install_deps.sh`

- Reproduce bugs: `./reproduce.py <bug_name|all> [--run <buggy|fixed|plot>]`

- Download Linux kernel: `./checkout_linux.py [--version <branch|tag|commit>]`

- Build Linux kernel and kSTEP: `make linux kstep`

- Run kernel: `./run_qemu.py [--debug] [--driver <name>] [--params <param1=value1>...]` (also triggers `make`)

    - `driver`: Driver name to run, see `kmod/driver.c` for available drivers
    - `params`: Additional parameters to pass to the kernel module, see `kmod/main.c` for available parameters
    - Example: `./run_qemu.py --driver sync_wakeup`

- Debug kernel: `./run_gdb.py`
