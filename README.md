kSTEP
==============

- Install dependencies: `./install_deps.sh`

- Reproduce bugs: `./reproduce.py [--controller <bug_name|all>] [--run <buggy|fixed|plot>]`

- Download Linux kernel: `./checkout_linux.py [--version <branch|tag|commit>]`

- Build Linux kernel: `./make_linux.py [--clean] [--reconfig]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug] [--controller <name>]` (also triggers `make`)

    - `controller`: Controller name to run, see `kmod/controller.h` for available controllers
    - Example: `./run_qemu.py --controller cd9626e`

- Debug kernel: `./run_gdb.py`
