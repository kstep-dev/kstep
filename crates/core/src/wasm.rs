//! kSTEP's core for the website: kstep.mjs asks for QEMU's arguments, then copies the shared
//! region out of QEMU's memory after each command and hands the bytes here. JS owns that memory,
//! so it also owns the seqlock retry: a `null` result means "snapshot again".

use wasm_bindgen::prelude::*;

use crate::qemu::{Boot, Machine, ARCH};
use crate::shm;

/// The argv for the page's QEMU (an aarch64 build): the cli driver on `smp` CPUs with `mem` MB,
/// the kernel and initramfs at fixed paths in the module's file system; with `snapshot`, resumed
/// from the migration stream at /snap instead (run.mjs --snapshot writes it, at the ready line).
#[wasm_bindgen]
pub fn qemu_args(smp: u32, mem: u32, snapshot: bool) -> Vec<String> {
    Boot {
        arch: ARCH,
        snapshot: snapshot.then(|| "/snap".into()),
        kernel: "/kernel".into(),
        rootfs: "/rootfs.cpio".into(),
        driver: "cli".into(),
        machine: Machine {
            num_cpus: smp,
            mem_mb: mem,
        },
        // the device nodes kstep.mjs registers (its pipe() and createDevice calls)
        kernel_log: "/kernel.log".into(),
        kstep_socket: "/kstep".into(),
        kstep_log: None,
        monitor_socket: "/monitor".into(),
        terminal: false,
        debug: false,
        ram_file: None,
    }
    .argv()
}

/// How many bytes to snapshot: the whole region.
#[wasm_bindgen]
pub fn shm_size() -> usize {
    shm::SIZE
}

/// The machine's state from a snapshot of the region. Fails on a region this decoder was not
/// built for. Every field fits a JS number: the kernel's
/// wrapping vruntimes are decoded signed, and nothing else gets near 2^53.
#[wasm_bindgen]
pub fn shm_decode(region: &[u8]) -> Result<JsValue, JsError> {
    match shm::decode(region) {
        Ok(state) => serde_wasm_bindgen::to_value(&state).map_err(|e| JsError::new(&e.to_string())),
        Err(e) => Err(JsError::new(&e.to_string())),
    }
}
