
// --- Begin DuckDB patches ---
// DEBUG is an enum variant in ffi::Level but conflicts with the C DEBUG macro.
#pragma push_macro("DEBUG")
#undef DEBUG

// Clang warns about C++ types returned from extern "C" functions (valid but harmless here).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
// --- End prefix ---

