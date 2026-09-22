//! kmod/shm.h, byte for byte: the region of guest memory the cli driver rewrites after every
//! command. The structs come from the header itself, through bindgen (build.rs), as `raw`, so a
//! snapshot of the region is read as one `kstep_shm` and each table sliced by its count. `gen` is
//! odd while the writer is mid-update; a snapshot with an odd gen is `Busy`. That is the whole
//! check: the kmod rewrites the region before it replies, and a host snapshots after the reply,
//! so a snapshot never straddles an update. A host that read live memory concurrently would have
//! to compare gen before and after its copy itself.

use serde::Serialize;

/// kmod/shm.h as bindgen reads it.
#[allow(non_camel_case_types, non_upper_case_globals, dead_code)]
pub mod raw {
    include!(concat!(env!("OUT_DIR"), "/shm_h.rs"));
}
use raw::*;

pub const MAGIC: u32 = KSTEP_SHM_MAGIC;
pub const LAYOUT: u32 = KSTEP_SHM_LAYOUT;
/// The whole region: what to snapshot
pub const SIZE: usize = std::mem::size_of::<kstep_shm>();
/// cov.c's edge map: saturating byte counts
pub const COV_SIZE: usize = KSTEP_COV_SIZE as usize;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// Not a kSTEP region
    Magic(u32),
    /// A region this decoder was not built for: rebuild the image from the current kmod
    Layout(u32),
    /// Mid-update: read again
    Busy,
    /// The snapshot is shorter than the region
    Short,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            Error::Magic(m) => write!(f, "not a kSTEP shared region (magic {m:#x})"),
            Error::Layout(l) => write!(f, "shm layout {l}, this decoder speaks {LAYOUT}: rebuild the image from the current kmod"),
            Error::Busy => f.write_str("shm mid-update"),
            Error::Short => write!(f, "shm snapshot shorter than the region ({SIZE} bytes)"),
        }
    }
}
impl std::error::Error for Error {}

