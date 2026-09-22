//! `kstep viz`: build the visualization website (website/site), serve it, or publish it.

use std::fs;
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
    #[arg(long, global = true, default_value_t = 8080)]
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
/// The snapshot's machine: the page's default four test CPUs plus CPU 0 for the driver. A layout
/// of up to that many CPUs resumes it, the driver keeping the CPUs beyond the layout idle.
const SNAPSHOT_SMP: u32 = 5;

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

/// One of website/'s scripts under node, from website/.
fn mjs(node: &std::path::Path, script: &str, args: &[&str]) -> std::process::Command {
    let mut c = cmd(node.to_str().unwrap(), ["--wasm-lazy-compilation"]);
    c.arg(website().join(script))
        .args(args)
        .current_dir(website());
    c
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
    run(
        cmd(
            "wasm-bindgen",
            ["--target", "web", "--no-typescript", "--out-dir"],
        )
        .arg(site())
        .arg(&wasm),
        None,
    )
    .context("wasm-bindgen: cargo install wasm-bindgen-cli at the version in Cargo.lock")
}

fn build() -> Result<String> {
    if !site().join(format!("qemu/{}.wasm", AARCH64.qemu)).exists() {
        bail!("no wasm QEMU in website/site/qemu; run website/setup.sh");
    }
    if *ARCH != AARCH64 {
        bail!("the playground image is arm64; build the site on an arm64 host");
    }
    build_decoder()?;

    // data.json: a cache-busting version and the bug catalog. Every URL the page fetches carries the
    // version, so two builds must never share one: the stamp goes to the second
    let rev = output(cmd("git", ["rev-parse", "--short", "HEAD"]).current_dir(website()))?
        .trim()
        .to_string();
    let version = format!("{rev}-{}", chrono::Utc::now().format("%Y%m%d%H%M%S"));
    let all = bugs::load()?;
    let mut sorted: Vec<&Bug> = all.values().collect();
    sorted.sort_by_key(|b| b.extra); // the paper's table first, in file order
    let bugs: Vec<_> = sorted.into_iter().map(catalog).collect();
    fs::write(
        site().join("data.json"),
        serde_json::to_string_pretty(&json!({
            "version": version, "bugs": bugs,
            "kernels": kstep::checkout::LTS, "kernel": DEFAULT_IMAGE,
            "snapshot_cpus": SNAPSHOT_SMP - 1,
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
            && images.join(format!("snap-{SNAPSHOT_SMP}.json")).is_file()
        {
            continue;
        }
        fs::copy(b.kernel(), images.join("kernel"))?;
        fs::copy(b.rootfs(), images.join("rootfs.cpio"))?;
        run(
            &mut mjs(
                &node,
                "run.mjs",
                &["--snapshot", "--smp", &SNAPSHOT_SMP.to_string(), "--image", images.to_str().unwrap()],
            ),
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

/// Python's static file server on site/, bound to localhost. It answers conditional requests with
/// 304s, so a refresh does not fetch the 14 MB QEMU again, and a browser revalidates the document
/// itself on every reload, so an edit to index.html shows at once. No keep-alive (HTTP/1.0): the
/// page's ~40 requests each open a connection, which over a local port forward is not felt.
fn serve(port: u16) -> Result<()> {
    println!("http://localhost:{port}/");
    run(
        cmd("python3", ["-m", "http.server", "-b", "127.0.0.1", "-d"])
            .arg(site())
            .arg(port.to_string()),
        None,
    )
    .context("python3 -m http.server")
}

/// Refuse to publish a page whose playground images do not speak its protocol, then push site/
/// as the orphan gh-pages branch.
fn deploy(version: &str) -> Result<()> {
    let node = node()?;
    run(&mut mjs(&node, "pagetest.mjs", &[]), None)
        .context("deploy aborted: site/viz.mjs failed its checks")?;
    for kernel in kstep::checkout::LTS {
        let image = site().join("images").join(kernel);
        run(
            &mut mjs(
                &node,
                "run.mjs",
                &["--check", "--image", image.to_str().unwrap()],
            ),
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
