//! kmod/shm.h, byte for byte: the region of guest memory the cli driver rewrites after every
//! command. Little-endian u32/u64 at natural alignment. The header describes the tables (offset,
//! stride, count), so only magic, layout and gen sit at fixed places; LAYOUT is bumped when a
//! record's fields change meaning without changing size. `gen` is a seqlock: odd mid-update,
//! and a read is good only if it is even and unchanged around it.
//!
//! The structs come from the header itself, through bindgen (build.rs), as `raw`; records are
//! read straight out of a snapshot with them.

use serde::Serialize;

/// kmod/shm.h as bindgen reads it.
#[allow(non_camel_case_types, non_upper_case_globals, dead_code)]
pub mod raw {
    include!(concat!(env!("OUT_DIR"), "/shm_h.rs"));
}
use raw::*;

pub const MAGIC: u32 = KSTEP_SHM_MAGIC;
pub const LAYOUT: u32 = KSTEP_SHM_LAYOUT;
pub const HDR_SIZE: usize = std::mem::size_of::<kstep_shm_hdr>();
/// cov.c's edge map: saturating byte counts
pub const COV_SIZE: usize = KSTEP_COV_SIZE as usize;
const MAX_SIZE: usize = 1 << 20; // sanity bound on what a header may claim
const NTABLES: usize = KSTEP_TBL_N as usize;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// Not a kSTEP region
    Magic(u32),
    /// A region this decoder was not built for: rebuild the image from the current kmod
    Layout(u32),
    /// The header claims an implausible size
    Size(usize),
    /// Mid-update: read again
    Busy,
    /// The snapshot is shorter than the layout says
    Short,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            Error::Magic(m) => write!(f, "not a kSTEP shared region (magic {m:#x})"),
            Error::Layout(l) => write!(f, "shm layout {l}, this decoder speaks {LAYOUT}: rebuild the image from the current kmod"),
            Error::Size(n) => write!(f, "shm header claims {n} bytes"),
            Error::Busy => f.write_str("shm mid-update"),
            Error::Short => f.write_str("shm snapshot shorter than its header says"),
        }
    }
}
impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy)]
pub struct Table {
    pub count_at: usize,
    pub max: usize,
    pub off: usize,
    pub stride: usize,
}

