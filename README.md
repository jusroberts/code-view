# Code View

A native KDE/Qt IDE for working with Claude Code sessions.

- `codeview`: the GUI (Qt Widgets plus KTextEditor, Kate's editor component). It's a Rust binary with a C++ UI layer in `crates/gui/cpp`.
- `codeviewd`: a background daemon that owns the Claude sessions and serves files. The GUI starts it automatically. Sessions keep running when the GUI is closed.
- `codeview-protocol`: the JSON-lines protocol shared by the GUI and the daemon.

## Build

You need Rust, Qt 6 (`qt6-base`), KTextEditor 6 (`ktexteditor`) and the `claude` CLI on your `PATH`.

```sh
cargo build --release
./target/release/codeview
```

The daemon keeps running after you close the GUI. After rebuilding, restart it with `pkill -x codeviewd`; the GUI then starts the new build.

## How it works

- Each session runs `claude -p --input-format stream-json --output-format stream-json --permission-prompt-tool stdio` in the session's folder. Permission prompts appear in the GUI as Allow/Deny buttons. Images you paste or drop are sent to Claude inline.
- Sessions are saved in `~/.local/state/codeview/sessions.json` and resumed with `claude --resume`. The daemon log is `~/.local/state/codeview/daemon.log`.
- The daemon only listens on `$XDG_RUNTIME_DIR/codeview/daemon.sock`, which only your user can access. No network port is opened.
- **Claude → Log In on Current Host…** runs `claude auth login` on that host and shows its output, so you can open the login link and paste the code back.

## Remote hosts

**Hosts → Add Remote Host…** takes an SSH target (`user@host` or an alias from `~/.ssh/config`). Code View then runs `ssh <target> codeviewd bridge`, so SSH handles encryption and authentication with your existing keys. Nothing listens on the network. `codeviewd bridge` starts the remote daemon if it isn't running, and the daemon outlives the SSH connection, so remote sessions keep running when you disconnect.

Setup on each remote host:

1. Install `codeviewd` somewhere a login shell finds it, for example: `cargo install --path crates/daemon` from this repo (installs to `~/.cargo/bin`).
2. Install `claude` there. If it isn't logged in, use **Hosts → *host* → Log In to Claude…**.
3. Make sure you can run `ssh <target>` without a password prompt and that the host key is already accepted. Code View uses `BatchMode=yes`, so it can't answer prompts.

Remote files you open in the editor are cached under `~/.cache/codeview/remote/`, and each save is uploaded back through the daemon. Saved hosts live in `~/.config/codeview/codeview.conf`.

## Tests

```sh
cargo test   # daemon tests run against a fake claude (crates/daemon/tests/fake_claude.py)
```

## License

MIT. See [LICENSE](LICENSE).
