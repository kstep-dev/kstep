//! bugs.yaml: one entry per bug, the single place that records what a bug needs. The fields are
//! documented at the top of that file.

use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use indexmap::IndexMap;
use serde::Deserialize;

use kstep_core::qemu::Machine;

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Bug {
    #[serde(skip)]
    pub name: String,
    /// The fix commit: buggy = fix~1, fixed = fix
    pub fix: Option<String>,
    /// Or a base ref and a patch: buggy = ref, fixed = ref + linux/<patch>
    #[serde(rename = "ref")]
    pub git_ref: Option<String>,
    pub patch: Option<String>,
    /// Extra kernel config fragment to merge, relative to the repo root
    pub config: Option<PathBuf>,
    /// Outside the paper's main table
    #[serde(default)]
    pub extra: bool,
    /// scripts/plot_<format>.py draws the reproduction
    pub plot_format: Option<String>,
    #[serde(default = "default_cpus")]
    pub num_cpus: u32,
    #[serde(default = "default_mem")]
    pub mem_mb: u32,
    #[serde(default)]
    pub fuzz: Fuzz,
}

fn default_cpus() -> u32 {
    2
}
fn default_mem() -> u32 {
    128 // kSTEP itself needs ~20 MB
}

/// What the fuzzer needs beyond the machine.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Fuzz {
    /// checkers/ rules to enable
    #[serde(default)]
    pub checks: Vec<String>,
    /// cli verbs beyond the general table
    #[serde(default)]
    pub verbs: Vec<String>,
    /// cli lines run before every fuzzed program
    #[serde(default)]
    pub setup: Vec<String>,
}

/// One of a bug's two kernels.
#[derive(Debug, Clone)]
pub struct Kernel {
    /// "buggy" or "fixed"; the build is `<bug>_<name>`
    pub name: &'static str,
    pub git_ref: String,
    pub patch: Option<PathBuf>,
    pub config: Option<PathBuf>,
}

impl Bug {
    pub fn machine(&self) -> Machine {
        Machine {
            num_cpus: self.num_cpus,
            mem_mb: self.mem_mb,
        }
    }

    /// The buggy and the fixed kernel, from `fix` or from `ref` + `patch`.
    pub fn kernels(&self) -> Result<[Kernel; 2]> {
        let kernel = |name, git_ref: String, patch| Kernel {
            name,
            git_ref,
            patch,
            config: self.config.clone(),
        };
        match (&self.fix, &self.git_ref, &self.patch) {
            (Some(fix), _, _) => Ok([
                kernel("buggy", format!("{fix}~1"), None),
                kernel("fixed", fix.clone(), None),
            ]),
            (None, Some(r), Some(p)) => {
                let patch = crate::proj_dir().join("linux").join(p);
                Ok([
                    kernel("buggy", r.clone(), None),
                    kernel("fixed", r.clone(), Some(patch)),
                ])
            }
            _ => bail!(
                "bug '{}': specify either 'fix' or 'ref' + 'patch'",
                self.name
            ),
        }
    }

    pub fn build_name(&self, kernel: &str) -> String {
        format!("{}_{kernel}", self.name)
    }
}

/// All bugs, in file order.
pub fn load() -> Result<IndexMap<String, Bug>> {
    let path = crate::proj_dir().join("bugs.yaml");
    let text = std::fs::read_to_string(&path).with_context(|| path.display().to_string())?;
    let raw: IndexMap<String, Option<Bug>> =
        serde_yaml_ng::from_str(&text).with_context(|| path.display().to_string())?;
    Ok(raw
        .into_iter()
        .map(|(name, bug)| {
            let mut bug = bug.unwrap_or_else(|| serde_yaml_ng::from_str("{}").unwrap());
            bug.name = name.clone();
            (name, bug)
        })
        .collect())
}

/// The bug a build belongs to: `<bug>_buggy` or `<bug>_fixed` with `<bug>` in bugs.yaml.
pub fn for_build(build: &str) -> Result<Option<Bug>> {
    let Some(name) = build
        .strip_suffix("_buggy")
        .or_else(|| build.strip_suffix("_fixed"))
    else {
        return Ok(None);
    };
    Ok(load()?.get(name).cloned())
}

/// One bug by name, with a message listing the choices when it is unknown.
pub fn get(name: &str) -> Result<Bug> {
    let bugs = load()?;
    bugs.get(name).cloned().with_context(|| {
        let names: Vec<_> = bugs.keys().map(String::as_str).collect();
        format!("no bug '{name}' in bugs.yaml; known: {}", names.join(", "))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_parses_and_every_bug_has_two_kernels() {
        let bugs = load().unwrap();
        assert!(bugs.len() > 10);
        for bug in bugs.values() {
            let [buggy, fixed] = bug.kernels().unwrap();
            assert_eq!((buggy.name, fixed.name), ("buggy", "fixed"));
        }
        let sw = &bugs["sync_wakeup"];
        assert_eq!((sw.num_cpus, sw.mem_mb, sw.extra), (3, 128, false));
        assert_eq!(sw.fuzz.setup, ["create 3"]);
        assert!(sw.kernels().unwrap()[1]
            .patch
            .as_ref()
            .unwrap()
            .ends_with("linux/sync_wakeup.patch"));
        assert!(bugs["vlag_overflow"].extra);
        assert!(get("nope").unwrap_err().to_string().contains("sync_wakeup"));
    }
}