/// The shape of a region, read once from its header.
#[derive(Debug, Clone)]
pub struct Layout {
    /// kmod/shm.h's enum kstep_shm_tables order: cpu, task, cgroup, domain, cfs, entity, rt, rt_entity
    pub tables: [Table; NTABLES],
    pub max_groups: usize,
    pub group_stride: usize,
    /// Bytes to snapshot: the end of the farthest table
    pub size: usize,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
/// One record out of a snapshot. The generated structs hold only integers and byte arrays, so
/// any bytes are a valid value; the bounds check is the one thing that can fail.
fn rec<T: Copy>(b: &[u8], o: usize) -> T {
    let n = std::mem::size_of::<T>();
    assert!(o + n <= b.len(), "shm record at {o} runs past the snapshot");
    // SAFETY: T is a repr(C) struct of integers, read unaligned from a checked byte range.
    unsafe { std::ptr::read_unaligned(b.as_ptr().add(o) as *const T) }
}

#[allow(clippy::unnecessary_cast)] // c_char is u8 on aarch64 and i8 on x86_64
fn cstr(s: &[std::os::raw::c_char]) -> String {
    let bytes: Vec<u8> = s
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

impl Layout {
    /// From the first HDR_SIZE bytes of a region.
    pub fn parse(hdr: &[u8]) -> Result<Layout, Error> {
        if hdr.len() < HDR_SIZE {
            return Err(Error::Short);
        }
        let h: kstep_shm_hdr = rec(hdr, 0);
        if h.magic != MAGIC {
            return Err(Error::Magic(h.magic));
        }
        if h.layout != LAYOUT {
            return Err(Error::Layout(h.layout));
        }
        let tables = std::array::from_fn(|i| Table {
            count_at: std::mem::offset_of!(kstep_shm_hdr, table)
                + i * std::mem::size_of::<kstep_shm_table>(),
            max: h.table[i].max as usize,
            off: h.table[i].off as usize,
            stride: h.table[i].stride as usize,
        });
        let size = tables
            .iter()
            .map(|t| t.off + t.max * t.stride)
            .max()
            .unwrap();
        if size == 0 || size > MAX_SIZE {
            return Err(Error::Size(size));
        }
        Ok(Layout {
            tables,
            max_groups: h.max_groups as usize,
            group_stride: h.group_stride as usize,
            size,
        })
    }
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
    pub min_vruntime: u64,
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
    pub vruntime: u64,
    pub deadline: u64,
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

const GEN: usize = std::mem::offset_of!(kstep_shm_hdr, gen_);
const TIMESTAMP: usize = std::mem::offset_of!(kstep_shm_hdr, timestamp);

/// Decode one snapshot of the region. `Busy` means the writer was mid-update: snapshot again.
pub fn decode(b: &[u8], l: &Layout) -> Result<State, Error> {
    if b.len() < l.size {
        return Err(Error::Short);
    }
    let gen = u32_at(b, GEN);
    if gen & 1 == 1 {
        return Err(Error::Busy);
    }
    // one table's records at the stride the kmod declared; a count past the capacity is a
    // half-written header
    fn table<R: Copy, T>(b: &[u8], t: &Table, f: impl Fn(usize, R) -> T) -> Result<Vec<T>, Error> {
        let n = u32_at(b, t.count_at) as usize;
        if n > t.max {
            return Err(Error::Busy);
        }
        Ok((0..n)
            .map(|i| t.off + i * t.stride)
            .map(|o| f(o, rec(b, o)))
            .collect())
    }
    let [t_cpu, t_task, t_cgroup, t_domain, t_cfs, t_entity, t_rt, t_rte] = &l.tables;

    let cpus = table(b, t_cpu, |_, r: kstep_shm_cpu| Cpu {
        cpu: r.cpu,
        current: r.curr,
        idle: r.idle != 0,
        capacity: r.capacity,
        freq: r.freq,
        next_balance_in: r.next_balance_in,
        nr_running: r.nr_running,
        nr_switches: r.nr_switches,
    })?;
    let groups = table(b, t_cgroup, |_, r: kstep_shm_cgroup| Cgroup {
        path: cstr(&r.path),
        cpus: r.cpus,
        weight: r.weight,
    })?;
    let group_path = |i: u32| {
        groups
            .get(i as usize)
            .map_or_else(|| "/".to_string(), |g| g.path.clone())
    };
    let tasks = table(b, t_task, |_, r: kstep_shm_task| Task {
        task: r.task,
        state: TASK_STATES.get(r.state as usize).copied().unwrap_or("?"),
        cpu: r.cpu,
        policy: policy(r.policy),
        nice: r.nice,
        rt_priority: r.rt_priority,
        cgroup: group_path(r.cgroup),
        cpus: r.cpus,
        sum_exec_runtime: r.sum_exec_runtime,
    })?;
    let cfs = table(b, t_cfs, |_, r: kstep_shm_cfs| Cfs {
        cpu: r.cpu,
        min_vruntime: r.min_vruntime,
        util_avg: r.util_avg,
        load_avg: r.load_avg,
        runnable_avg: r.runnable_avg,
        h_nr_runnable: r.h_nr_runnable,
    })?;
    let entities = table(b, t_entity, |_, r: kstep_shm_entity| Entity {
        task: r.task,
        cgroup: group_path(r.cgroup),
        cpu: r.cpu,
        eligible: r.se.flags & KSTEP_SE_ELIGIBLE != 0,
        delayed: r.se.flags & KSTEP_SE_DELAYED != 0,
        on_rq: r.se.flags & KSTEP_SE_ON_RQ != 0,
        curr: r.se.flags & KSTEP_SE_CURR != 0,
        pick: r.se.flags & KSTEP_SE_PICK != 0,
        share: r.se.share as f64 / 1024.0,
        weight: r.se.weight,
        sum_exec_runtime: r.se.sum_exec_runtime,
        vruntime: r.se.vruntime,
        deadline: r.se.deadline,
        slice: r.se.slice,
        lag: r.se.lag,
    })?;
    let rt = table(b, t_rt, |_, r: kstep_shm_rt| Rt {
        cpu: r.cpu,
        nr_running: r.nr_running,
        highest_prio: r.highest_prio,
        throttled: r.throttled != 0,
        rt_time: r.rt_time,
        rt_runtime: r.rt_runtime,
    })?;
    let rt_entities = table(b, t_rte, |_, r: kstep_shm_rt_entity| RtEntity {
        task: r.task,
        cpu: r.cpu,
        on_rq: r.flags & KSTEP_RT_ON_RQ != 0,
        curr: r.flags & KSTEP_RT_CURR != 0,
        pick: r.flags & KSTEP_RT_PICK != 0,
        position: r.position,
        time_slice: r.time_slice,
    })?;
    let domains = table(b, t_domain, |o, r: kstep_shm_domain| Domain {
        cpu: r.cpu,
        span: r.span,
        name: cstr(&r.name),
        flags: cstr(&r.flags),
        imbalance_pct: r.imbalance_pct,
        balance_interval: r.balance_interval,
        busy_factor: r.busy_factor,
        cache_nice_tries: r.cache_nice_tries,
        nr_balance_failed: r.nr_balance_failed,
        last_balance_ago: r.last_balance_ago,
        groups: (0..(r.ngroups as usize).min(l.max_groups))
            .map(|j| {
                rec::<kstep_shm_group>(
                    b,
                    o + std::mem::offset_of!(kstep_shm_domain, group) + j * l.group_stride,
                )
            })
            .map(|g| Group {
                span: g.span,
                capacity: g.capacity,
                min_capacity: g.min_capacity,
                max_capacity: g.max_capacity,
                weight: g.weight,
            })
            .collect(),
    })?;
    if u32_at(b, GEN) != gen {
        return Err(Error::Busy);
    }
    Ok(State {
        timestamp: u32_at(b, TIMESTAMP),
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

/// Parse the header and decode, for a whole-region snapshot.
pub fn decode_region(b: &[u8]) -> Result<State, Error> {
    decode(b, &Layout::parse(b)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A region built by hand: the header as shm.c writes it, one CPU, one task, one cgroup.
    #[test]
    fn decodes_a_hand_built_region() {
        let mut b = vec![0u8; 4096];
        let put32 = |b: &mut [u8], o: usize, v: u32| b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        let put64 = |b: &mut [u8], o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
        put32(&mut b, 0, MAGIC);
        put32(&mut b, 4, LAYOUT);
        put32(&mut b, 8, 2); // gen: even
        put32(&mut b, 12, 7); // timestamp
                              // tables: (n, max, off, stride)
        let tables = [
            (1, 2, 160, 40),
            (1, 2, 240, 48),
            (1, 2, 336, 56),
            (0, 1, 448, 208 + 24),
            (1, 2, 680, 48),
            (1, 2, 776, 72),
            (1, 2, 920, 32),
            (0, 2, 984, 24),
        ];
        for (i, (n, max, off, stride)) in tables.iter().enumerate() {
            let o = 16 + i * 16;
            put32(&mut b, o, *n);
            put32(&mut b, o + 4, *max);
            put32(&mut b, o + 8, *off);
            put32(&mut b, o + 12, *stride);
        }
        put32(&mut b, 16 + 8 * 16, 1); // max_groups
        put32(&mut b, 20 + 8 * 16, 24); // group_stride
                                        // cpu 1 running task 1, 2 runnable
        put32(&mut b, 160, 1);
        put32(&mut b, 164, 1);
        put64(&mut b, 184, 2);
        // task 1: running, cpu 1, rr prio 5, cgroup 0
        put32(&mut b, 240, 1);
        put32(&mut b, 244, 0);
        put32(&mut b, 248, 1);
        put32(&mut b, 252, 2);
        put32(&mut b, 256, (-3i32) as u32);
        put32(&mut b, 260, 5);
        put64(&mut b, 272, 0b110);
        b[336..338].copy_from_slice(b"/a");
        put32(&mut b, 384, 100);
        // cfs cpu 1, entity task 1 eligible+curr with lag -8
        put32(&mut b, 680, 1);
        put64(&mut b, 688, 1000);
        put32(&mut b, 776, 1);
        put32(&mut b, 784, 1);
        put32(&mut b, 792, 1 | 8);
        put32(&mut b, 796, 512);
        put64(&mut b, 792 + 48, (-8i64) as u64);
        put32(&mut b, 920, 1);
        put32(&mut b, 928, 100);

        let st = decode_region(&b).unwrap();
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
        assert_eq!(st.rt[0].highest_prio, 100);
        assert!(st.domains.is_empty() && st.rt_entities.is_empty());

        put32(&mut b, 8, 3);
        assert_eq!(decode_region(&b), Err(Error::Busy));
        put32(&mut b, 4, LAYOUT + 1);
        assert_eq!(decode_region(&b), Err(Error::Layout(LAYOUT + 1)));
    }
}
