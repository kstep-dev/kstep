//! Everything about QEMU in one place: what differs between x86_64 and aarch64, the kernel
//! command line, and the argv (never a shell string) for one kSTEP boot, natively (KVM when
//! /dev/kvm is usable, TCG otherwise) or in the website's Emscripten build; they differ only in
//! where the channels go.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The guest arch this build boots. Natively it is the host's: kSTEP never cross-compiles or
/// cross-emulates. The website's wasm build always boots aarch64.
#[cfg(target_arch = "x86_64")]
pub const ARCH: &ArchSpec = &X86_64;
#[cfg(any(target_arch = "aarch64", target_arch = "wasm32"))]
pub const ARCH: &ArchSpec = &AARCH64;

/// What differs between the arches, side by side. Read as a table: each field is one fact, the
/// two constants below are its two values.
#[derive(Debug, PartialEq, Eq)]
pub struct ArchSpec {
    /// As `uname -m` spells it: the config fragment suffix
    pub name: &'static str,
    /// The QEMU binary: natively on PATH, and the website's Emscripten build of the same name
    pub qemu: &'static str,
    /// The image QEMU boots, relative to the kernel tree
    pub kernel_image: &'static str,
    /// Guest-physical address of the first byte of RAM: offset 0 of a RAM file
    pub ram_base: u64,
    /// QEMU's machine type: named on both arches, so a QEMU changing its default changes nothing here
    machine: &'static str,
    /// The CPU model under TCG (KVM uses `host`)
    tcg_cpu: &'static str,
    /// The virtio-serial transport: PCI on pc, MMIO on virt (no PCI host there)
    virtio_serial: &'static str,
    /// The kernel console on the machine's UART, and what the clock needs under emulation
    console_args: &'static [&'static str],
}

pub const X86_64: ArchSpec = ArchSpec {
    name: "x86_64",
    qemu: "qemu-system-x86_64",
    kernel_image: "arch/x86/boot/bzImage",
    ram_base: 0,
    machine: "pc",
    tcg_cpu: "max",
    virtio_serial: "virtio-serial-pci",
    console_args: &["console=ttyS0", "tsc=nowatchdog", "tsc=reliable"],
};

pub const AARCH64: ArchSpec = ArchSpec {
    name: "aarch64",
    qemu: "qemu-system-aarch64",
    kernel_image: "arch/arm64/boot/Image",
    ram_base: 0x4000_0000,
    machine: "virt",
    tcg_cpu: "cortex-a57",
    virtio_serial: "virtio-serial-device",
    console_args: &["console=ttyAMA0"],
};

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

/// KVM when /dev/kvm can be opened read-write (what KVM_CREATE_VM needs); never in the browser.
pub fn kvm_available() -> bool {
    cfg!(not(target_arch = "wasm32"))
        && std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/kvm")
            .is_ok()
}

/// A two-way channel at `path`: natively a unix socket QEMU serves there (clients connect and
/// go); in the browser the pipe device node kstep.mjs registers there.
#[cfg(not(target_arch = "wasm32"))]
fn served(path: &Path) -> String {
    format!("socket,path={},server=on,wait=off", path.display())
}
#[cfg(target_arch = "wasm32")]
fn served(path: &Path) -> String {
    format!("pipe,path={}", path.display())
}

/// One kSTEP boot, native or in the browser.
#[derive(Debug, Clone)]
pub struct Boot {
    /// The guest's arch: the host's natively, aarch64 in the browser
    pub arch: &'static ArchSpec,
    pub kernel: PathBuf,
    pub rootfs: PathBuf,
    pub driver: String,
    pub machine: Machine,
    /// Where the three channels go: the kernel's console output, and two-way, kSTEP's own
    /// (kmod/io.c: a virtio console port the kmod drives, the driver's records out and the
    /// cli's commands in) and QEMU's monitor. Natively under the results dir, the last two unix
    /// sockets; in the browser, Emscripten device nodes kstep.mjs registers under these names,
    /// the log a write-only file, the "sockets" pipes (Emscripten has no unix sockets).
    pub kernel_log: PathBuf,
    pub kstep_socket: PathBuf,
    pub monitor_socket: PathBuf,
    /// Every record the driver wrote (results/kstep.jsonl, the published transcript). None in
    /// the browser, which keeps the stream itself.
    pub kstep_log: Option<PathBuf>,
    /// Natively: the console on the terminal too, with the monitor multiplexed in (ctrl-a c)
    pub terminal: bool,
    /// Start stopped with the gdb stub on :1234
    pub debug: bool,
    /// Back guest RAM with this shared file so the host can read kmod/shm.h out of it
    pub ram_file: Option<PathBuf>,
    /// Resume from this migration stream (`migrate file:...` on the monitor, taken at the driver's
    /// ready line) instead of booting the kernel: the website skips the ~4 s boot this way
    pub snapshot: Option<PathBuf>,
}

