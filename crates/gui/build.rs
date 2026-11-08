use std::path::PathBuf;

const SOURCES: &[&str] = &[
    "cpp/app.cpp",
    "cpp/connection.cpp",
    "cpp/mainwindow.cpp",
    "cpp/chatview.cpp",
    "cpp/editorarea.cpp",
    "cpp/filetree.cpp",
    "cpp/logindialog.cpp",
    "cpp/sessionbar.cpp",
    "cpp/terminalpanel.cpp",
];

/// KDE Frameworks only ship CMake configs, so add every KF6 header dir.
fn kf6_include_dirs() -> Vec<PathBuf> {
    let root = std::env::var_os("KF6_INCLUDE_DIR").map(PathBuf::from).unwrap_or_else(|| "/usr/include/KF6".into());
    let mut dirs: Vec<PathBuf> = std::fs::read_dir(&root)
        .unwrap_or_else(|e| panic!("KF6 headers not found in {} ({e}); install ktexteditor", root.display()))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir())
        .collect();
    dirs.sort();
    dirs
}

fn main() {
    let qt = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("Qt6Widgets")
        .expect("Qt6Widgets not found via pkg-config; install qt6-base");

    let mut build = cxx_build::bridge("src/main.rs");
    build.files(SOURCES).std("c++20").flag_if_supported("-Wno-deprecated-declarations").flag_if_supported("-Wno-sfinae-incomplete");
    for p in qt.include_paths.iter().chain(kf6_include_dirs().iter()) {
        build.include(p);
    }
    for (k, v) in &qt.defines {
        build.define(k, v.as_deref());
    }
    build.compile("codeview-ui");

    // Link libraries after the static C++ lib so --as-needed keeps them.
    pkg_config::Config::new().probe("Qt6Widgets").unwrap();
    for lib in ["KF6TextEditor", "KF6Parts", "KF6XmlGui", "KF6CoreAddons"] {
        println!("cargo:rustc-link-lib={lib}");
    }
    println!("cargo:rerun-if-changed=cpp");
    println!("cargo:rerun-if-changed=src/main.rs");
    println!("cargo:rerun-if-env-changed=KF6_INCLUDE_DIR");
}
