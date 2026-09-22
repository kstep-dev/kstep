//! kmod/shm.h -> Rust, so the header stays the only description of the shared region. The
//! header wants the kernel's fixed-width typedefs; include/ shims them, so no kernel tree is
//! needed. bindgen also emits layout tests: `cargo test` holds the structs to clang's offsets.

use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let header = root.join("../../kmod/shm.h");
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed=include");
    bindgen::Builder::default()
        .header(header.to_str().unwrap())
        .clang_arg(format!("-I{}", root.join("include").display()))
        .allowlist_item("kstep_.*|KSTEP_.*")
        .prepend_enum_name(false)
        .layout_tests(true) // clang's offsets, checked by `cargo test`
        .derive_default(true)
        .generate()
        .expect("bindgen kmod/shm.h")
        .write_to_file(PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("shm_h.rs"))
        .expect("write bindings");
}
