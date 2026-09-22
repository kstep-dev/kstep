//! Shared by the `kstep` CLI, the fuzzer and (through wasm) the website: the shm decoder and the
//! QEMU command line. No clap, no LibAFL.

pub mod qemu;
pub mod shm;
#[cfg(feature = "wasm")]
mod wasm;
