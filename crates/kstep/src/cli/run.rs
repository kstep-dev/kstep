use std::io::{BufRead, IsTerminal, Write};
use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, Result};
use kstep::{bugs, Build, ResultDir, Session};
use kstep_core::qemu::{kvm_available, Boot, Machine};
use kstep_core::shm::State;

/// Boot a driver under QEMU (building kSTEP first) and record the run under results/
#[derive(clap::Args)]
#[command(after_help = "Examples:
  kstep run                               # the cli driver on build/current: type commands, see the machine
  kstep run freeze_buggy                  # a bug's build: its driver on its machine (bugs.yaml)
  kstep run freeze_fixed cli              # the cli driver on that machine
  echo 'create 2\\ntick 3' | kstep run     # a scripted cli session
  kstep run -i findings/0001.cli          # the same, from a file
  kstep run --debug                       # start stopped with a gdb stub; then `kstep gdb`

With the cli driver, commands are read from stdin (kmod/cli.c lists them) and each reply is
printed with the machine's state.")]
pub struct Args {
    /// Build under build/ (default: build/current). A bug's build, <bug>_buggy or <bug>_fixed,
    /// brings the bug's driver and machine from bugs.yaml
    build: Option<String>,
    /// Driver to run (kmod/drivers/*.c; default: cli, or the bug's)
    driver: Option<String>,
    /// CPUs; 0 runs the driver, the rest are isolated for the test (default 2, or the bug's)
    #[arg(long, value_name = "N")]
    num_cpus: Option<u32>,
    /// Guest memory (default 512, or the bug's)
    #[arg(long, value_name = "MB")]
    mem_mb: Option<u32>,
    /// Output dir under results/ (default: a timestamped tmp_*, linked as results/latest)
    #[arg(short, long, value_name = "NAME")]
    out: Option<String>,
    /// cli commands from this file instead of stdin
    #[arg(short, long, value_name = "FILE")]
    input: Option<PathBuf>,
    /// Start stopped, with the gdb stub on :1234
    #[arg(long)]
    debug: bool,
}

fn is_driver(name: &str) -> bool {
    let kmod = kstep::proj_dir().join("kmod");
    name == "cli"
        || ["drivers", "drivers_generated"]
            .iter()
            .any(|d| kmod.join(d).join(format!("{name}.c")).is_file())
}

pub fn main(a: Args) -> Result<()> {
    let b = Build::new(a.build.as_deref()).map_err(|e| match &a.build {
        Some(name) if is_driver(name) => {
            e.context(format!("`{name}` is a driver: kstep run <build> {name}"))
        }
        _ => e,
    })?;
    b.build_kstep()?;
    let bug = bugs::for_build(&b.name)?;

    let mut machine = bug.as_ref().map_or_else(Machine::default, |b| b.machine());
    machine.num_cpus = a.num_cpus.unwrap_or(machine.num_cpus);
    machine.mem_mb = a.mem_mb.unwrap_or(machine.mem_mb);
    let driver = a
        .driver
        .or_else(|| bug.map(|b| b.name))
        .unwrap_or_else(|| "cli".into());
    let results = ResultDir::create(a.out.as_deref(), true)?;
    let interactive =
        a.input.is_none() && std::io::stdin().is_terminal() && std::io::stdout().is_terminal();
    let cli = driver == "cli";
    // the cli's terminal is ours; another driver's console goes to the terminal when there is
    // one, and in a script or CI to kernel.log alone
    let mut boot = b.boot(&driver, machine, &results, interactive && !cli);
    boot.debug = a.debug;
    if !kvm_available() && std::path::Path::new("/dev/kvm").exists() {
        eprintln!("/dev/kvm is not accessible, using TCG (sudo chmod 666 /dev/kvm to use KVM)");
    }
    if a.debug {
        eprintln!(
            "gdb stub on :1234, guest stopped; attach with `kstep gdb {}`",
            b.name
        );
    }
    if cli {
        let input: Box<dyn BufRead> = match &a.input {
            Some(f) => Box::new(std::io::BufReader::new(
                std::fs::File::open(f).with_context(|| f.display().to_string())?,
            )),
            None => Box::new(std::io::stdin().lock()),
        };
        repl(&boot, input, interactive)?;
    } else {
        // ctrl-c belongs to the guest console (QEMU's mux has signal=off); ctrl-a x quits QEMU
        unsafe { libc::signal(libc::SIGINT, libc::SIG_IGN) };
        kstep::cmd::run(&mut boot.command(), None)
            .with_context(|| format!("see {}", results.kernel_log().display()))?;
    }
    println!("Results saved to {}", results.path().display());
    Ok(())
}

