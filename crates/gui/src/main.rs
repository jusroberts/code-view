//! `codeview`: the Qt Widgets GUI. The UI lives in C++ (cpp/), the daemon
//! connection lives in Rust (client.rs); they exchange protocol JSON lines.

mod client;

#[cxx::bridge(namespace = "cv")]
mod ffi {
    extern "Rust" {
        /// Opens (or reopens) connection `conn` to a daemon: the local one when
        /// `ssh_target` is empty, otherwise via `ssh <ssh_target> codeviewd bridge`.
        fn client_open(conn: &str, ssh_target: &str);
        fn client_close(conn: &str);
        /// Sends one `ClientMsg` JSON object over connection `conn`.
        fn client_send(conn: &str, json: &str);
    }

    unsafe extern "C++" {
        include!("codeview/cpp/app.h");
        fn run_app() -> i32;
        /// Thread-safe: queues one `ServerMsg` JSON line onto the Qt thread.
        fn deliver_message(conn: &str, json: &str);
        /// Thread-safe: reports connection state changes.
        fn deliver_connection_state(conn: &str, connected: bool, detail: &str);
    }
}

fn client_open(conn: &str, ssh_target: &str) {
    client::open(conn, ssh_target);
}

fn client_close(conn: &str) {
    client::close(conn);
}

fn client_send(conn: &str, json: &str) {
    client::send(conn, json);
}

fn main() {
    std::process::exit(ffi::run_app());
}
