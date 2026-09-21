//! `run`: echo a command like the Python `system()` did and run it, optionally with its output
//! appended to a log file instead of the console.

use std::ffi::OsStr;
use std::io::Write;
use std::path::Path;
use std::process::{Command, Stdio};

use anyhow::{bail, Context, Result};

const BLUE: &str = "\x1b[94m";
pub const GREEN: &str = "\x1b[92m";
pub const RESET: &str = "\x1b[0m";

fn quote(s: &OsStr) -> String {
    let s = s.to_string_lossy();
    if !s.is_empty() && !s.contains(|c: char| c.is_whitespace() || "\"'$`\\|&;<>()*?[]".contains(c))
    {
        return s.into_owned();
    }
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// The command as a shell would show it.
fn display(cmd: &Command) -> String {
    let mut out = match cmd.get_current_dir() {
        Some(dir) => format!("cd {} && ", quote(dir.as_os_str())),
        None => String::new(),
    };
    out.push_str(&quote(cmd.get_program()));
    for arg in cmd.get_args() {
        out.push(' ');
        out.push_str(&quote(arg));
    }
    out
}

pub fn cmd<I, S>(program: &str, args: I) -> Command
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let mut c = Command::new(program);
    c.args(args);
    c
}

pub fn run(cmd: &mut Command, log: Option<&Path>) -> Result<()> {
    let shown = display(cmd);
    eprintln!("$ {BLUE}{shown}{RESET}");
    if let Some(log) = log {
        let mut f = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(log)?;
        writeln!(f, "$ {shown}")?;
        cmd.stdout(Stdio::from(f.try_clone()?))
            .stderr(Stdio::from(f));
    }
    let status = cmd.status().with_context(|| format!("spawn `{shown}`"))?;
    if !status.success() {
        match log {
            Some(log) => bail!(
                "`{}` failed ({status}); see {}",
                shown.split(' ').next().unwrap(),
                log.display()
            ),
            None => bail!("`{}` failed ({status})", shown.split(' ').next().unwrap()),
        }
    }
    Ok(())
}

/// Echo and run, returning stdout; fails on a non-zero exit.
pub fn output(cmd: &mut Command) -> Result<String> {
    let shown = display(cmd);
    eprintln!("$ {BLUE}{shown}{RESET}");
    let out = cmd
        .stderr(Stdio::inherit())
        .output()
        .with_context(|| format!("spawn `{shown}`"))?;
    if !out.status.success() {
        bail!("`{shown}` failed ({})", out.status);
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}
