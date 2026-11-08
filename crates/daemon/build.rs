// Records the git commit the daemon is built from, so the GUI can tell when a host is out of date.
use std::path::Path;
use std::process::Command;

fn main() {
    let commit = Command::new("git")
        .args(["rev-parse", "--short=12", "HEAD"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_else(|| "unknown".into());
    println!("cargo:rustc-env=CODEVIEW_COMMIT={commit}");
    let git = Path::new("../../.git");
    for f in ["HEAD", "refs/heads"] {
        if git.join(f).exists() {
            println!("cargo:rerun-if-changed=../../.git/{f}");
        }
    }
}