impl Boot {
    /// Machine, accelerator, CPU, memory, kernel, command line, kSTEP's channel (one virtio
    /// console port whose virtqueues the kmod drives itself, kmod/io.c: one kick per record
    /// instead of one port I/O exit per byte on a 16550), then where the channels go. QEMU
    /// creates chardevs before devices, whatever the order.
    pub fn argv(&self) -> Vec<String> {
        // Guest RAM as a file the host can read the shared region out of: the backend object, and
        // the machine told to use it. The browser has neither and reads the wasm heap directly.
        let mut argv: Vec<String> = vec![];
        match &self.ram_file {
            Some(ram) => argv.extend([
                "-machine".into(),
                format!("{},memory-backend=ram", self.arch.machine),
                "-object".into(),
                format!(
                    "memory-backend-file,id=ram,size={}M,mem-path={},share=on",
                    self.machine.mem_mb,
                    ram.display()
                ),
            ]),
            None => argv.extend(["-machine".into(), self.arch.machine.into()]),
        }
        // KVM where the host offers it, else TCG: one host thread per vCPU (MTTCG) and a 64 MB
        // translation cache, enough for kSTEP's small kernels and what fits next to guest RAM in
        // the website's 1 GB wasm heap
        let (accel, cpu) = if kvm_available() {
            ("kvm", "host")
        } else {
            ("tcg,tb-size=64,thread=multi", self.arch.tcg_cpu)
        };
        argv.extend(
            [
                "-accel",
                accel,
                "-cpu",
                cpu,
                "-smp",
                &self.machine.num_cpus.to_string(),
                "-m",
                &format!("{}M", self.machine.mem_mb),
            ]
            .map(String::from),
        );
        // A snapshot restores RAM, so the kernel, initrd and command line it was booted with are
        // not loaded again; the machine and devices must match what it was taken on.
        match &self.snapshot {
            Some(snap) => argv.extend(["-incoming".into(), format!("file:{}", snap.display())]),
            None => argv.extend([
                "-kernel".into(),
                self.kernel.display().to_string(),
                "-initrd".into(),
                self.rootfs.display().to_string(),
                "-append".into(),
                self.cmdline(),
            ]),
        }
        argv.extend(
            [
                "-nographic",
                "-nodefaults",
                "-no-reboot", // no automatic reboot after a panic
                "-serial",
                "chardev:console",
                "-device",
                &format!("{},id=vs0", self.arch.virtio_serial),
                "-device",
                "virtconsole,bus=vs0.0,nr=0,chardev=port",
            ]
            .map(String::from),
        );
        argv.extend(self.channels());
        if self.debug {
            argv.extend(["-s".into(), "-S".into()]);
        }
        argv
    }

    /// The chardevs `console`, `port` and `monitor` (QEMU's names: the serial console, the virtio
    /// console port, the monitor) at the three paths. The kernel log is a file either way; the
    /// two-way channels are served as the build allows.
    fn channels(&self) -> Vec<String> {
        let mut argv = if self.terminal {
            // the console on the terminal too, with the monitor multiplexed in (ctrl-a c)
            vec![
                "-chardev".to_string(),
                format!(
                    "stdio,id=console,mux=on,logfile={},signal=off",
                    self.kernel_log.display()
                ),
                "-mon".into(),
                "chardev=console".into(),
            ]
        } else {
            vec![
                "-chardev".to_string(),
                format!("file,id=console,path={}", self.kernel_log.display()),
            ]
        };
        let log = match &self.kstep_log {
            Some(log) => format!(",logfile={}", log.display()),
            None => String::new(),
        };
        argv.extend([
            "-chardev".into(),
            format!("{}{log},id=port", served(&self.kstep_socket)),
            "-chardev".into(),
            format!("{},id=monitor", served(&self.monitor_socket)),
            "-monitor".into(),
            "chardev:monitor".into(),
        ]);
        argv
    }

