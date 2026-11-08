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
- The daemon only listens on `$XDG_RUNTIME_DIR/codeview/daemon.sock`, which only your user can access. No network port is opened. Remote hosts will connect over SSH (milestone 2).
- **Claude → Log In…** runs `claude auth login` on the daemon's host and shows its output, so you can open the login link and paste the code back.

## Tests

```sh
cargo test   # daemon tests run against a fake claude (crates/daemon/tests/fake_claude.py)
```
