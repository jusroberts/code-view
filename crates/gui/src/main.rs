//! `codeview`: the Qt Widgets GUI. The UI lives in C++ (cpp/), the daemon
//! connection lives in Rust (client.rs); they exchange protocol JSON lines.

mod client;

#[cxx::bridge(namespace = "cv")]
mod ffi {
    extern "Rust" {
        /// Starts the background connection to codeviewd (called once the Qt app exists).
        fn client_start();
        /// Sends one `ClientMsg` JSON object to the daemon.
        fn client_send(json: &str);
    }

    unsafe extern "C++" {
        include!("codeview/cpp/app.h");
        fn run_app() -> i32;
        /// Thread-safe: queues one `ServerMsg` JSON line onto the Qt thread.
        fn deliver_message(json: &str);
        /// Thread-safe: reports connection state changes.
        fn deliver_connection_state(connected: bool, detail: &str);
    }
}

fn client_start() {
    client::start();
}

fn client_send(json: &str) {
    client::send(json);
}

fn main() {
    std::process::exit(ffi::run_app());
}
