use anyhow::{Context, Result};
use clap::ValueEnum;
use kstep::bugs::{self, Bug, Kernel};
use kstep::cmd::{cmd, run, GREEN, RESET};
use kstep::{Build, ResultDir};

/// Check out, build and run a bug on its buggy and fixed kernels, then plot the two traces
#[derive(clap::Args)]
#[command(after_help = "Examples:
  kstep reproduce sync_wakeup            # build/sync_wakeup_{buggy,fixed} -> results/repro_sync_wakeup/
  kstep reproduce all                    # every bug in the paper's table; `extra` for the others
  kstep reproduce freeze --steps plot    # redraw from the traces already there")]
pub struct Args {
    /// A bugs.yaml entry, `all` (the paper's table) or `extra` (the rest)
    bug: String,
    /// Which parts to run
    #[arg(long, value_enum, num_args = 1.., default_values = ["buggy", "fixed", "plot"])]
    steps: Vec<Step>,
}

#[derive(Clone, Copy, PartialEq, Eq, ValueEnum)]
enum Step {
    Buggy,
    Fixed,
    Plot,
}

fn log_step(prefix: &str, message: &str) {
    println!("{GREEN}{prefix}{RESET}: {message}");
}

/// One kernel of a bug: checked out, built (kernel log in build.log), and the driver run on it.
fn reproduce(bug: &Bug, kernel: &Kernel) -> Result<()> {
    let name = bug.build_name(kernel.name);
    log_step(&name, &format!("Checkout Linux {}", kernel.git_ref));
    let b = Build::new(Some(&name))?;
    let log = b.dir().join("build.log");
    log_step(&name, &format!("Build Linux (log: {})", log.display()));
    b.build_linux(None, true, Some(&log))?;
    log_step(&name, "Build kSTEP");
    b.build_kstep()?;

    let results = ResultDir::create(Some(&format!("repro_{}/{}", bug.name, kernel.name)), false)?;
    log_step(
        &name,
        &format!(
            "Run kSTEP (log: {}, output: {})",
            results.kernel_log().display(),
            results.kstep_log().display()
        ),
    );
    let boot = b.boot(&bug.name, bug.machine(), &results, false);
    run(&mut boot.command(), None)
        .with_context(|| format!("see {}", results.kernel_log().display()))
}

pub fn main(a: Args) -> Result<()> {
    let all = bugs::load()?;
    let selected: Vec<&Bug> = match a.bug.as_str() {
        "all" => all.values().filter(|b| !b.extra).collect(),
        "extra" => all.values().filter(|b| b.extra).collect(),
        name => vec![bugs::lookup(&all, name)?],
    };
    println!("running {} bug(s)", selected.len());
    for bug in selected {
        println!("{:=^80}", format!(" {} ", bug.name));
        let [buggy, fixed] = bug.kernels()?;
        for (step, kernel) in [(Step::Buggy, &buggy), (Step::Fixed, &fixed)] {
            if a.steps.contains(&step) {
                reproduce(bug, kernel)?;
            }
        }
        if a.steps.contains(&Step::Plot) {
            match &bug.plot_format {
                Some(f) => {
                    log_step(&bug.name, "Plot");
                    run(
                        cmd(
                            "uv",
                            [
                                "run",
                                "--script",
                                &format!("scripts/plot_{f}.py"),
                                &bug.name,
                            ],
                        )
                        .current_dir(kstep::proj_dir()),
                        None,
                    )?;
                }
                None => println!("no plot format for '{}'", bug.name),
            }
        }
    }
    Ok(())
}
