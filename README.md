kSTEP
==============

- Install dependencies: `./install_deps.py`

- Reproduce bugs: `./reproduce.py <bug_name|all> [--run <buggy|fixed|plot>]`

- Checkout Linux source: `./checkout_linux.py <branch|tag|commit> [<name>] [--tarball]`

- Build kSTEP and Linux: `make [kstep|linux|all]` (default: `make kstep`)

- Run: `./run.py [--driver <name>] [--params <key=val>...]` (also triggers `make`)

    - `driver`: Driver name to run, see `kmod/driver.c` for available drivers
    - `params`: Additional parameters to pass to the kernel module
    - Example: `./run.py --driver sync_wakeup`

- Debug: `./run.py --debug` in two different terminals starts QEMU and GDB