/// Drive the cli driver from stdin: each line is a command, answered with its reply and the
/// machine's state.
fn repl(boot: &Boot, mut input: Box<dyn BufRead>, interactive: bool) -> Result<()> {
    let timeout = Duration::from_secs(if boot.debug { 3600 } else { 60 });
    let mut s = Session::start(boot, timeout)?;
    eprintln!(
        "ready ({} CPUs, {} MB); enter repeats a command, `exit` or ^D ends the session",
        boot.machine.num_cpus, boot.machine.mem_mb
    );
    let mut out = std::io::stdout().lock();
    let mut last = String::new();
    loop {
        if interactive {
            write!(out, "kstep> ")?;
            out.flush()?;
        }
        let mut line = String::new();
        if input.read_line(&mut line)? == 0 {
            break;
        }
        // at the prompt, an empty line repeats the last command (`tick`, `tick`, ...)
        let line = match line.trim() {
            "" if interactive && !last.is_empty() => {
                writeln!(out, "kstep> {last}")?;
                last.clone()
            }
            l if l.is_empty() || l.starts_with('#') => continue,
            "exit" => break,
            l => l.to_string(),
        };
        last = line.clone();
        let r = s.cmd(&line)?;
        for e in &r.events {
            writeln!(out, "  {e}")?;
        }
        writeln!(out, "{}", r.reply)?;
        if r.error().is_none() {
            write!(out, "{}", summary(&s.state()?))?;
        }
    }
    s.exit()
}

/// The machine after a command: one line per CPU and per task, joined with its class's view.
fn summary(st: &State) -> String {
    let mut s = format!("t={}\n", st.timestamp);
    for c in &st.cpus {
        let util = st
            .cfs
            .iter()
            .find(|f| f.cpu == c.cpu)
            .map_or(0, |f| f.util_avg);
        let curr = if c.idle {
            "idle".to_string()
        } else if c.current == 0 {
            "other".to_string()
        } else {
            format!("task {}", c.current)
        };
        s += &format!(
            "  cpu {}: {curr}, nr_running={} util={} capacity={} freq={}\n",
            c.cpu, c.nr_running, util, c.capacity, c.freq
        );
    }
    for t in &st.tasks {
        s += &format!("  task {}: {} cpu={} {}", t.task, t.state, t.cpu, t.policy);
        if t.rt_priority > 0 {
            s += &format!(" prio={}", t.rt_priority);
        } else {
            s += &format!(" nice={}", t.nice);
        }
        s += &format!(" exec={:.1}ms", t.sum_exec_runtime as f64 / 1e6);
        if let Some(e) = st.entities.iter().find(|e| e.task == t.task) {
            s += &format!(
                " vruntime={} lag={} weight={} share={:.2}",
                e.vruntime, e.lag, e.weight, e.share
            );
            for (flag, on) in [
                ("eligible", e.eligible),
                ("pick", e.pick),
                ("delayed", e.delayed),
            ] {
                if on {
                    s += &format!(" {flag}");
                }
            }
        }
        if let Some(r) = st.rt_entities.iter().find(|r| r.task == t.task) {
            s += &format!(" position={} time_slice={}", r.position, r.time_slice);
            if r.pick {
                s += " pick";
            }
        }
        if t.cgroup != "/" {
            s += &format!(" cgroup={}", t.cgroup);
        }
        s += "\n";
    }
    s
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
