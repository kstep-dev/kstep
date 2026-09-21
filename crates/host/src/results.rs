//! `results/<label>/`: one directory per run with fixed file names, `results/latest` pointing at
//! the newest ad-hoc one.

use std::fs;
use std::os::unix::fs::symlink;
use std::path::PathBuf;

use anyhow::Result;

pub fn results_dir() -> PathBuf {
    crate::proj_dir().join("results")
}

#[derive(Debug, Clone)]
pub struct ResultDir {
    pub label: String,
}

impl ResultDir {
    pub fn new(label: &str) -> ResultDir {
        ResultDir {
            label: label.to_string(),
        }
    }

    /// Create `results/<label>/`, `tmp_<timestamp>` without a label; with `set_latest`, point
    /// `results/latest` at it.
    pub fn create(label: Option<&str>, set_latest: bool) -> Result<ResultDir> {
        let label = match label {
            Some(l) => l.to_string(),
            None => chrono::Local::now().format("tmp_%Y%m%d_%H%M%S").to_string(),
        };
        let r = ResultDir::new(&label);
        fs::create_dir_all(r.path())?;
        if set_latest {
            let latest = results_dir().join("latest");
            if fs::symlink_metadata(&latest).is_ok() {
                fs::remove_file(&latest)?;
            }
            symlink(&label, &latest)?;
        }
        Ok(r)
    }

    pub fn path(&self) -> PathBuf {
        results_dir().join(&self.label)
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
