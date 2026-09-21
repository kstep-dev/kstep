//! kSTEP fuzzer: a LibAFL client of the cli driver.
//!
//! A test case is a byte string. The host decodes it into cli lines (see `decode`), boots the
//! image, feeds the lines to the driver, and reads the replies. A `warn` record from an enabled
//! checker, or an oops in the console, is a finding; a stalled reply is a timeout. Coverage is
//! the guest's edge map (kmod/cov.c), read out of guest RAM after the run. The guest has no
//! fuzzer-specific code.
//!
//! One fuzzing client per core, each with its own QEMU and result directory; LibAFL's Launcher
//! forks them and a broker merges their corpora.

use std::fmt::Write as _;
use std::fs;
use std::num::NonZeroUsize;
use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, Result};
use clap::Parser;
use kstep::bugs::Bug;
use kstep::{bugs, Build, ResultDir, Session};
use kstep_core::qemu::{Accel, Boot, Console};
use kstep_core::shm::COV_SIZE;
use libafl::{
    corpus::{InMemoryOnDiskCorpus, OnDiskCorpus},
    events::{ClientDescription, EventConfig, Launcher},
    executors::{Executor, ExitKind, HasObservers},
    feedbacks::{CrashFeedback, MaxMapFeedback},
    fuzzer::{Fuzzer, StdFuzzer},
    generators::RandBytesGenerator,
    inputs::{BytesInput, HasTargetBytes},
    monitors::MultiMonitor,
    mutators::{havoc_mutations, HavocScheduledMutator},
    observers::StdMapObserver,
    schedulers::QueueScheduler,
    stages::StdMutationalStage,
    state::StdState,
};
use libafl_bolts::{
    core_affinity::Cores,
    current_nanos,
    rands::StdRand,
    shmem::{ShMemProvider, StdShMemProvider},
    tuples::{tuple_list, RefIndexable},
    ToSliceMut,
};

const MAX_RECORDS: usize = 64; // commands per program

/// Fuzz a bugs.yaml entry on build/<bug>_buggy, vetting findings against build/<bug>_fixed
#[derive(Parser)]
#[command(name = "kstep-fuzz", version)]
struct Args {
    /// The bug (bugs.yaml)
    bug: String,
    /// Cores to run clients on, e.g. 0-7 (default: all)
    #[arg(long)]
    cores: Option<String>,
}

// ---- input decoding ------------------------------------------------------------------------

// The general verb table, indexed by the verb byte: what every bug needs, kept small so a bug's
// alphabet stays close to what its rule is about. A bug's own verbs (bugs.yaml) follow it; any cli
// verb may appear there, spelled as cli.c spells it.
//
// create is not here: a bug's setup makes the tasks, so the ids a record can name are fixed for
// the whole program and a mutation stays local. A bug that wants task arrival fuzzed asks for
// `create` among its own verbs.
const VERBS: &[&str] = &["tick", "policy-fair", "affinity", "pause", "wake"];
const FAIR_POLICIES: &[&str] = &["normal", "batch", "idle"];
const RT_POLICIES: &[&str] = &["fifo", "rr"];

