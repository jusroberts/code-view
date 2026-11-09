use anyhow::{Context, Result, bail};
use codeview_protocol::{DirEntry, home_dir};
use std::path::PathBuf;

const MAX_FILE_BYTES: u64 = 20 * 1024 * 1024;

pub fn expand(path: &str) -> PathBuf {
    if path == "~" {
        home_dir()
    } else if let Some(rest) = path.strip_prefix("~/") {
        home_dir().join(rest)
    } else {
        PathBuf::from(path)
    }
}

pub fn list_dir(path: &str) -> Result<Vec<DirEntry>> {
    let dir = expand(path);
    let mut out = Vec::new();
    for entry in std::fs::read_dir(&dir).with_context(|| format!("reading {}", dir.display()))? {
        let entry = entry?;
        let p = entry.path();
        // Follow symlinks; fall back to the link itself if it's dangling.
        let meta = std::fs::metadata(&p).or_else(|_| std::fs::symlink_metadata(&p))?;
        out.push(DirEntry {
            name: entry.file_name().to_string_lossy().into_owned(),
            path: p.to_string_lossy().into_owned(),
            is_dir: meta.is_dir(),
            size: meta.len(),
        });
    }
    out.sort_by(|a, b| b.is_dir.cmp(&a.is_dir).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    Ok(out)
}

pub fn read_file(path: &str) -> Result<String> {
    let p = expand(path);
    let len = std::fs::metadata(&p).with_context(|| format!("reading {}", p.display()))?.len();
    if len > MAX_FILE_BYTES {
        bail!("{} is too large to open ({} MB)", p.display(), len / (1024 * 1024));
    }
    let bytes = std::fs::read(&p)?;
    String::from_utf8(bytes).map_err(|_| anyhow::anyhow!("{} is not a UTF-8 text file", p.display()))
}

pub fn write_file(path: &str, content: &str) -> Result<()> {
    let p = expand(path);
    std::fs::write(&p, content).with_context(|| format!("writing {}", p.display()))
}

pub fn make_dir(path: &str) -> Result<()> {
    let p = expand(path);
    std::fs::create_dir(&p).with_context(|| format!("creating {}", p.display()))
}
