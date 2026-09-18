//! kSTEP fuzzer: a LibAFL client of the cli driver.
//!
//! A test case is a byte string. The host decodes it into cli lines (see `decode`), boots the
//! image the way run.py does, feeds the lines over the jsonl socket, and reads the replies. A
//! `warn` record from an enabled checker, or an oops in the console, is a finding; a stalled reply
//! is a timeout. Coverage is the guest's edge map (kmod/cov.c), a region of its own whose address
//! the ready line reports: QEMU backs guest RAM with a file, and the host reads the map out of it
//! after the run. The guest has no
//! fuzzer-specific code.
//!
//! One fuzzing client per core, each with its own QEMU, result directory and RAM file; LibAFL's
//! Launcher forks them and a broker merges their corpora.
//!
//!   kstep-fuzzer <bug> [--cores <list>]   a bugs.yaml entry (read through ./reproduce.py --print),
//!                                         on the cores given, or on all of them; e.g. freeze --cores 0-7
use std::{
    fmt::Write as _,
    fs,
    io::{BufRead, BufReader, Write},
    num::NonZeroUsize,
    os::unix::net::UnixStream,
    path::PathBuf,
    process::{Command, Stdio},
    time::{Duration, Instant},
};

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
use libafl_bolts::{core_affinity::Cores, current_nanos, rands::StdRand, shmem::{ShMemProvider, StdShMemProvider}, tuples::{tuple_list, RefIndexable}};

const MAP_SIZE: usize = 1 << 16;
const MAX_RECORDS: usize = 64; // commands per program

// ---- input decoding ------------------------------------------------------------------------

// The general verb table, indexed by the verb byte: what every bug needs, kept small so a bug's
// alphabet stays close to what its rule is about. A bug's own verbs (bugs.yaml) follow it; any cli
// verb may appear there, and `fifo` and `rr` stand for the policy verb with those policies.
//
// create is not here: a bug's setup makes the tasks, so the ids a record can name are fixed for
// the whole program and a mutation stays local. A bug that wants task arrival fuzzed asks for
// `create` among its own verbs.
const VERBS: &[&str] = &["tick", "policy", "affinity", "pause", "wake"];
const POLICIES: &[&str] = &["normal", "batch", "idle"];

/// What a fuzzing run targets: the build, the machine, and the program's fixed parts.
#[derive(Clone, Default)]
struct Target {
    build: String,
    ncpus: usize,
    run_args: Vec<String>, // run.py machine options
    checks: Vec<String>,
    setup: Vec<String>, // cli lines before every program: the initial condition
    verbs: Vec<String>, // the bug's own verbs, after the general table
}

impl Target {
    /// From bugs.yaml, through `./reproduce.py <bug> --print` (one `key value` line per fact).
    fn from_bug(root: &std::path::Path, bug: &str) -> Target {
        let out = Command::new("./reproduce.py").args([bug, "--print"]).current_dir(root).stderr(Stdio::inherit()).output().expect("reproduce.py");
        assert!(out.status.success(), "reproduce.py {bug} --print failed");
        let mut t = Target { ncpus: 2, ..Default::default() };
        for line in String::from_utf8(out.stdout).unwrap().lines() {
            let (key, value) = line.split_once(' ').unwrap_or((line, ""));
            match key {
                "name" => t.build = format!("{value}_buggy"),
                "num_cpus" => t.ncpus = value.parse().expect("num_cpus"),
                "mem_mb" => t.run_args.extend([format!("--{key}"), value.to_string()]),
                "check" => t.checks.push(value.to_string()),
                "setup" => t.setup.push(value.to_string()),
                "verb" => t.verbs.push(value.to_string()),
                _ => {}
            }
        }
        t
    }
}

