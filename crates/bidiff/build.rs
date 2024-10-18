use pkg_config;
use cc;

// Example custom build script.
fn main() {
    // Tell Cargo that if the given file changes, to rerun this build script.
    println!("cargo::rerun-if-changed=shim.c");
    // Use the `cc` crate to build a C file and statically link it.
    //

//     fn main() {
//     pkg_config::probe_library("foo").unwrap();
// }

    let glib = pkg_config::probe_library("glib-2.0").unwrap();
    let squashfs_ng = pkg_config::probe_library("libsquashfs1").unwrap();

    cc::Build::new()
        .includes(glib.include_paths)
        .includes(squashfs_ng.include_paths)
        .file("shim.c")
        .compile("shim");
}
