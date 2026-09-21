//! A running `cli` driver: QEMU with guest RAM in a shared file (the session's own, unlinked once
//! open), the driver's JSON stream on a unix socket, and the machine's state (kmod/shm.h) read out of the RAM file after each command.
//! The CLI's REPL and the fuzzer both sit on this.

use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::FileExt;
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Stdio};
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use kstep_core::qemu::{Boot, RAM_BASE};
use kstep_core::shm::{self, Layout, State, COV_SIZE, HDR_SIZE};
use serde_json::Value;

pub struct Session {
    child: Child,
    reader: BufReader<UnixStream>,
    writer: UnixStream,
    ram: File,
    layout: Layout,
    shm_at: u64,
    cov_at: Option<u64>,
    snapshot: Vec<u8>,
    /// The driver's ready line
    pub ready: Value,
}

/// One command's answer: the reply, and the trace events that arrived before it.
#[derive(Debug, Default)]
pub struct Reply {
    pub reply: Value,
    pub events: Vec<Value>,
}

impl Reply {
    pub fn error(&self) -> Option<&str> {
        self.reply.get("error").and_then(Value::as_str)
    }
}

impl Session {
    /// Boot `boot` with its RAM in a shared file and wait for the driver's ready line.
    pub fn start(boot: &Boot, timeout: Duration) -> Result<Session> {
        let mut boot = boot.clone();
        // QEMU creates the file (memory-backend-file); once open here it can be unlinked
        let ram_path = std::env::temp_dir().join(format!("kstep-{}-ram", std::process::id()));
        let ram_path = if Path::new("/dev/shm").is_dir() {
            Path::new("/dev/shm").join(ram_path.file_name().unwrap())
        } else {
            ram_path
        };
        boot.ram_file = Some(ram_path.clone());
        let sock_path = Boot::socket(&boot.jsonl);
        let _ = std::fs::remove_file(&sock_path);
        let mut command = boot.command();
        command.stdin(Stdio::null()).stdout(Stdio::null());
        // QEMU dies with us, however we die: a killed fuzzer must not leave guests behind
        unsafe {
            command.pre_exec(|| {
                if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) == 0 {
                    Ok(())
                } else {
                    Err(std::io::Error::last_os_error())
                }
            })
        };
        let mut child = command.spawn().context("spawn qemu")?;

        // QEMU opens the socket a moment after it starts, and the RAM file with it.
        let deadline = Instant::now() + timeout;
        let stream = loop {
            if let Ok(s) = UnixStream::connect(&sock_path) {
                break s;
            }
            if let Some(status) = child.try_wait()? {
                bail!(
                    "qemu exited before opening {}: {status}",
                    sock_path.display()
                );
            }
            if Instant::now() > deadline {
                let _ = child.kill();
                bail!("no socket at {} after {timeout:?}", sock_path.display());
            }
            std::thread::sleep(Duration::from_millis(20));
        };
        stream.set_read_timeout(Some(timeout))?;
        let mut reader = BufReader::new(stream.try_clone()?);
        let ram = File::open(&ram_path).with_context(|| format!("open {}", ram_path.display()))?;
        let _ = std::fs::remove_file(&ram_path);

        let ready = read_reply(&mut reader)?.reply;
        let shm = ready
            .get("shm")
            .and_then(Value::as_u64)
            .context("ready line without shm address")?;
        let shm_at = shm.checked_sub(RAM_BASE).context("shm address below RAM")?;
        let cov_at = ready
            .get("cov")
            .and_then(Value::as_u64)
            .filter(|&c| c >= RAM_BASE)
            .map(|c| c - RAM_BASE);
        let mut hdr = [0u8; HDR_SIZE];
        ram.read_exact_at(&mut hdr, shm_at)
            .context("read shm header")?;
        let layout = Layout::parse(&hdr)?;
        let snapshot = vec![0; layout.size];
        Ok(Session {
            child,
            reader,
            writer: stream,
            ram,
            layout,
            shm_at,
            cov_at,
            snapshot,
            ready,
        })
    }

    /// Send one cli line and collect its answer.
    pub fn cmd(&mut self, line: &str) -> Result<Reply> {
        writeln!(self.writer, "{line}")?;
        read_reply(&mut self.reader)
    }

    /// The machine's state as of the last reply. The kmod writes the region before it replies,
    /// so a snapshot taken now is consistent; the seqlock retry covers the rest.
    pub fn state(&mut self) -> Result<State> {
        for _ in 0..100 {
            self.ram
                .read_exact_at(&mut self.snapshot, self.shm_at)
                .context("read shm")?;
            match shm::decode(&self.snapshot, &self.layout) {
                Err(shm::Error::Busy) => std::thread::sleep(Duration::from_millis(1)),
                r => return r.map_err(Into::into),
            }
        }
        Err(anyhow!("shm stayed mid-update"))
    }

    /// Copy the guest's coverage map out; false when this kernel has none.
    pub fn coverage(&self, map: &mut [u8; COV_SIZE]) -> Result<bool> {
        match self.cov_at {
            Some(at) => self
                .ram
                .read_exact_at(map, at)
                .context("read coverage map")?,
            None => return Ok(false),
        }
        Ok(true)
    }

    /// End the session: `exit` reboots the guest and -no-reboot ends QEMU; give it a moment,
    /// then insist.
    pub fn exit(mut self) -> Result<()> {
        let _ = writeln!(self.writer, "exit");
        let deadline = Instant::now() + Duration::from_secs(10);
        while self.child.try_wait()?.is_none() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(20));
        }
        Ok(())
    }
}

/// Records up to and including the next reply (a record without "type").
fn read_reply(reader: &mut BufReader<UnixStream>) -> Result<Reply> {
    let mut r = Reply::default();
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => bail!("the driver closed its channel"),
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                bail!("no reply from the driver")
            }
            Err(e) => return Err(e.into()),
        }
        let v: Value =
            serde_json::from_str(line.trim()).with_context(|| format!("bad record {line:?}"))?;
        if v.get("type").is_some() {
            r.events.push(v);
        } else {
            r.reply = v;
            return Ok(r);
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}
