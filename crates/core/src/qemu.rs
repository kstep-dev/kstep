//! Everything about QEMU in one place: what differs between x86_64 and aarch64, the kernel
//! command line, and the argv (never a shell string) for one kSTEP boot, natively (KVM when
//! /dev/kvm is usable, TCG otherwise) or in the website's Emscripten build; they differ only in
//! where the channels go (`Io`).

use std::path::{Path, PathBuf};
use std::process::Command;

/// The host's arch, which is the guest's: kSTEP never cross-compiles or cross-emulates. Every
/// arch-specific fact lives here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arch {
    X86_64,
    Aarch64,
}

#[cfg(target_arch = "x86_64")]
pub const ARCH: Arch = Arch::X86_64;
#[cfg(target_arch = "aarch64")]
pub const ARCH: Arch = Arch::Aarch64;
#[cfg(target_arch = "wasm32")]
pub const ARCH: Arch = Arch::Aarch64;

impl Arch {
    /// As `uname -m` spells it: the config fragment suffix and the qemu-system binary
    pub const fn name(self) -> &'static str {
        match self {
            Arch::X86_64 => "x86_64",
            Arch::Aarch64 => "aarch64",
        }
    }
    /// The image QEMU boots, relative to the kernel tree
    pub const fn kernel_image(self) -> &'static str {
        match self {
            Arch::X86_64 => "arch/x86/boot/bzImage",
            Arch::Aarch64 => "arch/arm64/boot/Image",
        }
    }
    /// Guest-physical address of the first byte of RAM: offset 0 of a RAM file
    pub const fn ram_base(self) -> u64 {
        match self {
            Arch::X86_64 => 0,
            Arch::Aarch64 => 0x4000_0000,
        }
    }
    /// QEMU's machine type; None for the arch's default (pc)
    const fn machine(self) -> Option<&'static str> {
        match self {
            Arch::X86_64 => None,
            Arch::Aarch64 => Some("virt"),
        }
    }
    /// The CPU model under TCG (KVM uses `host`)
    const fn tcg_cpu(self) -> &'static str {
        match self {
            Arch::X86_64 => "max",
            Arch::Aarch64 => "cortex-a57",
        }
    }
    /// The virtio-serial transport: PCI on pc, MMIO on virt (no PCI host there)
    const fn virtio_serial(self) -> &'static str {
        match self {
            Arch::X86_64 => "virtio-serial-pci",
            Arch::Aarch64 => "virtio-serial-device",
        }
    }
    /// The kernel console on the machine's UART, and what the clock needs under emulation
    const fn console_args(self) -> &'static [&'static str] {
        match self {
            Arch::X86_64 => &["console=ttyS0", "tsc=nowatchdog", "tsc=reliable"],
            Arch::Aarch64 => &["console=ttyAMA0", "earlycon"],
        }
    }

    /// The kernel command line: CPU 0 runs the driver, the rest are isolated for the test. After
    /// `--`, the kmod's one parameter, the driver to run.
    /// https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    pub fn cmdline(self, num_cpus: u32, driver: &str) -> String {
        let isol = if num_cpus > 2 {
            format!("1-{}", num_cpus - 1)
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
        args.extend(self.console_args().iter().map(|s| s.to_string()));
        args.push("--".into());
        args.push(format!("driver={driver}"));
        args.join(" ")
    }
}

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
    /// One host thread per vCPU (MTTCG), and a 64 MB translation cache: enough for kSTEP's small
    /// kernels, and what fits next to guest RAM in the website's 1 GB wasm heap
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

    const fn option(self) -> &'static str {
        match self {
            Accel::Kvm => "kvm",
            Accel::Tcg => "tcg,tb-size=64,thread=multi",
        }
    }
}

/// Where a boot's channels go: the kernel console (chardev `console`), the driver's JSON port
/// (`port`), and QEMU's monitor.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Io {
    /// The console to `log`; with `terminal`, on the terminal too, with the monitor multiplexed
    /// in (ctrl-a c). The port is logged to `jsonl` and served on a unix socket at `<jsonl>.sock`
    /// (the cli driver reads its commands there).
    Native {
        log: PathBuf,
        jsonl: PathBuf,
        terminal: bool,
    },
    /// The website: Emscripten device nodes kstep.mjs registers, the PL011 console (write-only),
    /// the port (a pipe, so commands can go in), and the monitor
    Emscripten,
}

impl Io {
    pub fn socket(jsonl: &Path) -> PathBuf {
        let mut s = jsonl.as_os_str().to_owned();
        s.push(".sock");
        PathBuf::from(s)
    }

    /// The console log, where there is one.
    pub fn log(&self) -> Option<&Path> {
        match self {
            Io::Native { log, .. } => Some(log),
            Io::Emscripten => None,
        }
    }

    /// The driver's JSON stream, where it is a file.
    pub fn jsonl(&self) -> Option<&Path> {
        match self {
            Io::Native { jsonl, .. } => Some(jsonl),
            Io::Emscripten => None,
        }
    }

    fn argv(&self) -> Vec<String> {
        match self {
            Io::Native {
                log,
                jsonl,
                terminal,
            } => {
                let sock = Self::socket(jsonl);
                let (log, sock, jsonl) = (log.display(), sock.display(), jsonl.display());
                let mut argv = vec![
                    "-chardev".to_string(),
                    format!("socket,id=port,path={sock},server=on,wait=off,logfile={jsonl}"),
                ];
                if *terminal {
                    argv.extend([
                        "-chardev".into(),
                        format!("stdio,id=console,mux=on,logfile={log},signal=off"),
                        "-mon".into(),
                        "chardev=console".into(),
                    ]);
                } else {
                    argv.extend(["-chardev".into(), format!("file,id=console,path={log}")]);
                }
                argv
            }
            Io::Emscripten => [
                "-chardev",
                "file,id=console,path=/dev/console",
                "-chardev",
                "pipe,id=port,path=/dev/port",
                "-chardev",
                "pipe,id=monitor,path=/dev/monitor",
                "-monitor",
                "chardev:monitor",
            ]
            .map(String::from)
            .to_vec(),
        }
    }
}

