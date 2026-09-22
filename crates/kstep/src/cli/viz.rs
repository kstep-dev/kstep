//! `kstep viz`: build the visualization website (website/site), serve it, or publish it.

use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use clap::Subcommand;
use kstep::bugs::{self, Bug};
use kstep::cmd::{cmd, output, run};
use kstep::Build;
use kstep_core::qemu::{AARCH64, ARCH};
use serde_json::json;

/// Build the website and serve it at http://localhost:PORT/; `build`, `serve` and `deploy` do one thing each
#[derive(clap::Args)]
#[command(
    after_help = "The site is website/site: the tracked page plus qemu/ (from website/setup.sh) and what `build`
writes there: data.json, images/<kernel>/ (a playground image per supported LTS kernel, from
kSTEP's build/<kernel>, and its snapshot) and kstep_core.js + kstep_core_bg.wasm (crates/core for
wasm32). Plain `kstep viz` builds first, incrementally (an unchanged image keeps its snapshot), then
serves; `serve` skips the build, for page edits, which show on a reload."
)]
pub struct Args {
    #[command(subcommand)]
    cmd: Option<Cmd>,
    #[arg(long, default_value_t = 8080)]
    port: u16,
}

#[derive(Subcommand)]
enum Cmd {
    /// Rebuild the bug catalog, the wasm decoder, the playground images and their snapshots
    Build,
    /// Serve site/ as it is, without building
    Serve,
    /// Build, gate on pagetest.mjs and `run.mjs --check`, force-push site/ as gh-pages
    Deploy,
}

const KSTEP_URL: &str = "https://github.com/kstep-dev/kstep/blob/master";
const RESULTS_URL: &str = "https://raw.githubusercontent.com/kstep-dev/results/main";
/// The playground images: every supported LTS kernel for arm64, shared with the repro builds;
/// the page boots this one unless the URL says otherwise
const DEFAULT_IMAGE: &str = "v6.18";

fn website() -> PathBuf {
    kstep::proj_dir().join("website")
}

fn site() -> PathBuf {
    website().join("site")
}

/// emsdk's node (website/setup.sh installs one) or the system's.
fn node() -> Result<PathBuf> {
    let emsdk = website().join("build/emsdk/node");
    if let Ok(versions) = fs::read_dir(&emsdk) {
        for v in versions.flatten() {
            let node = v.path().join("bin/node");
            if node.is_file() {
                return Ok(node);
            }
        }
    }
    let path = std::env::var_os("PATH").unwrap_or_default();
    std::env::split_paths(&path)
        .map(|p| p.join("node"))
        .find(|p| p.is_file())
        .context("no node: run website/setup.sh or install Node >= 20")
}

/// What the bug catalog shows (the README's results table says the same by hand).
fn catalog(bug: &Bug) -> serde_json::Value {
    let commit = |h: &str| json!({ "label": format!("linux@{}", &h[..7]), "url": format!("https://github.com/torvalds/linux/commit/{h}") });
    let mut fixes: Vec<_> = bug
        .fix
        .iter()
        .chain(&bug.upstream_fix)
        .map(|h| commit(h))
        .collect();
    if let Some(p) = &bug.patch {
        fixes.push(json!({ "label": p, "url": format!("{KSTEP_URL}/linux/{p}") }));
    }
    let driver = ["drivers", "drivers_generated"]
        .iter()
        .map(|d| format!("kmod/{d}/{}.c", bug.name))
        .find(|p| kstep::proj_dir().join(p).is_file())
        .unwrap_or_else(|| format!("kmod/drivers/{}.c", bug.name));
    json!({
        "name": bug.name, "num_cpus": bug.num_cpus, "mem_mb": bug.mem_mb,
        "driver_url": format!("{KSTEP_URL}/{driver}"),
        "fixes": fixes,
        // results are published for every bug with a plot format
        "plot": bug.plot_format.as_ref().map(|_| format!("{RESULTS_URL}/repro_{}/plot.png", bug.name)),
    })
}

/// crates/core for wasm32 through wasm-bindgen, into site/.
fn build_decoder() -> Result<()> {
    let root = kstep::proj_dir();
    run(
        cmd(
            "cargo",
            [
                "build",
                "-p",
                "kstep-core",
                "--profile",
                "wasm",
                "--target",
                "wasm32-unknown-unknown",
                "--features",
                "wasm",
            ],
        )
        .current_dir(&root),
        None,
    )
    .context("the wasm target: rustup target add wasm32-unknown-unknown")?;
    let wasm = root.join("target/wasm32-unknown-unknown/wasm/kstep_core.wasm");
    // the CLI must match the wasm-bindgen crate in Cargo.lock exactly
    let lock = fs::read_to_string(root.join("Cargo.lock"))?;
    let version = lock
        .split("name = \"wasm-bindgen\"\n")
        .nth(1)
        .and_then(|s| s.lines().next())
        .map(|l| l.trim_start_matches("version = ").trim_matches('"'))
        .unwrap_or("?");
    run(
        cmd(
            "wasm-bindgen",
            ["--target", "web", "--no-typescript", "--out-dir"],
        )
        .arg(site())
        .arg(&wasm),
        None,
    )
    .with_context(|| format!("wasm-bindgen: cargo install wasm-bindgen-cli --version {version}"))
}

