//! The QEMU command line for one kSTEP boot, as an argv (never a shell string). One machine per
//! host arch: `pc` with virtio-serial-pci on x86_64, `virt` with virtio-serial-device on
//! aarch64; KVM when /dev/kvm is usable, TCG otherwise.

use std::path::{Path, PathBuf};
use std::process::Command;

pub const ARCH: &str = std::env::consts::ARCH;

/// Guest-physical address of the first byte of RAM: offset 0 of a RAM file.
#[cfg(target_arch = "x86_64")]
pub const RAM_BASE: u64 = 0;
#[cfg(target_arch = "aarch64")]
pub const RAM_BASE: u64 = 0x4000_0000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Machine {
    pub num_cpus: u32,
    pub mem_mb: u32,
}

impl Default for Machine {
    fn default() -> Self {
        Machine {
            num_cpus: 2,
            mem_mb: 512,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Accel {
    Kvm,
    Tcg,
}

impl Accel {
    /// KVM when /dev/kvm exists and is readable, else TCG.
    pub fn detect() -> Accel {
        if std::fs::File::open("/dev/kvm").is_ok() {
            Accel::Kvm
        } else {
            Accel::Tcg
        }
    }
}

/// Where the kernel console goes: the terminal (with QEMU's monitor multiplexed in, ctrl-a c)
/// and a log, or the log alone.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Console {
    Terminal,
    Headless,
}

#[derive(Debug, Clone)]
pub struct Boot {
    pub kernel: PathBuf,
    pub rootfs: PathBuf,
    pub driver: String,
    /// kSTEP module parameters after the `--`, as `key=value`
    pub params: Vec<String>,
    pub machine: Machine,
    /// The console log (qemu.log) and the driver's JSON stream (kstep.jsonl); the stream is also
    /// a unix socket at `<jsonl>.sock` so a client can talk to the driver
    pub log: PathBuf,
    pub jsonl: PathBuf,
    pub console: Console,
    pub accel: Accel,
    /// Start stopped with the gdb stub on :1234
    pub debug: bool,
    /// Back guest RAM with this shared file so the host can read kmod/shm.h out of it
    pub ram_file: Option<PathBuf>,
}

impl Boot {
    pub fn program() -> String {
        format!("qemu-system-{ARCH}")
    }

    pub fn socket(jsonl: &Path) -> PathBuf {
        let mut s = jsonl.as_os_str().to_owned();
        s.push(".sock");
        PathBuf::from(s)
    }

    /// The kernel command line. Everything after `--` goes to init (kmod/user.c).
    /// https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    pub fn cmdline(&self) -> String {
        let n = self.machine.num_cpus;
        let isol = if n > 2 {
            format!("1-{}", n - 1)
        } else {
            "1".to_string()
        };
        let mut args = vec![
            "rw".to_string(),
            "nokaslr".into(),
            "loglevel=7".into(), // up to KERN_DEBUG, so KERN_WARNING always appears
            "sched_verbose".into(),
            format!("isolcpus=nohz,managed_irq,{isol}"),
            "irqaffinity=0".into(),
            format!("rcu_nocbs={isol}"),
            format!("nohz_full={isol}"),
            "init=/user".into(),
            "panic=-1".into(), // exit immediately on panic
        ];
        match ARCH {
            "x86_64" => args.extend([
                "console=ttyS0".into(),
                "tsc=nowatchdog".into(),
                "tsc=reliable".into(),
            ]),
            _ => args.extend(["console=ttyAMA0".into(), "earlycon".into()]),
        }
        args.push("--".into());
        args.push(format!("driver={}", self.driver));
        args.extend(self.params.iter().cloned());
        args.join(" ")
    }

    pub fn argv(&self) -> Vec<String> {
        let (mut machine, tcg_cpu, serial) = match ARCH {
            "x86_64" => (vec![], "max", "virtio-serial-pci"),
            _ => (vec!["virt"], "cortex-a57", "virtio-serial-device"),
        };
        let mut argv: Vec<String> = vec![];
        if self.ram_file.is_some() {
            machine.push("memory-backend=ram");
        }
        if !machine.is_empty() {
            argv.extend(["-machine".into(), machine.join(",")]);
        }
        if let Some(ram) = &self.ram_file {
            let m = self.machine.mem_mb;
            argv.extend([
                "-object".into(),
                format!(
                    "memory-backend-file,id=ram,size={m}M,mem-path={},share=on",
                    ram.display()
                ),
            ]);
        }
        let cpu = if self.accel == Accel::Kvm {
            "host"
        } else {
            tcg_cpu
        };
        let (log, jsonl) = (self.log.display(), self.jsonl.display());
        argv.extend([
            "-cpu".into(),
            cpu.into(),
            "-smp".into(),
            self.machine.num_cpus.to_string(),
            "-m".into(),
            format!("{}M", self.machine.mem_mb),
            "-kernel".into(),
            self.kernel.display().to_string(),
            "-initrd".into(),
            self.rootfs.display().to_string(),
            "-append".into(),
            self.cmdline(),
            "-nographic".into(),
            "-nodefaults".into(),
            "-no-reboot".into(), // no automatic reboot after a panic
            // the kernel console on the machine's UART, to char0 (set up below)
            "-serial".into(),
            "chardev:char0".into(),
            // the driver's JSON stream: logged, and a socket a client can talk on (the cli driver
            // reads its commands here)
            "-chardev".into(),
            format!(
                "socket,id=char1,path={},server=on,wait=off,logfile={jsonl}",
                Self::socket(&self.jsonl).display()
            ),
            // kSTEP's channel: one virtio console port whose virtqueues the kmod drives itself
            // (kmod/io.c): one kick per record instead of one port I/O exit per byte on a 16550
            "-device".into(),
            format!("{serial},id=vs0"),
            "-device".into(),
            "virtconsole,bus=vs0.0,nr=0,chardev=char1".into(),
            "-accel".into(),
            if self.accel == Accel::Kvm {
                "kvm"
            } else {
                "tcg"
            }
            .into(),
        ]);
        match self.console {
            Console::Headless => {
                argv.extend(["-chardev".into(), format!("file,id=char0,path={log}")])
            }
            Console::Terminal => argv.extend([
                "-chardev".into(),
                format!("stdio,id=char0,mux=on,logfile={log},signal=off"),
                "-mon".into(),
                "chardev=char0".into(),
            ]),
        }
        if self.debug {
            argv.extend(["-s".into(), "-S".into()]);
        }
        argv
    }

    pub fn command(&self) -> Command {
        let mut c = Command::new(Self::program());
        c.args(self.argv());
        c
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn boot() -> Boot {
        Boot {
            kernel: "/b/kernel".into(),
            rootfs: "/b/rootfs.cpio".into(),
            driver: "cli".into(),
            params: vec!["foo=1".into()],
            machine: Machine {
                num_cpus: 3,
                mem_mb: 128,
            },
            log: "/r/qemu.log".into(),
            jsonl: "/r/kstep.jsonl".into(),
            console: Console::Headless,
            accel: Accel::Tcg,
            debug: false,
            ram_file: None,
        }
    }

    #[test]
    fn cmdline_isolates_the_test_cpus() {
        let c = boot().cmdline();
        assert!(c.contains("isolcpus=nohz,managed_irq,1-2 "), "{c}");
        assert!(c.ends_with("-- driver=cli foo=1"), "{c}");
        let mut two = boot();
        two.machine.num_cpus = 2;
        assert!(two.cmdline().contains("nohz_full=1 "));
    }

    #[test]
    fn argv_wires_the_channels() {
        let a = boot().argv().join(" ");
        assert!(a.contains("-chardev socket,id=char1,path=/r/kstep.jsonl.sock,server=on,wait=off,logfile=/r/kstep.jsonl"));
        assert!(a.contains("-chardev file,id=char0,path=/r/qemu.log"));
        assert!(a.contains("-accel tcg"));
        assert!(!a.contains("memory-backend"));
        let mut ram = boot();
        ram.ram_file = Some("/dev/shm/x".into());
        ram.debug = true;
        let a = ram.argv().join(" ");
        assert!(
            a.contains("memory-backend=ram")
                && a.contains("mem-path=/dev/shm/x,share=on")
                && a.ends_with("-s -S")
        );
    }
}
