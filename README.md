# Code View

A native KDE/Qt IDE for working with Claude Code sessions.

- `codeview`: the GUI (Qt Widgets plus KTextEditor, Kate's editor component). It's a Rust binary with a C++ UI layer in `crates/gui/cpp`.
- `codeviewd`: a background daemon that owns the Claude sessions and serves files. The GUI starts it automatically. Sessions keep running when the GUI is closed.
- `codeview-protocol`: the JSON-lines protocol shared by the GUI and the daemon.

## Build

You need Rust, Qt 6 (`qt6-base`), KTextEditor 6 (`ktexteditor`), Konsole (for its terminal component), git and the `claude` CLI on your `PATH`.

```sh
cargo build --release
./target/release/codeview
```

The daemon keeps running after you close the GUI. After rebuilding, restart it with `pkill -x codeviewd`; the GUI then starts the new build.

## Using it

- **Sessions** are listed on the far left. Click one to switch the whole window (chat, files, changes, terminals) to it. The toggle button at the top collapses the list to initials badges (**Ctrl+Shift+B**). **Alt+1…9** jumps to a session, **Ctrl+Shift+N** starts a new one, and right-click renames or closes one. When choosing a session's folder, **New Folder…** creates one inside the selected folder.
- **Session → Resume Conversation…** (or right-click a session → *Resume Earlier Conversation Here*) lists the conversations Claude saved for a folder, including ones started with `claude` in a terminal, and opens one as a session with its earlier messages. Panels minimize to buttons on the window edges; click a button to bring its panel back.
- **Permission mode**: the dropdown under the message box changes what Claude may do without asking (ask every time, accept edits, Auto, plan only, bypass), even mid-session. Switching to bypass restarts Claude between turns, because Claude only allows it when launched with it.
- **Changes** (the tab next to Files) lists the files Claude edited in this session, your uncommitted changes, and everything changed on the current branch compared to `origin/HEAD`, `main` or `master`. Click a file to see its diff; right-click to open it or discard its changes.
- **Terminal → Show/Hide Terminal** (**Ctrl+`**) opens terminal tabs for the current session, started in its folder. For remote sessions they run `ssh -t <host>` and `cd` there. **Ctrl+Shift+`** opens another tab.
- **Code intelligence**: opening a file starts a language server on the session's host, if one is installed there: `rust-analyzer`, `clangd`, `pyright-langserver`/`basedpyright-langserver`/`pylsp`, `typescript-language-server`, `dart language-server`, `gopls`, `kotlin-language-server`, `lua-language-server`, `zls` or `bash-language-server`. You get completion, error underlines (hover for the message), hover info and **Go to Definition** (**F12**, **Ctrl+click** or right-click). Completion pops up as you type (e.g. after `.`), or with **Ctrl+Space**; hover info appears when the mouse rests on a name. The status bar shows the current file's state: which server is running, or an **Install…** link when none is found. The link offers an install command that fits the host (pamac/pacman, apt, dnf, npm, rustup, go…) typed into a terminal for you to run, or asking Claude to install one, then **Look Again**. Servers are looked up each time a file opens, on the daemon's `PATH` and a fresh login shell's (for zsh, set `PATH` in `~/.zprofile`, not only `~/.zshrc`), so a newly installed server is used without restarting anything. For Rust with rustup, install the server with `rustup component add rust-analyzer`.

## How it works

- Each session runs `claude -p --input-format stream-json --output-format stream-json --permission-prompt-tool stdio` in the session's folder. Permission prompts appear in the GUI as Allow/Deny buttons. Images you paste or drop are sent to Claude inline.
- Sessions are saved in `~/.local/state/codeview/sessions.json` and resumed with `claude --resume`. When the daemon restarts, each chat is rebuilt from Claude's own transcript of the conversation. The daemon log is `~/.local/state/codeview/daemon.log`.
- The daemon only listens on `$XDG_RUNTIME_DIR/codeview/daemon.sock`, which only your user can access. No network port is opened.
- Git queries and language servers also run in the daemon, so the Changes panel and code intelligence work the same for remote sessions. Language servers stop when the GUI disconnects.
- **Claude → Log In on Current Host…** runs `claude auth login` on that host and shows its output, so you can open the login link and paste the code back.

## Remote hosts

**Hosts → Add Remote Host…** takes an SSH target (`user@host` or an alias from `~/.ssh/config`). Code View then runs `ssh <target> codeviewd bridge`, so SSH handles encryption and authentication with your existing keys. Nothing listens on the network. `codeviewd bridge` starts the remote daemon if it isn't running, and the daemon outlives the SSH connection, so remote sessions keep running when you disconnect.

Setup on each remote host:

1. Install `codeviewd`: when you add a host that doesn't have it, Code View offers to install it (also **Hosts → *host* → Install Daemon…**). This clones `main` from GitHub into `~/.local/share/codeview/src` on the host and runs `cargo install`, so the host needs `git` and Rust ([rustup](https://rustup.rs)). **Hosts → *host* → Update Daemon…** pulls and rebuilds it the same way, then restarts it; the host's menu shows when GitHub has a newer version. To install by hand instead: `cargo install --path crates/daemon` from this repo (installs to `~/.cargo/bin`).
2. Install `claude` there. If it isn't logged in, use **Hosts → *host* → Log In to Claude…**.
3. Make sure you can run `ssh <target>` without a password prompt and that the host key is already accepted. Code View uses `BatchMode=yes`, so it can't answer prompts.

Remote files you open in the editor are cached under `~/.cache/codeview/remote/`, and each save is uploaded back through the daemon. Saved hosts live in `~/.config/codeview/codeview.conf`.

## Tests

```sh
cargo test   # daemon tests use a fake claude and a fake language server (crates/daemon/tests/)
```

## License

MIT. See [LICENSE](LICENSE).