fn build() -> Result<String> {
    if !site().join(format!("qemu/{}.wasm", AARCH64.qemu)).exists() {
        bail!("no wasm QEMU in website/site/qemu; run website/setup.sh");
    }
    if *ARCH != AARCH64 {
        bail!("the playground image is arm64; build the site on an arm64 host");
    }
    build_decoder()?;

    // data.json: a cache-busting version and the bug catalog
    let rev = output(cmd("git", ["rev-parse", "--short", "HEAD"]).current_dir(website()))?
        .trim()
        .to_string();
    let version = format!("{rev}-{}", chrono::Utc::now().format("%Y%m%d%H%M"));
    let all = bugs::load()?;
    let mut sorted: Vec<&Bug> = all.values().collect();
    sorted.sort_by_key(|b| b.extra); // the paper's table first, in file order
    let bugs: Vec<_> = sorted.into_iter().map(catalog).collect();
    fs::write(
        site().join("data.json"),
        serde_json::to_string_pretty(&json!({
            "version": version, "bugs": bugs,
            "kernels": kstep::checkout::LTS, "kernel": DEFAULT_IMAGE,
        }))?,
    )?;

    // The playground images, one per supported LTS kernel, rebuilt from the current kmod and
    // user.c so the page and the driver it talks to are always published together. First time:
    // a kernel build each (~10 min). The machine at the driver's ready line for the page's
    // default CPU count goes next to each, so the page resumes it instead of booting (~0.3 s
    // instead of ~4 s); the stream fits this QEMU build and image only, so it is taken again
    // exactly when the image changed. Needs site/qemu (website/setup.sh).
    let node = node()?;
    for kernel in kstep::checkout::LTS {
        let b = Build::new(Some(kernel))?;
        b.build_kstep()?;
        let images = site().join("images").join(kernel);
        fs::create_dir_all(&images)?;
        let same = |src: PathBuf, dst: &str| fs::read(&src).ok() == fs::read(images.join(dst)).ok();
        if same(b.kernel(), "kernel")
            && same(b.rootfs(), "rootfs.cpio")
            && images.join("snap-5.json").is_file()
        {
            continue;
        }
        fs::copy(b.kernel(), images.join("kernel"))?;
        fs::copy(b.rootfs(), images.join("rootfs.cpio"))?;
        run(
            cmd(node.to_str().unwrap(), ["--wasm-lazy-compilation"])
                .arg(website().join("run.mjs"))
                .args(["--snapshot", "--image"])
                .arg(&images)
                .current_dir(website()),
            None,
        )
        .with_context(|| {
            format!("snapshot of the {kernel} playground image (website/run.mjs --snapshot)")
        })?;
    }
    let size = output(cmd("du", ["-sh"]).arg(site()))?
        .split_whitespace()
        .next()
        .unwrap_or("?")
        .to_string();
    println!("site/: {size}; {} bugs; version {version}", all.len());
    Ok(version)
}

/// A static file server for site/, for a browser possibly far away (a VS Code port forward):
/// keep-alive, so one connection carries the page's ~40 requests, and an ETag from the file's
/// size and mtime, so a refresh gets 304s instead of the 14 MB QEMU again. Everything the page
/// fetches is versioned (?v=) except index.html, so no-cache: the browser revalidates every
/// time and sees an edit at once.
fn serve(port: u16) -> Result<()> {
    let listener =
        TcpListener::bind(("127.0.0.1", port)).with_context(|| format!("bind port {port}"))?;
    println!("http://localhost:{port}/");
    for stream in listener.incoming().flatten() {
        std::thread::spawn(move || {
            let _ = connection(stream);
        });
    }
    Ok(())
}

