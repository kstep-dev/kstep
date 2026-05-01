# Uclamp Effective Value Hierarchical Consistency
**Source bug:** `7226017ad37a888915628e59a84a2d1e57b40707`

**Property:** For any online task group with a parent, the effective uclamp value must equal `min(requested, parent_effective)` — i.e., a child's effective clamp can never exceed what its parent allows.

**Variables:**
- `tg->uclamp[clamp_id].value` — the effective uclamp value for this task group. Read in-place from `struct task_group`.
- `tg->uclamp_req[clamp_id].value` — the requested (user-configured) uclamp value for this task group. Read in-place from `struct task_group`.
- `parent->uclamp[clamp_id].value` — the effective uclamp value of the parent task group. Read in-place by following `tg->css.parent` to get the parent `task_group`.

All three are fields on existing structs; no shadow variables needed.

**Check(s):**

Check 1: Performed at the end of `cpu_cgroup_css_online()`. Precondition: `CONFIG_UCLAMP_TASK_GROUP` is enabled and the task group has a parent.
```c
#ifdef CONFIG_UCLAMP_TASK_GROUP
struct task_group *tg = css_tg(css);
struct task_group *parent = css_tg(css->parent);
if (parent) {
    unsigned int clamp_id;
    for_each_clamp_id(clamp_id) {
        unsigned int eff = tg->uclamp[clamp_id].value;
        unsigned int req = tg->uclamp_req[clamp_id].value;
        unsigned int parent_eff = parent->uclamp[clamp_id].value;
        unsigned int expected = min(req, parent_eff);
        SCHED_WARN_ON(eff != expected);
    }
}
#endif
```

Check 2: Performed at the end of `cpu_util_update_eff()` (after propagation completes), for each task group visited. Same precondition.
```c
// Inside the css_for_each_descendant_pre loop, after updating tg->uclamp:
if (parent_tg) {
    for_each_clamp_id(clamp_id) {
        unsigned int eff = tg->uclamp[clamp_id].value;
        unsigned int req = tg->uclamp_req[clamp_id].value;
        unsigned int parent_eff = parent_tg->uclamp[clamp_id].value;
        SCHED_WARN_ON(eff != min(req, parent_eff));
    }
}
```

**Example violation:** A new child cgroup is created under root_task_group. `alloc_uclamp_sched_group()` copies parent's effective `uclamp[UCLAMP_MIN]=1024` into the child, but the child's `uclamp_req[UCLAMP_MIN]=0`. Without the `cpu_util_update_eff()` call, the effective value remains 1024 instead of `min(0, 1024)=0`, violating the invariant.

**Other bugs caught:** Potentially `uclamp_tg_restrict_inversion` (if it involves hierarchy propagation errors). Any future bug where uclamp effective values fail to be recomputed after a cgroup topology or configuration change.