#[allow(clippy::unnecessary_cast)] // c_char is u8 on aarch64 and i8 on x86_64
fn cstr(s: &[std::os::raw::c_char]) -> String {
    let bytes: Vec<u8> = s
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

// ---- the decoded state: field names as kstep.mjs reports them, so the page can switch ----------

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct State {
    /// Logical ticks
    pub timestamp: u32,
    pub cpus: Vec<Cpu>,
    pub tasks: Vec<Task>,
    /// The cgroups, root first
    pub groups: Vec<Cgroup>,
    pub domains: Vec<Domain>,
    /// The fair class: its root queue per CPU, and every entity on it
    pub cfs: Vec<Cfs>,
    pub entities: Vec<Entity>,
    /// The real-time class: its queue per CPU, and the tasks on it
    pub rt: Vec<Rt>,
    pub rt_entities: Vec<RtEntity>,
}

/// The runqueue itself; each class's queue on it is in that class's table, joined by cpu.
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Cpu {
    pub cpu: u32,
    /// kSTEP task number running there, 0 for none or another process
    pub current: u32,
    pub idle: bool,
    pub capacity: u32,
    pub freq: u32,
    /// Ticks until rq->next_balance comes due, 0 when it already has
    pub next_balance_in: u32,
    pub nr_running: u64,
    pub nr_switches: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Task {
    pub task: u32,
    /// running, runnable, sleeping, blocked
    pub state: &'static str,
    pub cpu: u32,
    /// normal, fifo, rr, batch, idle
    pub policy: &'static str,
    pub nice: i32,
    /// 1..99 under fifo/rr, 0 otherwise
    pub rt_priority: u32,
    /// The cgroup's path
    pub cgroup: String,
    /// Allowed CPUs as a bitmask
    pub cpus: u64,
    pub sum_exec_runtime: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Cgroup {
    pub path: String,
    /// cpuset.cpus.effective as a bitmask
    pub cpus: u64,
    /// cpu.weight, 0 where the controller is off
    pub weight: u32,
}

/// rq->cfs on one CPU
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Cfs {
    pub cpu: u32,
    pub min_vruntime: i64,
    pub util_avg: u64,
    pub load_avg: u64,
    pub runnable_avg: u64,
    /// Runnable tasks as the balancer counts them
    pub h_nr_runnable: u64,
}

/// A sched_entity: a task's (task > 0) or a cgroup's on one CPU (task 0). The flags are the
/// kernel's own answers: eligible is entity_eligible, pick is pick_eevdf on its queue.
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Entity {
    pub task: u32,
    pub cgroup: String,
    pub cpu: u32,
    pub eligible: bool,
    pub delayed: bool,
    pub on_rq: bool,
    pub curr: bool,
    pub pick: bool,
    /// Of the CPU, 1.0 being all of it
    pub share: f64,
    pub weight: u64,
    pub sum_exec_runtime: u64,
    pub vruntime: i64,
    pub deadline: i64,
    pub slice: u64,
    /// Queue average vruntime minus this entity's, weighted: 0 fair, positive owed time
    pub lag: i64,
}

/// rq->rt on one CPU
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Rt {
    pub cpu: u32,
    pub nr_running: u32,
    /// 0 (highest) .. 98; 100 when empty
    pub highest_prio: u32,
    pub throttled: bool,
    pub rt_time: u64,
    pub rt_runtime: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct RtEntity {
    pub task: u32,
    pub cpu: u32,
    pub on_rq: bool,
    pub curr: bool,
    /// The head of the highest list: what pick_next_task_rt would take
    pub pick: bool,
    /// Place in its priority's list, 0 the head
    pub position: u32,
    /// RR time left, in ticks
    pub time_slice: u32,
}

/// One sched domain as the kernel built it, per CPU, innermost first.
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Domain {
    pub cpu: u32,
    pub span: u64,
    /// SMT, CLS, MC, PKG, NODE
    pub name: String,
    /// SD_* names, "SD_" stripped
    pub flags: String,
    pub imbalance_pct: u32,
    pub balance_interval: u32,
    pub busy_factor: u32,
    pub cache_nice_tries: u32,
    pub nr_balance_failed: u32,
    pub last_balance_ago: u32,
    pub groups: Vec<Group>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Group {
    pub span: u64,
    pub capacity: u32,
    pub min_capacity: u32,
    pub max_capacity: u32,
    pub weight: u32,
}

const TASK_STATES: [&str; 4] = ["running", "runnable", "sleeping", "blocked"];

fn policy(n: u32) -> &'static str {
    match n {
        0 => "normal",
        1 => "fifo",
        2 => "rr",
        3 => "batch",
        5 => "idle",
        _ => "?",
    }
}

/// The first `n` records of a table, or `Busy` when a half-written header claims more than fit.
fn table<R, T>(rows: &[R], n: u32, f: impl Fn(&R) -> T) -> Result<Vec<T>, Error> {
    rows.get(..n as usize)
        .ok_or(Error::Busy)
        .map(|rows| rows.iter().map(f).collect())
}

/// Decode one snapshot of the region (`SIZE` bytes). `Busy` means the writer was mid-update:
/// snapshot again.
pub fn decode(b: &[u8]) -> Result<State, Error> {
    if b.len() < SIZE {
        return Err(Error::Short);
    }
    // SAFETY: kstep_shm is a repr(C) struct of integers and byte arrays, so any SIZE bytes are a
    // valid value; read unaligned from a checked range.
    let r: kstep_shm = unsafe { std::ptr::read_unaligned(b.as_ptr() as *const kstep_shm) };
    let h = &r.hdr;
    if h.magic != MAGIC {
        return Err(Error::Magic(h.magic));
    }
    if h.layout != LAYOUT {
        return Err(Error::Layout(h.layout));
    }
    if h.gen_ & 1 == 1 {
        return Err(Error::Busy);
    }

    let cpus = table(&r.cpu, h.ncpus, |c| Cpu {
        cpu: c.cpu,
        current: c.curr,
        idle: c.idle != 0,
        capacity: c.capacity,
        freq: c.freq,
        next_balance_in: c.next_balance_in,
        nr_running: c.nr_running,
        nr_switches: c.nr_switches,
    })?;
    let groups = table(&r.cgroup, h.ncgroups, |g| Cgroup {
        path: cstr(&g.path),
        cpus: g.cpus,
        weight: g.weight,
    })?;
    let group_path = |i: u32| {
        groups
            .get(i as usize)
            .map_or_else(|| "/".to_string(), |g| g.path.clone())
    };
    let tasks = table(&r.task, h.ntasks, |t| Task {
        task: t.task,
        state: TASK_STATES.get(t.state as usize).copied().unwrap_or("?"),
        cpu: t.cpu,
        policy: policy(t.policy),
        nice: t.nice,
        rt_priority: t.rt_priority,
        cgroup: group_path(t.cgroup),
        cpus: t.cpus,
        sum_exec_runtime: t.sum_exec_runtime,
    })?;
    let cfs = table(&r.cfs, h.ncpus, |q| Cfs {
        cpu: q.cpu,
        min_vruntime: q.min_vruntime,
        util_avg: q.util_avg,
        load_avg: q.load_avg,
        runnable_avg: q.runnable_avg,
        h_nr_runnable: q.h_nr_runnable,
    })?;
    let entities = table(&r.entity, h.nentities, |e| Entity {
        task: e.task,
        cgroup: group_path(e.cgroup),
        cpu: e.cpu,
        eligible: e.se.flags & KSTEP_SE_ELIGIBLE != 0,
        delayed: e.se.flags & KSTEP_SE_DELAYED != 0,
        on_rq: e.se.flags & KSTEP_SE_ON_RQ != 0,
        curr: e.se.flags & KSTEP_SE_CURR != 0,
        pick: e.se.flags & KSTEP_SE_PICK != 0,
        share: e.se.share as f64 / 1024.0,
        weight: e.se.weight,
        sum_exec_runtime: e.se.sum_exec_runtime,
        vruntime: e.se.vruntime,
        deadline: e.se.deadline,
        slice: e.se.slice,
        lag: e.se.lag,
    })?;
    let rt = table(&r.rt, h.ncpus, |q| Rt {
        cpu: q.cpu,
        nr_running: q.nr_running,
        highest_prio: q.highest_prio,
        throttled: q.throttled != 0,
        rt_time: q.rt_time,
        rt_runtime: q.rt_runtime,
    })?;
    let rt_entities = table(&r.rt_entity, h.nrt_entities, |e| RtEntity {
        task: e.task,
        cpu: e.cpu,
        on_rq: e.flags & KSTEP_RT_ON_RQ != 0,
        curr: e.flags & KSTEP_RT_CURR != 0,
        pick: e.flags & KSTEP_RT_PICK != 0,
        position: e.position,
        time_slice: e.time_slice,
    })?;
    let domains = table(&r.domain, h.ndomains, |d| Domain {
        cpu: d.cpu,
        span: d.span,
        name: cstr(&d.name),
        flags: cstr(&d.flags),
        imbalance_pct: d.imbalance_pct,
        balance_interval: d.balance_interval,
        busy_factor: d.busy_factor,
        cache_nice_tries: d.cache_nice_tries,
        nr_balance_failed: d.nr_balance_failed,
        last_balance_ago: d.last_balance_ago,
        groups: d.group[..(d.ngroups as usize).min(d.group.len())]
            .iter()
            .map(|g| Group {
                span: g.span,
                capacity: g.capacity,
                min_capacity: g.min_capacity,
                max_capacity: g.max_capacity,
                weight: g.weight,
            })
            .collect(),
    })?;
    Ok(State {
        timestamp: h.timestamp,
        cpus,
        tasks,
        groups,
        domains,
        cfs,
        entities,
        rt,
        rt_entities,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bytes(r: &kstep_shm) -> Vec<u8> {
        // SAFETY: a plain repr(C) struct of integers, viewed as its bytes
        unsafe { std::slice::from_raw_parts(r as *const kstep_shm as *const u8, SIZE) }.to_vec()
    }

    /// A region built through the generated structs: one CPU running one rr task in cgroup /a.
    #[test]
    fn decodes_a_region() {
        let mut r = kstep_shm {
            hdr: kstep_shm_hdr {
                magic: MAGIC,
                layout: LAYOUT,
                gen_: 2,
                timestamp: 7,
                ncpus: 1,
                ntasks: 1,
                ncgroups: 1,
                ndomains: 0,
                nentities: 1,
                nrt_entities: 1,
            },
            ..Default::default()
        };
        r.cpu[0] = kstep_shm_cpu {
            cpu: 1,
            curr: 1,
            nr_running: 2,
            ..Default::default()
        };
        r.task[0] = kstep_shm_task {
            task: 1,
            state: 0,
            cpu: 1,
            policy: 2,
            nice: -3,
            rt_priority: 5,
            cgroup: 0,
            cpus: 0b110,
            ..Default::default()
        };
        r.cgroup[0].path[..2].copy_from_slice(&[b'/' as _, b'a' as _]);
        r.cgroup[0].weight = 100;
        r.cfs[0] = kstep_shm_cfs {
            cpu: 1,
            min_vruntime: 1000,
            ..Default::default()
        };
        r.entity[0] = kstep_shm_entity {
            task: 1,
            cpu: 1,
            se: kstep_shm_se {
                flags: KSTEP_SE_ELIGIBLE | KSTEP_SE_CURR,
                share: 512,
                lag: -8,
                ..Default::default()
            },
            ..Default::default()
        };
        r.rt[0] = kstep_shm_rt {
            cpu: 1,
            highest_prio: 100,
            ..Default::default()
        };
        r.rt_entity[0] = kstep_shm_rt_entity {
            task: 1,
            cpu: 1,
            flags: KSTEP_RT_ON_RQ | KSTEP_RT_PICK,
            time_slice: 99,
            ..Default::default()
        };

        let st = decode(&bytes(&r)).unwrap();
        assert_eq!(st.timestamp, 7);
        assert_eq!(
            st.cpus,
            [Cpu {
                cpu: 1,
                current: 1,
                idle: false,
                capacity: 0,
                freq: 0,
                next_balance_in: 0,
                nr_running: 2,
                nr_switches: 0
            }]
        );
        let t = &st.tasks[0];
        assert_eq!(
            (
                t.state,
                t.cpu,
                t.policy,
                t.nice,
                t.rt_priority,
                t.cgroup.as_str(),
                t.cpus
            ),
            ("running", 1, "rr", -3, 5, "/a", 0b110)
        );
        assert_eq!(
            (st.groups[0].path.as_str(), st.groups[0].weight),
            ("/a", 100)
        );
        assert_eq!(st.cfs[0].min_vruntime, 1000);
        let e = &st.entities[0];
        assert_eq!(
            (
                e.task,
                e.cgroup.as_str(),
                e.eligible,
                e.curr,
                e.pick,
                e.share,
                e.lag
            ),
            (1, "/a", true, true, false, 0.5, -8)
        );
        assert_eq!(
            (
                st.rt[0].highest_prio,
                st.rt_entities[0].pick,
                st.rt_entities[0].time_slice
            ),
            (100, true, 99)
        );
        assert!(st.domains.is_empty());

        r.hdr.gen_ = 3;
        assert_eq!(decode(&bytes(&r)), Err(Error::Busy));
        r.hdr.gen_ = 2;
        r.hdr.ntasks = KSTEP_SHM_TASKS + 1;
        assert_eq!(decode(&bytes(&r)), Err(Error::Busy));
        r.hdr.layout = LAYOUT + 1;
        assert_eq!(decode(&bytes(&r)), Err(Error::Layout(LAYOUT + 1)));
        assert_eq!(decode(&bytes(&r)[..100]), Err(Error::Short));
    }
}
