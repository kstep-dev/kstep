use std::path::PathBuf;

use anyhow::{Context, Result};
use kstep_core::qemu::{Accel, Boot, Console, Machine};
use kstep_host::bugs;
use kstep_host::{build, Build, ResultDir};

/// Boot a driver under QEMU (building kSTEP first) and record the run under results/
#[derive(clap::Args)]
#[command(after_help = "Examples:
  kstep run                               # the default driver on build/current
  kstep run foo_buggy sync_wakeup         # a driver on another build
  kstep run --bug freeze                  # freeze's driver and machine on build/freeze_buggy
  kstep run freeze_fixed --bug freeze     # the same on the fixed kernel
  kstep run v6.18 cli -- foo=1            # module parameters after --
  kstep run --debug                       # start stopped with a gdb stub; then `kstep gdb`")]
pub struct Args {
    /// Build under build/ (default: build/current, or <bug>_buggy with --bug)
    build: Option<String>,
    /// Driver to run (kmod/drivers/*.c; default: `default`, or the bug's)
    driver: Option<String>,
    /// Take the driver, machine and build from this bugs.yaml entry
    #[arg(long, value_name = "BUG")]
    bug: Option<String>,
    /// CPUs; 0 runs the driver, the rest are isolated for the test (default 2, or the bug's)
    #[arg(long, value_name = "N")]
    num_cpus: Option<u32>,
    /// Guest memory (default 512, or the bug's)
    #[arg(long, value_name = "MB")]
    mem_mb: Option<u32>,
    /// Subdir under results/ (default: a timestamped tmp_* dir, linked as results/latest)
    #[arg(short, long)]
    label: Option<String>,
    /// Start stopped, with the gdb stub on :1234
    #[arg(long)]
    debug: bool,
    /// Back guest RAM with this shared file (the host reads kmod/shm.h out of it)
    #[arg(long, value_name = "FILE")]
    ram_file: Option<PathBuf>,
    /// kSTEP module parameters, key=value
    #[arg(last = true)]
    params: Vec<String>,
}

fn is_driver(name: &str) -> bool {
    let kmod = kstep_host::proj_dir().join("kmod");
    ["drivers", "drivers_generated"]
        .iter()
        .any(|d| kmod.join(d).join(format!("{name}.c")).is_file())
        || name == "cli"
}

pub fn main(a: Args) -> Result<()> {
    let bug = a.bug.as_deref().map(bugs::get).transpose()?;
    let build_name = a
        .build
        .clone()
        .or_else(|| bug.as_ref().map(|b| b.build_name("buggy")));
    let b = Build::new(build_name.as_deref()).map_err(|e| match &a.build {
        // a driver name in the build slot: say what was meant
        Some(name) if is_driver(name) => {
            e.context(format!("`{name}` is a driver: kstep run <build> {name}"))
        }
        _ => e,
    })?;
    build::build_kstep(&b)?;

    let mut machine = bug.as_ref().map_or_else(Machine::default, |b| b.machine());
    if let Some(n) = a.num_cpus {
        machine.num_cpus = n;
    }
    if let Some(m) = a.mem_mb {
        machine.mem_mb = m;
    }
    let driver = a
        .driver
        .or_else(|| bug.map(|b| b.name))
        .unwrap_or_else(|| "default".into());
    let results = ResultDir::create(a.label.as_deref(), true)?;
    let accel = Accel::detect();
    if accel == Accel::Tcg && std::path::Path::new("/dev/kvm").exists() {
        eprintln!("/dev/kvm is not readable, using TCG (sudo chmod 666 /dev/kvm to use KVM)");
    }
    let boot = Boot {
        kernel: b.kernel(),
        rootfs: b.rootfs(),
        driver,
        params: a.params,
        machine,
        log: results.log(),
        jsonl: results.jsonl(),
        // the console on the terminal when there is one; in a script or CI, qemu.log alone
        console: if unsafe { libc::isatty(0) == 1 && libc::isatty(1) == 1 } {
            Console::Terminal
        } else {
            Console::Headless
        },
        accel,
        debug: a.debug,
        ram_file: a.ram_file,
    };
    if a.debug {
        eprintln!(
            "gdb stub on :1234, guest stopped; attach with `kstep gdb -b {}`",
            b.name
        );
    }
    // ctrl-c belongs to the guest console (QEMU's mux has signal=off); ctrl-a x quits QEMU
    unsafe { libc::signal(libc::SIGINT, libc::SIG_IGN) };
    let status = kstep_host::cmd::status(&mut boot.command()).context("qemu")?;
    println!("Results saved to {}", results.path().display());
    if !status.success() {
        anyhow::bail!("qemu exited with {status}");
    }
    Ok(())
}

/// Attach gdb to a `kstep run --debug` guest
#[derive(clap::Args)]
pub struct GdbArgs {
    /// Build whose vmlinux to load (default: build/current)
    build: Option<String>,
}

pub fn gdb(a: GdbArgs) -> Result<()> {
    use std::os::unix::process::CommandExt;
    let linux = Build::new(a.build.as_deref())?.linux();
    let mut c = std::process::Command::new("gdb");
    c.arg(linux.join("vmlinux"))
        .args([
            "-iex",
            "set pagination off",
            "-iex",
            "set debuginfod enabled off",
        ])
        .arg("-iex")
        .arg(format!("set auto-load safe-path {}", linux.display()))
        .arg("-ex")
        .arg(format!("source {}/vmlinux-gdb.py", linux.display()))
        .args(["-ex", "target remote :1234"]);
    Err(c.exec()).context("exec gdb")
}