    /// The kernel command line: CPU 0 runs the driver, the rest are isolated for the test. After
    /// `--`, the kmod's one parameter, the driver to run.
    /// https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    pub fn cmdline(&self) -> String {
        let num_cpus = self.machine.num_cpus;
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
        args.extend(self.arch.console_args.iter().map(|s| s.to_string()));
        args.push("--".into());
        args.push(format!("driver={}", self.driver));
        args.join(" ")
    }

    pub fn command(&self) -> Command {
        let mut c = Command::new(self.arch.qemu);
        c.args(self.argv());
        c
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The page's boot, as wasm.rs makes it
    fn browser(smp: u32) -> Boot {
        Boot {
            arch: &AARCH64,
            kernel: "/kernel".into(),
            rootfs: "/rootfs.cpio".into(),
            driver: "cli".into(),
            machine: Machine {
                num_cpus: smp,
                mem_mb: 64,
            },
            kernel_log: "/kernel.log".into(),
            kstep_socket: "/kstep".into(),
            kstep_log: None,
            monitor_socket: "/monitor".into(),
            terminal: false,
            debug: false,
            ram_file: None,
            snapshot: None,
        }
    }

    #[test]
    fn cmdline_isolates_the_test_cpus() {
        let mut boot = browser(3);
        let c = boot.cmdline();
        assert!(c.contains("isolcpus=nohz,managed_irq,1-2 "), "{c}");
        assert!(c.ends_with("console=ttyAMA0 -- driver=cli"), "{c}");
        boot.arch = &X86_64;
        boot.machine.num_cpus = 2;
        boot.driver = "default".into();
        let x = boot.cmdline();
        assert!(
            x.contains("nohz_full=1 ")
                && x.ends_with("console=ttyS0 tsc=nowatchdog tsc=reliable -- driver=default"),
            "{x}"
        );
    }

    #[test]
    fn emscripten_boot() {
        let boot = browser(3);
        let a = boot.argv().join(" ");
        // the accelerator is the host's: KVM where /dev/kvm is usable
        assert!(a.starts_with("-machine virt -accel "), "{a}");
        assert!(
            a.contains("-smp 3 -m 64M -kernel /kernel -initrd /rootfs.cpio -append rw nokaslr"),
            "{a}"
        );
        // the browser's two-way channels are the wasm32 build's (served()); run.mjs --check boots them
        assert!(a.contains("-device virtio-serial-device,id=vs0 -device virtconsole,bus=vs0.0,nr=0,chardev=port -chardev file,id=console,path=/kernel.log -chardev "), "{a}");
    }

    #[test]
    fn native_boot_wires_the_channels() {
        let mut boot = Boot {
            arch: &X86_64,
            kernel: "/b/kernel".into(),
            rootfs: "/b/rootfs.cpio".into(),
            driver: "cli".into(),
            machine: Machine {
                num_cpus: 3,
                mem_mb: 128,
            },
            kernel_log: "/r/kernel.log".into(),
            kstep_socket: "/r/kstep.sock".into(),
            kstep_log: Some("/r/kstep.jsonl".into()),
            monitor_socket: "/r/monitor.sock".into(),
            terminal: false,
            debug: false,
            ram_file: None,
            snapshot: None,
        };
        let a = boot.argv().join(" ");
        assert!(
            a.contains("-accel kvm -cpu host")
                || a.contains("-accel tcg,tb-size=64,thread=multi -cpu "),
            "{a}"
        );
        assert!(a.ends_with("-chardev file,id=console,path=/r/kernel.log -chardev socket,path=/r/kstep.sock,server=on,wait=off,logfile=/r/kstep.jsonl,id=port -chardev socket,path=/r/monitor.sock,server=on,wait=off,id=monitor -monitor chardev:monitor"), "{a}");
        assert!(!a.contains("memory-backend"));
        boot.ram_file = Some("/dev/shm/x".into());
        boot.debug = true;
        boot.terminal = true;
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
