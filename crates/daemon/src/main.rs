mod bridge;
mod claude;
mod daemon;
mod files;
mod git;
mod login;
mod server;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    match std::env::args().nth(1).as_deref() {
        None | Some("serve") => server::serve().await,
        Some("bridge") => bridge::bridge().await,
        Some("--version") => {
            println!("codeviewd {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Some(_) => {
            eprintln!("usage: codeviewd [serve | bridge | --version]");
            std::process::exit(2);
        }
    }
}