/// Bytes to cli lines. Fixed 4-byte records (verb, a, b, c) keep mutations aligned. Arguments are
/// choices resolved against what the host knows for certain: tasks created, tasks it has killed,
/// cgroups created, and the CPU count. Nothing about scheduling state is mirrored; a line the
/// guest rejects costs one error reply.
fn decode(bytes: &[u8], t: &Target) -> Vec<String> {
    let ncpus = t.ncpus;
    let mut lines: Vec<String> = t.checks.iter().map(|c| format!("check {c}")).collect();
    let mut alive: Vec<usize> = vec![]; // task numbers not yet killed
    let mut ntasks = 0usize;
    let mut cgroups: Vec<String> = vec![]; // paths created, "/g1", "/g1/g2", ..; destroyed ones stay (one error reply)
    let test_cpus = ncpus - 1; // CPUs 1..N-1; 0 is kSTEP's controller

    // The setup lines come first; what they create, the program can refer to.
    for line in &t.setup {
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

    for rec in bytes.chunks_exact(4).take(MAX_RECORDS) {
        let (index, a, b) = (rec[0] as usize % (VERBS.len() + t.verbs.len()), rec[1] as usize, rec[2] as usize);
        let verb = if index < VERBS.len() { VERBS[index] } else { t.verbs[index - VERBS.len()].as_str() };
        let task = |a: usize| alive[a % alive.len()];
        let cpu = |b: usize| 1 + b % test_cpus;
        let needs_task = !matches!(verb, "tick" | "create" | "cpu-freq" | "cgroup-create" | "cgroup-weight" | "cgroup-destroy");
        if needs_task && alive.is_empty() {
            lines.push("tick".into());
            continue;
        }
        let line = match verb {
            "tick" => format!("tick {}", 1 + a % 128),
            "fifo" | "rr" => format!("policy {} {verb}", task(a)),
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
            "cpu-freq" => format!("cpu-freq {}={}", cpu(a), 1 + b % 1024),
            "nice" => format!("nice {} {}", task(a), b as i32 % 40 - 20),
            "policy" => format!("policy {} {}", task(a), POLICIES[b % POLICIES.len()]),
            "affinity" => {
                // b's low bits pick the allowed test CPUs; at least one
                let mut cpus: Vec<String> = (0..test_cpus).filter(|i| b >> i & 1 == 1).map(|i| (i + 1).to_string()).collect();
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
                let parent = if cgroups.is_empty() || b % 2 == 0 { String::new() } else { cgroups[b % cgroups.len()].clone() };
                cgroups.push(format!("{parent}/g{}", cgroups.len() + 1));
                format!("cgroup-create {}", cgroups.last().unwrap())
            }
            "cgroup-weight" if !cgroups.is_empty() => {
                let weight = 1 + (b << 8 | rec[3] as usize) % 10000;
                format!("cgroup-weight {} {weight}", cgroups[a % cgroups.len()])
            }
            "cgroup-destroy" if !cgroups.is_empty() => format!("cgroup-destroy {}", cgroups[a % cgroups.len()]),
            "cgroup-weight" | "cgroup-destroy" => "tick".to_string(), // nothing to name yet
            "cgroup-attach" => match b % (cgroups.len() + 1) {
                0 => format!("cgroup-attach / {}", task(a)),
                g => format!("cgroup-attach {} {}", cgroups[g - 1], task(a)),
            },
            _ => format!("{verb} {}", task(a)), // pause, wake, block, chan-read, chan-write, freeze, thaw
        };
        lines.push(line);
    }
    lines.extend(["tick", "tick", "tick", "exit"].map(String::from));
    lines
}

// ---- the executor --------------------------------------------------------------------------

struct Paths {
    sock: PathBuf,
    log: PathBuf,
    ram: PathBuf,
}

/// One QEMU boot per test case, driven over the jsonl socket.
struct QemuExecutor {
    cmd: String,
    paths: Paths,
    fixed: Option<(String, Paths)>, // the same bug's fixed kernel, which vets every candidate
    target: Target,
    ram_base: u64, // guest-physical address of RAM's first byte (the RAM file's offset 0)
    findings: PathBuf,
    nfindings: usize,
    observers: (StdMapObserver<'static, u8, false>, ()),
    map: &'static mut [u8],
}

enum Outcome {
    Ok,
    Warn(String),
    Timeout(String),
}

impl QemuExecutor {
    /// Feed the program and read the replies. A reply is a line without "type"; records with a
    /// type ("warn" being the one we care about) may precede it.
    fn drive(&self, paths: &Paths, lines: &[String]) -> std::io::Result<(Outcome, String, u64)> {
        // QEMU takes a moment to create the socket, and creates it before it listens on it.
        let deadline = Instant::now() + Duration::from_secs(30);
        let mut sock = loop {
            match UnixStream::connect(&paths.sock) {
                Ok(s) => break s,
                Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
                Err(_) => return Ok((Outcome::Timeout("no socket".into()), String::new(), 0)),
            }
        };
        sock.set_read_timeout(Some(Duration::from_secs(20)))?;
        let mut rd = BufReader::new(sock.try_clone()?);
        let mut transcript = String::new();
        let mut line = String::new();
        // The ready line reports where the coverage map is; a kernel built without
        // linux/config.kstep.cov reports 0, and read_coverage leaves the map alone.
        let cov = loop {
            line.clear();
            if rd.read_line(&mut line)? == 0 {
                return Ok((Outcome::Timeout("eof before ready".into()), transcript, 0));
            }
            if let Some(v) = line.split("\"cov\":").nth(1) {
                break v.trim_end_matches(|c: char| !c.is_ascii_digit()).parse::<u64>().unwrap_or(0);
            }
        };
        sock.write_all(lines.join("\n").as_bytes())?;
        sock.write_all(b"\n")?;
        let mut warn = None;
        for cmd in lines {
            if cmd == "exit" {
                break;
            }
            loop {
                line.clear();
                match rd.read_line(&mut line) {
                    Ok(0) => return Ok((Outcome::Timeout(format!("eof at `{cmd}`")), transcript, cov)),
                    Ok(_) => {}
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        return Ok((Outcome::Timeout(format!("no reply to `{cmd}`")), transcript, cov))
                    }
                    Err(e) => return Err(e),
                }
                let _ = write!(transcript, "{cmd:24} {line}");
                if line.contains("\"type\":\"warn\"") && warn.is_none() {
                    warn = Some(line.trim().to_string());
                }
                if !line.contains("\"type\"") {
                    break;
                }
            }
        }
        Ok((warn.map_or(Outcome::Ok, Outcome::Warn), transcript, cov))
    }

    /// Copy the guest's edge map out of the RAM file, from the address the ready line reported.
    fn read_coverage(&mut self, cov: u64) {
        use std::io::{Read, Seek, SeekFrom};
        if cov < self.ram_base {
            return;
        }
        if let Ok(mut f) = fs::File::open(&self.paths.ram) {
            if f.seek(SeekFrom::Start(cov - self.ram_base)).is_ok() {
                let _ = f.read_exact(self.map);
            }
        }
    }

    // Only once the driver runs: a kernel that warns while booting (6.12-rc1 does, in
    // pick_task_fair) would otherwise turn every test case into a finding.
    fn console_oops(paths: &Paths) -> bool {
        let log = fs::read_to_string(&paths.log).unwrap_or_default();
        let run = log.find("Starting cli").map_or("", |i| &log[i..]);
        ["Oops", "Kernel panic", "BUG:", "WARNING:", "KASAN:", "UBSAN:", "general protection fault"].iter().any(|m| run.contains(m))
    }

    /// Boot one kernel and run the program through it.
    fn boot(&self, cmd: &str, paths: &Paths, lines: &[String]) -> std::io::Result<(Outcome, String, u64)> {
        let _ = fs::remove_file(&paths.sock);
        let _ = fs::remove_file(&paths.log);
        // `exec`: the shell becomes QEMU, so killing the child on a timeout kills QEMU, not just the shell.
        let mut child = Command::new("sh").arg("-c").arg(format!("exec {cmd}")).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn()?;
        let r = match self.drive(paths, lines) {
            Ok(r) => r,
            Err(e) => (Outcome::Timeout(format!("io: {e}")), String::new(), 0),
        };
        // `exit` reboots the guest and -no-reboot ends QEMU; give it a moment, then insist.
        let deadline = Instant::now() + Duration::from_secs(10);
        while child.try_wait()?.is_none() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(20));
        }
        let _ = child.kill();
        let _ = child.wait();
        Ok(r)
    }

    /// The same program on the kernel with the fix. Only a run that gets to the end of the program
    /// with nothing to say confirms the candidate: a rule that warns there is describing ordinary
    /// scheduling, and a run that timed out or never booted has not shown anything either way, so
    /// neither is evidence of the bug. Without a fixed build there is nothing to compare against
    /// and the candidate is kept.
    fn confirmed_on_fixed(&self, lines: &[String]) -> (bool, String) {
        let Some((cmd, paths)) = &self.fixed else { return (true, "not vetted: no fixed build".into()) };
        match self.boot(cmd, paths, lines) {
            Ok((Outcome::Ok, _, _)) if Self::console_oops(paths) => (false, "fixed kernel: oops".into()),
            Ok((Outcome::Ok, _, _)) => (true, "fixed kernel: silent".into()),
            Ok((Outcome::Warn(w), _, _)) => (false, format!("fixed kernel warns too: {w}")),
            Ok((Outcome::Timeout(t), _, _)) => (false, format!("fixed kernel: {t}")),
            Err(e) => (false, format!("fixed kernel: io {e}")),
        }
    }

    fn save_finding(&mut self, lines: &[String], transcript: &str, why: &str) {
        self.nfindings += 1;
        let base = self.findings.join(format!("{:04}", self.nfindings));
        let _ = fs::write(base.with_extension("cli"), lines.join("\n") + "\n");
        let _ = fs::write(base.with_extension("jsonl"), transcript);
        let _ = fs::copy(&self.paths.log, base.with_extension("log"));
        println!("finding {}: {why}", base.display());
    }
}

