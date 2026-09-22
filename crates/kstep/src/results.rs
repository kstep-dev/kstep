//! `results/<name>/`: one directory per run with fixed file names, `results/latest` pointing at
//! the newest ad-hoc one.

use std::fs;
use std::os::unix::fs::symlink;
use std::path::PathBuf;

use anyhow::Result;

fn results_dir() -> PathBuf {
    crate::proj_dir().join("results")
}

#[derive(Debug, Clone)]
pub struct ResultDir {
    pub name: String,
}

impl ResultDir {
    /// Create `results/<name>/`, `tmp_<timestamp>` without a name; with `set_latest`, point
    /// `results/latest` at it.
    pub fn create(name: Option<&str>, set_latest: bool) -> Result<ResultDir> {
        let name = match name {
            Some(l) => l.to_string(),
            None => chrono::Local::now().format("tmp_%Y%m%d_%H%M%S").to_string(),
        };
        let r = ResultDir { name };
        fs::create_dir_all(r.path())?;
        if set_latest {
            let latest = results_dir().join("latest");
            if fs::symlink_metadata(&latest).is_ok() {
                fs::remove_file(&latest)?;
            }
            symlink(&r.name, &latest)?;
        }
        Ok(r)
    }

    pub fn path(&self) -> PathBuf {
        results_dir().join(&self.name)
    }
    // The file names are a published format: the results repo and scripts/utils.py read them.

    /// The kernel's console output
    pub fn kernel_log(&self) -> PathBuf {
        self.path().join("kernel.log")
    }
    /// Every record the driver wrote on kSTEP's channel
    pub fn kstep_log(&self) -> PathBuf {
        self.path().join("kstep.jsonl")
    }
    /// kSTEP's channel (kmod/io.c): the driver's records out, the cli driver's commands in
    pub fn kstep_socket(&self) -> PathBuf {
        self.path().join("kstep.sock")
    }
    /// QEMU's monitor (`socat - UNIX:results/<run>/monitor.sock` to attach)
    pub fn monitor_socket(&self) -> PathBuf {
        self.path().join("monitor.sock")
    }
}
