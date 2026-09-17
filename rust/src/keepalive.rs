//! Force the linker to retain delta_kernel_ffi's `#[no_mangle]` exports.
//!
//! `delta_kernel_ffi` enters this build as an rlib dependency, and rustc drops objects from a
//! dependency rlib that nothing references -- so its C entry points vanish from our staticlib
//! unless we take their addresses from a symbol that is itself retained. (`-C link-dead-code`
//! does not cover this case; measured, not assumed.)

/// Never call this. It exists so the symbols above it survive into the archive.
#[no_mangle]
pub extern "C" fn duckdb_delta_kernel_ffi_keepalive() -> usize {
    let mut acc = 0usize;
    acc ^= delta_kernel_ffi::get_engine_builder as usize;
    acc ^= delta_kernel_ffi::set_builder_option as usize;
    acc ^= delta_kernel_ffi::builder_build as usize;
    acc
}