/// Bytes to cli lines. Fixed 4-byte records (verb, a, b, c) keep mutations aligned. Arguments are
/// choices resolved against what the host knows for certain: tasks created, tasks it has killed,
/// cgroups created, and the CPU count. Nothing about scheduling state is mirrored; a line the
/// guest rejects costs one error reply.
fn decode(bytes: &[u8], bug: &Bug) -> Vec<String> {
    let ncpus = bug.num_cpus as usize;
    let mut lines: Vec<String> = bug
        .fuzz
        .checks
        .iter()
        .map(|c| format!("check {c}"))
        .collect();
    let mut alive: Vec<usize> = vec![]; // task numbers not yet killed
    let mut ntasks = 0usize;
    let mut cgroups: Vec<String> = vec![]; // paths created, "/g1", "/g1/g2", ..; destroyed ones stay (one error reply)
    let test_cpus = ncpus - 1; // CPUs 1..N-1; 0 is kSTEP's controller

    // The setup lines come first; what they create, the program can refer to.
    for line in &bug.fuzz.setup {
        match line.split_once(' ').unwrap_or((line, "")) {
            ("create", n) => {
                for _ in 0..n.trim().parse().unwrap_or(1) {
                    ntasks += 1;
                    alive.push(ntasks);
                }
            }
            ("cgroup-create", path) => cgroups.push(path.to_string()),
            _ => {}
        }
        lines.push(line.clone());
    }

    let verbs = &bug.fuzz.verbs;
    for rec in bytes.as_chunks::<4>().0.iter().take(MAX_RECORDS) {
        let (index, a, b) = (
            rec[0] as usize % (VERBS.len() + verbs.len()),
            rec[1] as usize,
            rec[2] as usize,
        );
        let verb = if index < VERBS.len() {
            VERBS[index]
        } else {
            verbs[index - VERBS.len()].as_str()
        };
        let task = |a: usize| alive[a % alive.len()];
        let cpu = |b: usize| 1 + b % test_cpus;
        let needs_task = !matches!(
            verb,
            "tick" | "create" | "cpu-freq" | "cgroup-create" | "cgroup-weight" | "cgroup-destroy"
        );
        if needs_task && alive.is_empty() {
            lines.push("tick".into());
            continue;
        }
        let line = match verb {
            "tick" => "tick".to_string(), // one tick: a count would only repeat the event and hide which tick mattered
            // One task at a time: the scheduler event is a task arriving, and a count only repeats
            // it, at the price of guest time and of shifting every later task reference (a task is
            // resolved against how many are alive, so a count makes a one-byte mutation global).
            // A program that wants to start from n tasks says so in the bug's setup.
            "create" => {
                ntasks += 1;
                alive.push(ntasks);
                "create".to_string()
            }
            // one CPU's frequency, as cpufreq would change it: a scale of 1..1024
            "cpu-freq" => format!("cpu-freq {} {}", cpu(a), 1 + b % 1024),
            // a fair policy with a nice, or a real-time one with a priority: what the class reads
            "policy-fair" => format!(
                "policy-fair {} {} {}",
                task(a),
                FAIR_POLICIES[b % FAIR_POLICIES.len()],
                rec[3] as i32 % 40 - 20
            ),
            "policy-rt" => format!(
                "policy-rt {} {} {}",
                task(a),
                RT_POLICIES[b % RT_POLICIES.len()],
                1 + rec[3] as usize % 99
            ),
            "affinity" => {
                // b's low bits pick the allowed test CPUs; at least one
                let mut cpus: Vec<String> = (0..test_cpus)
                    .filter(|i| b >> i & 1 == 1)
                    .map(|i| (i + 1).to_string())
                    .collect();
                if cpus.is_empty() {
                    cpus.push(cpu(b).to_string());
                }
                format!("affinity {} {}", task(a), cpus.join(","))
            }
            "kill" => {
                let t = task(a);
                alive.retain(|&x| x != t);
                format!("kill {t}")
            }
            "cgroup-create" => {
                // under the root or an existing cgroup, so hierarchies form
                let parent = if cgroups.is_empty() || b % 2 == 0 {
                    String::new()
                } else {
                    cgroups[b % cgroups.len()].clone()
                };
                cgroups.push(format!("{parent}/g{}", cgroups.len() + 1));
                format!("cgroup-create {}", cgroups.last().unwrap())
            }
            "cgroup-weight" if !cgroups.is_empty() => {
                let weight = 1 + (b << 8 | rec[3] as usize) % 10000;
                format!("cgroup-weight {} {weight}", cgroups[a % cgroups.len()])
            }
            "cgroup-destroy" if !cgroups.is_empty() => {
                format!("cgroup-destroy {}", cgroups[a % cgroups.len()])
            }
            "cgroup-weight" | "cgroup-destroy" => "tick".to_string(), // nothing to name yet
            "cgroup-attach" => match b % (cgroups.len() + 1) {
                0 => format!("cgroup-attach / {}", task(a)),
                g => format!("cgroup-attach {} {}", cgroups[g - 1], task(a)),
            },
            _ => format!("{verb} {}", task(a)), // pause, wake, block, chan-read, chan-write, freeze, thaw
        };
        lines.push(line);
    }
    lines.extend(["tick", "tick", "tick"].map(String::from));
    lines
}

// ---- the executor --------------------------------------------------------------------------

