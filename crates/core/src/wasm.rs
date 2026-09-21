//! kSTEP's core for the website: kstep.mjs asks for QEMU's arguments, then copies the shared
//! region out of QEMU's memory after each command and hands the bytes here. JS owns that memory,
//! so it also owns the seqlock retry: a `null` result means "snapshot again".

use serde::Serialize;
use wasm_bindgen::prelude::*;

use crate::qemu::{Accel, Boot, Io, Machine};
use crate::shm;

/// The argv for the page's QEMU (an aarch64 build): the cli driver on `smp` CPUs with `mem` MB,
/// the kernel and initramfs at fixed paths in the module's file system.
#[wasm_bindgen]
pub fn qemu_args(smp: u32, mem: u32) -> Vec<String> {
    Boot {
        kernel: "/kernel".into(),
        rootfs: "/rootfs.cpio".into(),
        driver: "cli".into(),
        machine: Machine {
            num_cpus: smp,
            mem_mb: mem,
        },
        accel: Accel::Tcg,
        io: Io::Emscripten,
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

/// The machine's state from a snapshot of the region; `null` when the writer was mid-update.
/// Fails on a region this decoder was not built for. 64-bit fields come out as BigInt: a
/// vruntime can sit just below 2^64 (a negative value in the kernel's unsigned arithmetic), which
/// no JS number holds; kstep.mjs turns them into numbers the lossy way the page always did.
#[wasm_bindgen]
pub fn shm_decode(region: &[u8]) -> Result<JsValue, JsError> {
    let ser = serde_wasm_bindgen::Serializer::new().serialize_large_number_types_as_bigints(true);
    match shm::decode(region) {
        Ok(state) => state
            .serialize(&ser)
            .map_err(|e| JsError::new(&e.to_string())),
        Err(shm::Error::Busy) => Ok(JsValue::NULL),
        Err(e) => Err(JsError::new(&e.to_string())),
    }
}
