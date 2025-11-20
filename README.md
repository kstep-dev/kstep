kSTEP
==============

- Install dependencies: `./install_deps.sh`

- Reproduce bugs: `./reproduce.py <bug_name|all> [--run <buggy|fixed|plot>]`

- Download Linux kernel: `./checkout_linux.py [--version <branch|tag|commit>]`

- Build Linux kernel: `./make_linux.py [--clean] [--reconfig]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug] [--controller <name>] [--params <param1=value1>...]` (also triggers `make`)

    - `controller`: Controller name to run, see `kmod/controller.c` for available controllers
    - `params`: Additional parameters to pass to the kernel module, see `kmod/main.c` for available parameters
    - Example: `./run_qemu.py --controller cd9626e`

- Debug kernel: `./run_gdb.py`