/// One QEMU boot per test case, driven over the driver's socket.
struct QemuExecutor {
    boot: Boot,
    fixed: Option<Boot>, // the same bug's fixed kernel, which vets every candidate
    bug: Bug,
    findings: PathBuf,
    nfindings: usize,
    observers: (StdMapObserver<'static, u8, false>, ()),
}

enum Outcome {
    Ok,
    Warn(String),
    Timeout(String),
}

/// A program's run: what it came to, the transcript, and the coverage map when asked for. A
/// warn record from a checker or an oops in the console is a `Warn`; a stalled reply a `Timeout`.
fn run_program(boot: &Boot, lines: &[String], map: Option<&mut [u8]>) -> (Outcome, String) {
    let mut transcript = String::new();
    let mut session = match Session::start(boot, Duration::from_secs(30)) {
        Ok(s) => s,
        Err(e) => return (Outcome::Timeout(format!("boot: {e:#}")), transcript),
    };
    let mut warn = None;
    for cmd in lines {
        let Ok(r) = session.cmd(cmd) else {
            return (Outcome::Timeout(format!("no reply to `{cmd}`")), transcript);
        };
        for e in &r.events {
            let _ = writeln!(transcript, "{cmd:24} {e}");
            if warn.is_none() && e.get("type").and_then(|t| t.as_str()) == Some("warn") {
                warn = Some(e.to_string());
            }
        }
        let _ = writeln!(transcript, "{cmd:24} {}", r.reply);
    }
    if let Some(map) = map {
        // A kernel built without linux/config.kstep.cov has no map, and the map stays as it was.
        let _ = session.coverage(map.try_into().expect("map is COV_SIZE"));
    }
    let _ = session.exit();
    if warn.is_none() && console_oops(boot) {
        warn = Some("kernel oops/warning in console".into());
    }
    (warn.map_or(Outcome::Ok, Outcome::Warn), transcript)
}

// Only once the driver runs: a kernel that warns while booting (6.12-rc1 does, in
// pick_task_fair) would otherwise turn every test case into a finding.
fn console_oops(boot: &Boot) -> bool {
    let log = fs::read_to_string(&boot.log).unwrap_or_default();
    let run = log.find("Starting cli").map_or("", |i| &log[i..]);
    [
        "Oops",
        "Kernel panic",
        "BUG:",
        "WARNING:",
        "KASAN:",
        "UBSAN:",
        "general protection fault",
    ]
    .iter()
    .any(|m| run.contains(m))
}

impl QemuExecutor {
    /// The same program on the kernel with the fix. Only a run that gets to the end of the program
    /// with nothing to say confirms the candidate: a rule that warns there is describing ordinary
    /// scheduling, and a run that timed out or never booted has not shown anything either way, so
    /// neither is evidence of the bug. Without a fixed build there is nothing to compare against
    /// and the candidate is kept.
    fn confirmed_on_fixed(&self, lines: &[String]) -> Result<String, String> {
        let Some(fixed) = &self.fixed else {
            return Ok("not vetted: no fixed build".into());
        };
        match run_program(fixed, lines, None).0 {
            Outcome::Ok => Ok("fixed kernel: silent".into()),
            Outcome::Warn(w) => Err(format!("fixed kernel warns too: {w}")),
            Outcome::Timeout(t) => Err(format!("fixed kernel: {t}")),
        }
    }