fn connection(mut stream: std::net::TcpStream) -> std::io::Result<()> {
    let mut reader = BufReader::new(stream.try_clone()?);
    loop {
        let t0 = std::time::Instant::now();
        let mut req = String::new();
        if reader.read_line(&mut req)? == 0 {
            return Ok(()); // the browser closed the connection
        }
        let mut etag_seen = None;
        let mut close = false;
        let mut line = String::new();
        loop {
            line.clear();
            if reader.read_line(&mut line)? == 0 || line == "\r\n" || line == "\n" {
                break; // the blank line ends the headers
            }
            if let Some((name, value)) = line.split_once(':') {
                match name.to_ascii_lowercase().as_str() {
                    "if-none-match" => etag_seen = Some(value.trim().to_string()),
                    "connection" if value.trim().eq_ignore_ascii_case("close") => close = true,
                    _ => {}
                }
            }
        }
        let path = req
            .split_whitespace()
            .nth(1)
            .unwrap_or("/")
            .split('?')
            .next()
            .unwrap_or("/")
            .to_string();
        let rel = path.trim_start_matches('/');
        let file = site().join(if rel.is_empty() { "index.html" } else { rel });
        let mime = match file.extension().and_then(|e| e.to_str()) {
            Some("html") => "text/html",
            Some("js") | Some("mjs") => "text/javascript",
            Some("css") => "text/css",
            Some("json") => "application/json",
            Some("wasm") => "application/wasm",
            Some("png") => "image/png",
            Some("svg") => "image/svg+xml",
            Some("pdf") => "application/pdf",
            Some("gz") => "application/gzip", // the snapshot: raw bytes, the page gunzips them itself
            _ => "application/octet-stream",
        };
        let meta = fs::metadata(&file)
            .ok()
            .filter(|m| m.is_file() && !rel.contains(".."));
        let etag = meta.as_ref().map(|m| {
            let mtime = m
                .modified()
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map_or(0, |d| d.as_nanos());
            format!("\"{}-{mtime:x}\"", m.len())
        });
        let (status, bytes) = match (&etag, etag_seen) {
            (Some(tag), Some(seen)) if *tag == seen => {
                write!(
                    stream,
                    "HTTP/1.1 304 Not Modified\r\nETag: {tag}\r\nCache-Control: no-cache\r\n\r\n"
                )?;
                (304, 0)
            }
            (Some(tag), _) => {
                let body = fs::read(&file)?;
                write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: {mime}\r\nContent-Length: {}\r\nETag: {tag}\r\nCache-Control: no-cache\r\n\r\n", body.len())?;
                stream.write_all(&body)?;
                (200, body.len())
            }
            (None, _) => {
                write!(
                    stream,
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
                )?;
                (404, 0)
            }
        };
        // one line per request: what the browser asked for and how long it took
        eprintln!("{status} {path} {bytes} B {} ms", t0.elapsed().as_millis());
        if close {
            return Ok(());
        }
    }
}

/// Refuse to publish a page whose playground images do not speak its protocol, then push site/
/// as the orphan gh-pages branch.
fn deploy(version: &str) -> Result<()> {
    let node = node()?;
    run(
        cmd(node.to_str().unwrap(), [website().join("pagetest.mjs")]).current_dir(website()),
        None,
    )
    .context("deploy aborted: site/viz.mjs failed its checks")?;
    for kernel in kstep::checkout::LTS {
        run(
            cmd(node.to_str().unwrap(), ["--wasm-lazy-compilation"])
                .arg(website().join("run.mjs"))
                .args(["--check", "--image"])
                .arg(site().join("images").join(kernel))
                .current_dir(website()),
            None,
        )
        .with_context(|| {
            format!("deploy aborted: the {kernel} playground image failed its check")
        })?;
    }
    let origin = output(cmd("git", ["remote", "get-url", "origin"]).current_dir(website()))?
        .trim()
        .to_string();
    let tmp = std::env::temp_dir().join(format!("kstep-site-{}", std::process::id()));
    run(cmd("cp", ["-r"]).arg(site()).arg(&tmp), None)?;
    let git = |args: &[&str]| run(cmd("git", args).current_dir(&tmp), None);
    let publish = || -> Result<()> {
        git(&["init", "-q", "-b", "gh-pages"])?;
        git(&["add", "-A"])?;
        git(&[
            "-c",
            "user.name=kstep viz",
            "-c",
            "user.email=deploy@kstep",
            "commit",
            "-q",
            "-m",
            &format!("Deploy {version}"),
        ])?;
        git(&["push", "-q", "--force", &origin, "gh-pages"])
    };
    let result = publish();
    let _ = fs::remove_dir_all(&tmp);
    result?;
    println!("pushed gh-pages ({version})");
    Ok(())
}

pub fn main(a: Args) -> Result<()> {
    match a.cmd {
        Some(Cmd::Build) => build().map(|_| ()),
        Some(Cmd::Serve) => serve(a.port),
        Some(Cmd::Deploy) => deploy(&build()?),
        None => {
            build()?; // incremental: what is current stays as it is
            serve(a.port)
        }
    }
}
