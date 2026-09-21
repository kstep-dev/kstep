//! Shared by the `kstep` CLI, the fuzzer and (through wasm) the website: the shm decoder and the
//! QEMU command line. No clap, no LibAFL, no file system beyond what QEMU needs.

#[cfg(feature = "host")]
pub mod qemu;