    fn save_finding(&mut self, lines: &[String], transcript: &str, why: &str) {
        self.nfindings += 1;
        let base = self.findings.join(format!("{:04}", self.nfindings));
        let _ = fs::write(
            base.with_extension("cli"),
            format!("# {why}\n{}\n", lines.join("\n")),
        );
        let _ = fs::write(base.with_extension("jsonl"), transcript);
        let _ = fs::copy(&self.boot.log, base.with_extension("log"));
        println!("finding {}: {why}", base.display());
    }
}

impl<EM, S, Z> Executor<EM, BytesInput, S, Z> for QemuExecutor {
    fn run_target(
        &mut self,
        _fuzzer: &mut Z,
        _state: &mut S,
        _mgr: &mut EM,
        input: &BytesInput,
    ) -> Result<ExitKind, libafl::Error> {
        let lines = decode(&input.target_bytes(), &self.bug);
        let map = self.observers.0.to_slice_mut();
        map.fill(0);
        let (outcome, transcript) = run_program(&self.boot, &lines, Some(map));
        Ok(match outcome {
            Outcome::Ok => ExitKind::Ok,
            // A candidate until the fixed kernel has been shown to be quiet on the same program:
            // one extra boot, and only for a candidate.
            Outcome::Warn(what) => match self.confirmed_on_fixed(&lines) {
                Ok(how) => {
                    self.save_finding(&lines, &transcript, &format!("{what} [{how}]"));
                    ExitKind::Crash
                }
                Err(_) => ExitKind::Ok, // ordinary scheduling, or nothing shown either way
            },
            Outcome::Timeout(why) => {
                self.save_finding(&lines, &transcript, &format!("timeout: {why}"));
                ExitKind::Timeout
            }
        })
    }
}

impl HasObservers for QemuExecutor {
    type Observers = (StdMapObserver<'static, u8, false>, ());
    fn observers(&self) -> RefIndexable<&Self::Observers, Self::Observers> {
        RefIndexable::from(&self.observers)
    }
    fn observers_mut(&mut self) -> RefIndexable<&mut Self::Observers, Self::Observers> {
        RefIndexable::from(&mut self.observers)
    }
}

// ---- main ------------------------------------------------------------------------------------

/// A headless cli boot of `build` on the bug's machine, logging under results/<label>.
fn boot(build: &Build, bug: &Bug, label: &str) -> Result<Boot> {
    build.build_kstep()?;
    let results = ResultDir::create(Some(label), false)?;
    Ok(Boot {
        kernel: build.kernel(),
        rootfs: build.rootfs(),
        driver: "cli".into(),
        machine: bug.machine(),
        log: results.log(),
        jsonl: results.jsonl(),
        console: Console::Headless,
        accel: Accel::detect(),
        debug: false,
        ram_file: None,
    })
}

fn main() -> Result<()> {
    let args = Args::parse();
    let cores = match &args.cores {
        Some(list) => Cores::from_cmdline(list).context("--cores")?,
        None => Cores::all().context("cores")?,
    };
    let bug = bugs::get(&args.bug)?;
    let buggy = Build::new(Some(&bug.build_name("buggy")))?;
    // The bug's other kernel: a candidate that warns there is not this bug. Without it the fuzzer
    // still runs, and every candidate is kept.
    let fixed = Build::new(Some(&bug.build_name("fixed"))).ok();
    if fixed.is_none() {
        println!(
            "no fixed build for {}: findings will not be vetted against it",
            bug.name
        );
    }
    let work = kstep::proj_dir().join("fuzz").join(&buggy.name);
    let findings = work.join("findings");
    fs::create_dir_all(&findings)?;

    // One QEMU configuration per client, prepared here before the clients fork.
    let mut clients = vec![];
    for id in 0..cores.ids.len() {
        let b = boot(&buggy, &bug, &format!("fuzz_{}_{id}", buggy.name))?;
        let f = fixed
            .as_ref()
            .map(|fb| boot(fb, &bug, &format!("fuzz_{}_{id}", fb.name)))
            .transpose()?;
        clients.push((b, f));
    }

    let run_client = |state: Option<_>, mut mgr, client: ClientDescription| {
        let id = client.id() - 1; // the launcher numbers clients from 1
        let (boot, fixed) = clients.into_iter().nth(id).unwrap();

        // The coverage map lives for the whole run; the observer borrows it, the executor fills it.
        let map: &'static mut [u8] = Box::leak(vec![0u8; COV_SIZE].into_boxed_slice());
        let map_ptr = map.as_mut_ptr();
        // SAFETY: the map is leaked and only the executor writes it, between observer reads.
        let observer = unsafe {
            StdMapObserver::new("cov", std::slice::from_raw_parts_mut(map_ptr, COV_SIZE))
        };
        let mut feedback = MaxMapFeedback::new(&observer);
        let mut objective = CrashFeedback::new();
        let mut state = state.unwrap_or_else(|| {
            StdState::new(
                StdRand::with_seed(current_nanos()),
                InMemoryOnDiskCorpus::<BytesInput>::new(work.join(format!("corpus_{id}"))).unwrap(),
                OnDiskCorpus::new(work.join("solutions")).unwrap(),
                &mut feedback,
                &mut objective,
            )
            .unwrap()
        });
        let mut fuzzer = StdFuzzer::new(QueueScheduler::new(), feedback, objective);
        let mut executor = QemuExecutor {
            boot,
            fixed,
            bug: bug.clone(),
            findings: findings.join(format!("{id}")),
            nfindings: 0,
            observers: tuple_list!(observer),
        };
        fs::create_dir_all(&executor.findings).unwrap();

        if state.must_load_initial_inputs() {
            let mut generator =
                RandBytesGenerator::new(NonZeroUsize::new(4 * MAX_RECORDS).unwrap());
            state.generate_initial_inputs_forced(
                &mut fuzzer,
                &mut executor,
                &mut generator,
                &mut mgr,
                4,
            )?;
        }
        let mut stages = tuple_list!(StdMutationalStage::new(HavocScheduledMutator::new(
            havoc_mutations()
        )));
        fuzzer.fuzz_loop(&mut stages, &mut executor, &mut state, &mut mgr)
    };

    // One broker per run: two fuzzers on the same machine must not share a port.
    let broker_port = 1337
        + buggy
            .name
            .bytes()
            .fold(0u16, |h, b| h.wrapping_mul(31).wrapping_add(b as u16))
            % 10000;
    match Launcher::builder()
        .shmem_provider(StdShMemProvider::new().context("shmem")?)
        .configuration(EventConfig::from_name("kstep"))
        .broker_port(broker_port)
        .monitor(MultiMonitor::new(|s| println!("{s}")))
        .run_client(run_client)
        .cores(&cores)
        .build()
        .launch()
    {
        Ok(()) | Err(libafl::Error::ShuttingDown) => Ok(()),
        Err(e) => anyhow::bail!("launcher: {e}"),
    }
}
