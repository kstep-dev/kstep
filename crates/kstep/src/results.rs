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
    /// The kernel console
    pub fn log(&self) -> PathBuf {
        self.path().join("qemu.log")
    }
    /// The driver's JSON records
    pub fn jsonl(&self) -> PathBuf {
        self.path().join("kstep.jsonl")
    }
}