/// One kSTEP boot, native or in the browser.
#[derive(Debug, Clone)]
pub struct Boot {
    pub kernel: PathBuf,
    pub rootfs: PathBuf,
    pub driver: String,
    pub machine: Machine,
    pub accel: Accel,
    pub io: Io,
    /// Start stopped with the gdb stub on :1234
    pub debug: bool,
    /// Back guest RAM with this shared file so the host can read kmod/shm.h out of it
    pub ram_file: Option<PathBuf>,
}

impl Boot {
    /// Machine, accelerator, CPU, memory, kernel, command line, kSTEP's channel (one virtio
    /// console port whose virtqueues the kmod drives itself, kmod/io.c: one kick per record
    /// instead of one port I/O exit per byte on a 16550), then where the channels go. QEMU
    /// creates chardevs before devices, whatever the order.
    pub fn argv(&self) -> Vec<String> {
        let mut machine: Vec<&str> = ARCH.machine().into_iter().collect();
        if self.ram_file.is_some() {
            machine.push("memory-backend=ram");
        }
        let mut argv: Vec<String> = vec![];
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
            ARCH.tcg_cpu()
        };
        argv.extend(
            [
                "-accel",
                self.accel.option(),
                "-cpu",
                cpu,
                "-smp",
                &self.machine.num_cpus.to_string(),
                "-m",
                &format!("{}M", self.machine.mem_mb),
                "-kernel",
                &self.kernel.display().to_string(),
                "-initrd",
                &self.rootfs.display().to_string(),
                "-append",
                &ARCH.cmdline(self.machine.num_cpus, &self.driver),
                "-nographic",
                "-nodefaults",
                "-no-reboot", // no automatic reboot after a panic
                "-serial",
                "chardev:console",
                "-device",
                &format!("{},id=vs0", ARCH.virtio_serial()),
                "-device",
                "virtconsole,bus=vs0.0,nr=0,chardev=port",
            ]
            .map(String::from),
        );
        argv.extend(self.io.argv());
        if self.debug {
            argv.extend(["-s".into(), "-S".into()]);
        }
        argv
    }

    pub fn command(&self) -> Command {
        let mut c = Command::new(format!("qemu-system-{}", ARCH.name()));
        c.args(self.argv());
        c
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cmdline_isolates_the_test_cpus() {
        let c = Arch::Aarch64.cmdline(3, "cli");
        assert!(c.contains("isolcpus=nohz,managed_irq,1-2 "), "{c}");
        assert!(c.ends_with("console=ttyAMA0 earlycon -- driver=cli"), "{c}");
        let x = Arch::X86_64.cmdline(2, "default");
        assert!(
            x.contains("nohz_full=1 ")
                && x.ends_with("console=ttyS0 tsc=nowatchdog tsc=reliable -- driver=default"),
            "{x}"
        );
    }

    #[test]
    fn emscripten_boot() {
        let boot = Boot {
            kernel: "/kernel".into(),
            rootfs: "/rootfs.cpio".into(),
            driver: "cli".into(),
            machine: Machine {
                num_cpus: 3,
                mem_mb: 64,
            },
            accel: Accel::Tcg,
            io: Io::Emscripten,
            debug: false,
            ram_file: None,
        };
        let a = boot.argv().join(" ");
        assert!(a.starts_with("-machine virt -accel tcg,tb-size=64,thread=multi -cpu cortex-a57 -smp 3 -m 64M -kernel /kernel -initrd /rootfs.cpio -append rw nokaslr"), "{a}");
        assert!(a.ends_with("-chardev file,id=console,path=/dev/console -chardev pipe,id=port,path=/dev/port -chardev pipe,id=monitor,path=/dev/monitor -monitor chardev:monitor"), "{a}");
    }

    #[test]
    fn native_boot_wires_the_channels() {
        let mut boot = Boot {
            kernel: "/b/kernel".into(),
            rootfs: "/b/rootfs.cpio".into(),
            driver: "cli".into(),
            machine: Machine {
                num_cpus: 3,
                mem_mb: 128,
            },
            accel: Accel::Tcg,
            io: Io::Native {
                log: "/r/qemu.log".into(),
                jsonl: "/r/kstep.jsonl".into(),
                terminal: false,
            },
            debug: false,
            ram_file: None,
        };
        let a = boot.argv().join(" ");
        assert!(
            a.contains("-accel tcg,tb-size=64,thread=multi -cpu "),
            "{a}"
        );
        assert!(a.ends_with("-chardev socket,id=port,path=/r/kstep.jsonl.sock,server=on,wait=off,logfile=/r/kstep.jsonl -chardev file,id=console,path=/r/qemu.log"), "{a}");
        assert!(!a.contains("memory-backend"));
        boot.ram_file = Some("/dev/shm/x".into());
        boot.debug = true;
        boot.io = Io::Native {
            log: "/r/qemu.log".into(),
            jsonl: "/r/kstep.jsonl".into(),
            terminal: true,
        };
        let a = boot.argv().join(" ");
        assert!(
            a.contains("memory-backend=ram")
                && a.contains("mem-path=/dev/shm/x,share=on")
                && a.contains("-mon chardev=console")
                && a.ends_with("-s -S"),
            "{a}"
        );
    }
}