impl<EM, S, Z> Executor<EM, BytesInput, S, Z> for QemuExecutor {
    fn run_target(&mut self, _fuzzer: &mut Z, _state: &mut S, _mgr: &mut EM, input: &BytesInput) -> Result<ExitKind, libafl::Error> {
        let lines = decode(&input.target_bytes(), &self.target);
        self.map.fill(0);
        let (outcome, transcript, cov) = self.boot(&self.cmd, &self.paths, &lines)?;
        self.read_coverage(cov);
        // A warn or an oops is a candidate until the fixed kernel has been shown to be quiet on
        // the same program; one extra boot, and only for a candidate.
        let candidate = match &outcome {
            Outcome::Warn(w) => Some(w.clone()),
            _ if Self::console_oops(&self.paths) => Some("kernel oops/warning in console".to_string()),
            _ => None,
        };
        if let Some(what) = candidate {
            return Ok(match self.confirmed_on_fixed(&lines) {
                (true, how) => {
                    self.save_finding(&lines, &transcript, &format!("{what} [{how}]"));
                    ExitKind::Crash
                }
                (false, _) => ExitKind::Ok, // ordinary scheduling, or nothing shown either way
            });
        }
        Ok(match outcome {
            Outcome::Timeout(why) => {
                self.save_finding(&lines, &transcript, &format!("timeout: {why}"));
                ExitKind::Timeout
            }
            _ => ExitKind::Ok,
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

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf();

    // --cores anywhere; without it every core runs a client, which is what a fuzzer is for.
    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    let cores = match argv.iter().position(|a| a == "--cores") {
        Some(i) => {
            let list = argv.get(i + 1).expect("--cores <list>").clone();
            argv.drain(i..=i + 1);
            Cores::from_cmdline(&list).expect("cores")
        }
        None => Cores::all().expect("cores"),
    };
    let bug = argv.first().expect("usage: kstep-fuzzer <bug> [--cores <list>]");
    let target = Target::from_bug(&root, bug);
    let (build, ncpus) = (target.build.clone(), target.ncpus);
    let work = root.join("fuzzer").join("out").join(&build);
    let findings = work.join("findings");
    fs::create_dir_all(&findings).unwrap();

    // run.py builds kSTEP for this kernel and prints the headless QEMU command line; one result
    // directory and RAM file per client, prepared here before the clients fork.
    let qemu = |build: &str, label: &str| {
        let ram = PathBuf::from(format!("/dev/shm/kstep-{label}-ram"));
        let out = Command::new("./run.py")
            .args(["cli", "--build", build, "--num_cpus", &ncpus.to_string(), "--label", label, "--print_cmd"])
            .arg("--ram_file")
            .arg(&ram)
            .args(&target.run_args)
            .current_dir(&root)
            .stderr(Stdio::inherit())
            .output()
            .expect("run.py");
        assert!(out.status.success(), "run.py --print_cmd failed");
        let cmd = String::from_utf8(out.stdout).unwrap().lines().last().unwrap().to_string();
        let results = root.join("results").join(label);
        (cmd, Paths { sock: results.join("kstep.jsonl.sock"), log: results.join("qemu.log"), ram })
    };

    // The bug's other kernel: a candidate that warns there is not this bug. Built by make.py like
    // the buggy one; without it the fuzzer still runs, and every candidate is kept.
    let fixed_build = build
        .strip_suffix("_buggy")
        .map(|b| format!("{b}_fixed"))
        .filter(|b| root.join("build").join(b).join("kernel").exists());
    if fixed_build.is_none() {
        println!("no fixed build for {build}: findings will not be vetted against it");
    }

    let mut clients = vec![];
    for id in 0..cores.ids.len() {
        let label = format!("fuzz_{build}_{id}");
        let fixed = fixed_build.as_ref().map(|b| qemu(b, &format!("fuzz_{b}_{id}")));
        clients.push((qemu(&build, &label), fixed));
    }

    let run_client = |state: Option<_>, mut mgr, client: ClientDescription| {
        let id = client.id() - 1; // the launcher numbers clients from 1
        let ((cmd, paths), fixed) = clients.into_iter().nth(id).unwrap();
        let ram_base = if cmd.starts_with("qemu-system-aarch64") { 0x4000_0000 } else { 0 };

        // The coverage map lives for the whole run; the observer borrows it, the executor fills it.
        let map: &'static mut [u8] = Box::leak(vec![0u8; MAP_SIZE].into_boxed_slice());
        let map_ptr = map.as_mut_ptr();
        // SAFETY: the map is leaked and only the executor writes it, between observer reads.
        let observer = unsafe { StdMapObserver::new("cov", std::slice::from_raw_parts_mut(map_ptr, MAP_SIZE)) };
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
            cmd,
            paths,
            fixed,
            target: target.clone(),
            ram_base,
            findings: findings.join(format!("{id}")),
            nfindings: 0,
            observers: tuple_list!(observer),
            map,
        };
        fs::create_dir_all(&executor.findings).unwrap();

        if state.must_load_initial_inputs() {
            let mut generator = RandBytesGenerator::new(NonZeroUsize::new(4 * MAX_RECORDS).unwrap());
            state.generate_initial_inputs_forced(&mut fuzzer, &mut executor, &mut generator, &mut mgr, 4)?;
        }
        let mut stages = tuple_list!(StdMutationalStage::new(HavocScheduledMutator::new(havoc_mutations())));
        fuzzer.fuzz_loop(&mut stages, &mut executor, &mut state, &mut mgr)
    };

    // One broker per run: two fuzzers on the same machine must not share a port.
    let broker_port = 1337 + build.bytes().fold(0u16, |h, b| h.wrapping_mul(31).wrapping_add(b as u16)) % 10000;
    match Launcher::builder()
        .shmem_provider(StdShMemProvider::new().expect("shmem"))
        .configuration(EventConfig::from_name("kstep"))
        .broker_port(broker_port)
        .monitor(MultiMonitor::new(|s| println!("{s}")))
        .run_client(run_client)
        .cores(&cores)
        .build()
        .launch()
    {
        Ok(()) | Err(libafl::Error::ShuttingDown) => {}
        Err(e) => panic!("launcher: {e}"),
    }
}
