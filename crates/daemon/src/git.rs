//! Git queries for the Changes panel. Everything shells out to `git`.

use crate::files::expand;
use anyhow::{Context, Result, bail};
use codeview_protocol::{GitFile, GitStatus};
use std::path::Path;
use std::time::Duration;
use tokio::process::Command;

const MAX_DIFF_BYTES: usize = 2 * 1024 * 1024;

struct Output {
    ok: bool,
    stdout: String,
    stderr: String,
}

async fn git(dir: &Path, args: &[&str]) -> Result<Output> {
    let mut cmd = Command::new("git");
    cmd.arg("-C").arg(dir).args(args).env("GIT_OPTIONAL_LOCKS", "0").kill_on_drop(true);
    let out = tokio::time::timeout(Duration::from_secs(30), cmd.output())
        .await
        .context("git took too long")?
        .context("running git (is it installed?)")?;
    Ok(Output {
        ok: out.status.success(),
        stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&out.stderr).trim().to_string(),
    })
}

async fn git_ok(dir: &Path, args: &[&str]) -> Result<String> {
    let out = git(dir, args).await?;
    if !out.ok {
        bail!("git {}: {}", args.first().unwrap_or(&""), out.stderr);
    }
    Ok(out.stdout)
}

async fn repo_root(cwd: &str) -> Result<std::path::PathBuf> {
    let dir = expand(cwd);
    let out = git(&dir, &["rev-parse", "--show-toplevel"]).await?;
    if !out.ok {
        bail!("not a git repository");
    }
    Ok(out.stdout.trim().into())
}

/// The ref a branch is reviewed against: origin's default branch, else main, else master.
async fn base_ref(root: &Path) -> Option<String> {
    if let Ok(r) = git_ok(root, &["symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"]).await {
        return Some(r.trim().to_string());
    }
    for name in ["main", "master"] {
        if git(root, &["rev-parse", "--verify", "--quiet", name]).await.is_ok_and(|o| o.ok) {
            return Some(name.to_string());
        }
    }
    None
}

fn file(root: &Path, rel: &str, status: &str) -> GitFile {
    GitFile { path: root.join(rel).to_string_lossy().into_owned(), rel: rel.to_string(), status: status.to_string() }
}

/// Parses `git status --porcelain=v1 -z`.
fn parse_status(root: &Path, out: &str) -> Vec<GitFile> {
    let mut files = Vec::new();
    let mut parts = out.split('\0');
    while let Some(entry) = parts.next() {
        if entry.len() < 4 {
            continue;
        }
        let (xy, rel) = entry.split_at(3);
        let (x, y) = (xy.as_bytes()[0] as char, xy.as_bytes()[1] as char);
        if x == 'R' || x == 'C' {
            parts.next(); // the original path
        }
        let status = match (x, y) {
            ('?', _) => '?',
            ('U', _) | (_, 'U') | ('A', 'A') | ('D', 'D') => 'U',
            (' ', y) => y,
            (x, _) => x,
        }
        .to_string();
        files.push(file(root, rel, &status));
    }
    files
}

/// Parses `git diff --name-status -z`.
fn parse_name_status(root: &Path, out: &str) -> Vec<GitFile> {
    let mut files = Vec::new();
    let mut parts = out.split('\0').filter(|s| !s.is_empty());
    while let Some(code) = parts.next() {
        let letter = &code[..1];
        if letter == "R" || letter == "C" {
            parts.next(); // old path
        }
        if let Some(rel) = parts.next() {
            files.push(file(root, rel, letter));
        }
    }
    files
}

pub async fn status(cwd: &str) -> Result<GitStatus> {
    let root = repo_root(cwd).await?;
    let branch = git_ok(&root, &["symbolic-ref", "--quiet", "--short", "HEAD"]).await.ok().map(|s| s.trim().to_string());
    let uncommitted = parse_status(&root, &git_ok(&root, &["status", "--porcelain=v1", "-z", "--untracked-files=all"]).await?);

    let base = base_ref(&root).await;
    let mut merge_base = None;
    let mut branch_files = Vec::new();
    if let Some(b) = &base {
        if let Ok(mb) = git_ok(&root, &["merge-base", "HEAD", b]).await {
            let mb = mb.trim().to_string();
            branch_files = parse_name_status(&root, &git_ok(&root, &["diff", "--name-status", "-z", &mb]).await?);
            merge_base = Some(mb);
        }
    }
    Ok(GitStatus {
        root: root.to_string_lossy().into_owned(),
        branch,
        base,
        merge_base,
        uncommitted,
        branch_files,
    })
}

pub async fn diff(cwd: &str, path: &str, base: Option<&str>) -> Result<String> {
    let root = repo_root(cwd).await?;
    let path = expand(path).to_string_lossy().into_owned();
    let tracked = git(&root, &["ls-files", "--error-unmatch", "--", &path]).await?.ok;
    let has_head = git(&root, &["rev-parse", "--verify", "--quiet", "HEAD"]).await?.ok;
    let out = if !tracked {
        // Exit status 1 just means "differences found".
        git(&root, &["diff", "--no-color", "--no-index", "--", "/dev/null", &path]).await?
    } else {
        let against = base.unwrap_or(if has_head { "HEAD" } else { "--cached" });
        let o = git(&root, &["diff", "--no-color", against, "--", &path]).await?;
        if !o.ok {
            bail!("git diff: {}", o.stderr);
        }
        o
    };
    let mut text = out.stdout;
    if text.len() > MAX_DIFF_BYTES {
        text.truncate(text.floor_char_boundary(MAX_DIFF_BYTES));
        text.push_str("\n… (diff truncated)\n");
    }
    Ok(text)
}

pub async fn revert(cwd: &str, path: &str) -> Result<()> {
    let root = repo_root(cwd).await?;
    let path = expand(path);
    let p = path.to_string_lossy();
    let in_head = !git(&root, &["ls-tree", "--name-only", "HEAD", "--", &p]).await?.stdout.trim().is_empty();
    if in_head {
        git_ok(&root, &["restore", "--source=HEAD", "--staged", "--worktree", "--", &p]).await?;
    } else if git(&root, &["ls-files", "--error-unmatch", "--", &p]).await?.ok {
        git_ok(&root, &["rm", "--force", "--quiet", "--", &p]).await?; // staged but never committed
    } else if path.is_dir() {
        std::fs::remove_dir_all(&path).with_context(|| format!("deleting {p}"))?;
    } else {
        std::fs::remove_file(&path).with_context(|| format!("deleting {p}"))?;
    }
    Ok(())
}
