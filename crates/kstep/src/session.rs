//! A running `cli` driver: QEMU with guest RAM in a shared file (the session's own, unlinked once
//! open), the driver's JSON stream on a unix socket, and the machine's state (kmod/shm.h) read
//! out of the RAM file after each command. The CLI's REPL and the fuzzer both sit on this.
//! A session at the ready line can be snapshotted (a migration stream, taken over QEMU's
//! monitor) and later sessions of the same boot resumed from it, skipping the kernel boot.

use std::fs::File;
use std::io::{BufRead, BufReader, ErrorKind, Read, Write};
use std::os::unix::fs::FileExt;
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use kstep_core::qemu::{Boot, ARCH};
use kstep_core::shm::{self, State};
use serde_json::Value;

pub struct Session {
    child: Child,
    reader: BufReader<UnixStream>,
    writer: UnixStream,
    monitor: PathBuf,
    ready: Value,
    ram: File,
    shm_at: u64,
    cov_at: Option<u64>,
    snapshot: Vec<u8>,
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
        let ram_dir = match Path::new("/dev/shm").is_dir() {
            true => PathBuf::from("/dev/shm"),
            false => std::env::temp_dir(),
        };
        let ram_path = ram_dir.join(format!("kstep-{}-ram", std::process::id()));
        boot.ram_file = Some(ram_path.clone());
        let _ = std::fs::remove_file(&boot.kstep_socket);
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

        // QEMU opens the socket a moment after it starts.
        let deadline = Instant::now() + timeout;
        let stream = loop {
            if let Ok(s) = UnixStream::connect(&boot.kstep_socket) {
                break s;
            }
            if let Some(status) = child.try_wait()? {
                bail!(
                    "qemu exited before opening {}: {status}",
                    boot.kstep_socket.display()
                );
            }
            if Instant::now() > deadline {
                let _ = child.kill();
                bail!(
                    "no socket at {} after {timeout:?}",
                    boot.kstep_socket.display()
                );
            }
            std::thread::sleep(Duration::from_millis(20));
        };
        stream.set_read_timeout(Some(timeout))?;
        let mut reader = BufReader::new(stream.try_clone()?);

        // A resumed machine's driver printed its ready line before the snapshot: it is in the
        // stream's sidecar (`snapshot()` wrote it), and the driver is waiting for a command. The
        // socket opens before the stream is loaded, so wait for the machine to run.
        let ready = match &boot.snapshot {
            Some(snap) => {
                let sidecar = std::fs::read(sidecar(snap))
                    .with_context(|| format!("read {}", sidecar(snap).display()))?;
                let mut mon = monitor(&boot.monitor_socket)?;
                while !hmp(&mut mon, "info status")?.contains("running") {
                    if Instant::now() > deadline {
                        bail!("{} not running after {timeout:?}", snap.display());
                    }
                    std::thread::sleep(Duration::from_millis(20));
                }
                serde_json::from_slice(&sidecar)?
            }
            None => read_reply(&mut reader)?.reply,
        };
        // The guest has booted, so its RAM file certainly exists by now.
        let ram = File::open(&ram_path).with_context(|| format!("open {}", ram_path.display()))?;
        let _ = std::fs::remove_file(&ram_path);
        let shm = ready
            .get("shm")
            .and_then(Value::as_u64)
            .context("ready line without shm address")?;
        let shm_at = shm
            .checked_sub(ARCH.ram_base())
            .context("shm address below RAM")?;
        // 0 where the kernel has no coverage map (built without linux/config.kstep.cov)
        let cov_at = ready
            .get("cov")
            .and_then(Value::as_u64)
            .filter(|&c| c != 0)
            .and_then(|c| c.checked_sub(ARCH.ram_base()));
        let mut s = Session {
            child,
            reader,
            writer: stream,
            monitor: boot.monitor_socket.clone(),
            ready,
            ram,
            shm_at,
            cov_at,
            snapshot: vec![0; shm::SIZE],
        };
        s.state().context("read the shared region")?; // magic and layout: the image speaks our version
        Ok(s)
    }

    /// The machine as it is now, as a migration stream at `path` with the ready line in a
    /// sidecar next to it, for `Boot::snapshot` to resume from. Taken at the ready line, before
    /// any command, it stands for the boot itself. QEMU's monitor blocks on `migrate` until the
    /// stream is complete; the guest is idle between commands, so that is one pass. The machine
    /// is left stopped: this session is over.
    pub fn snapshot(mut self, path: &Path) -> Result<()> {
        let mut mon = monitor(&self.monitor)?;
        hmp(&mut mon, &format!("migrate file:{}", path.display()))?;
        let status = hmp(&mut mon, "info migrate")?;
        if !status.contains("Migration status: completed") {
            bail!("snapshot to {} did not complete:\n{status}", path.display());
        }
        std::fs::write(sidecar(path), serde_json::to_vec(&self.ready)?)?;
        let _ = self.child.kill();
        Ok(())
    }

    /// Send one cli line and collect its answer.
    pub fn cmd(&mut self, line: &str) -> Result<Reply> {
        writeln!(self.writer, "{line}")?;
        read_reply(&mut self.reader)
    }

    /// The machine's state as of the last reply. The kmod writes the region before it replies,
    /// so a snapshot taken now is consistent; a `Busy` snapshot is retried a few times anyway.
    pub fn state(&mut self) -> Result<State> {
        for _ in 0..100 {
            self.ram
                .read_exact_at(&mut self.snapshot, self.shm_at)
                .context("read shm")?;
            match shm::decode(&self.snapshot) {
                Err(shm::Error::Busy) => std::thread::sleep(Duration::from_millis(1)),
                r => return r.map_err(Into::into),
            }
        }
        Err(anyhow!("shm stayed mid-update"))
    }

    /// Copy the guest's coverage map out (`map` is `shm::COV_SIZE` bytes); false when this
    /// kernel has none.
    pub fn coverage(&self, map: &mut [u8]) -> Result<bool> {
        let Some(at) = self.cov_at else {
            return Ok(false);
        };
        self.ram
            .read_exact_at(map, at)
            .context("read coverage map")?;
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

/// The ready line a snapshot was taken at, next to the stream
fn sidecar(snapshot: &Path) -> PathBuf {
    snapshot.with_extension("json")
}

/// QEMU's monitor at `path`, past its banner
fn monitor(path: &Path) -> Result<UnixStream> {
    let mut mon =
        UnixStream::connect(path).with_context(|| format!("connect {}", path.display()))?;
    mon.set_read_timeout(Some(Duration::from_secs(120)))?;
    hmp(&mut mon, "")?;
    Ok(mon)
}

/// One HMP command and everything up to the next prompt (the echo, with terminal escapes, and
/// the answer). `migrate` blocks until the stream is complete.
fn hmp(mon: &mut UnixStream, cmd: &str) -> Result<String> {
    writeln!(mon, "{cmd}")?;
    let mut out = String::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = mon.read(&mut buf).context("read the monitor")?;
        if n == 0 {
            bail!("the monitor closed");
        }
        out.push_str(&String::from_utf8_lossy(&buf[..n]));
        if out.ends_with("(qemu) ") {
            return Ok(out);
        }
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
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {
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
