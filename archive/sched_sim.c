#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched.h>

// Linux private headers
#include <kernel/sched/sched.h>

#define KERNEL_SYMBOL_LIST                                                     \
  X(void, enqueue_task, (struct rq * rq, struct task_struct * p, int flags))   \
  X(bool, dequeue_task, (struct rq * rq, struct task_struct * p, int flags))   \
  X(void, init_cfs_rq, (struct cfs_rq * cfs_rq))                               \
  X(void, init_rt_rq, (struct rt_rq * rt_rq))                                  \
  X(void, init_dl_rq, (struct dl_rq * dl_rq))                                  \
  X(void, fair_server_init, (struct rq * rq))                                  \
  X(void, init_tg_cfs_entry,                                                   \
    (struct task_group * tg, struct cfs_rq * cfs_rq, struct sched_entity * se, \
     int cpu, struct sched_entity *parent))                                    \
  X(int, sched_fork, (unsigned long clone_flags, struct task_struct *p))       \
  X(int, init_rootdomain, (struct root_domain * rd))                           \
  X(void, rq_attach_root, (struct rq * rq, struct root_domain * rd))           \
  X(void, init_cfs_bandwidth,                                                  \
    (struct cfs_bandwidth * cfs_bandwidth,                                     \
     struct cfs_bandwidth * parent_cfs_bandwidth))
#include "kernel_sym.h"
#include "utils.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler simulator");

// Global data structures
struct root_domain rd;
struct rq rq_array[NR_CPUS];
struct sched_entity se_array[NR_CPUS];
struct cfs_rq cfs_rq_array[NR_CPUS];
struct task_group root_task_group;
LIST_HEAD(task_groups);

struct task_spec {
  int pid;
  int cpu;
};

static struct task_struct *create_tasks(struct task_spec specs[], int n) {
  struct task_struct *tasks =
      kcalloc(n, sizeof(struct task_struct), GFP_KERNEL);
  for (int i = 0; i < n; i++) {
    kernel_sched_fork(0, &tasks[i]);
    tasks[i].pid = specs[i].pid;
    tasks[i].se.cfs_rq = &rq_array[specs[i].cpu].cfs;
    strcpy(tasks[i].comm, "test");
  }
  return tasks;
}

static void update_clock(struct rq *rq) {
  static int clock = 0;
  clock += 10000000;
  rq->clock = clock;
}

static void init_sched(void) {
  // from sched_init
  pr_info("Scheduler initializing\n");
#ifdef CONFIG_FAIR_GROUP_SCHED
  root_task_group.se = (struct sched_entity **)&se_array;
  root_task_group.cfs_rq = (struct cfs_rq **)&cfs_rq_array;
  root_task_group.shares = ROOT_TASK_GROUP_LOAD;
  kernel_init_cfs_bandwidth(&root_task_group.cfs_bandwidth, NULL);
#endif

  kernel_init_rootdomain(&rd);

#ifdef CONFIG_CGROUP_SCHED
  list_add(&root_task_group.list, &task_groups);
  INIT_LIST_HEAD(&root_task_group.children);
  INIT_LIST_HEAD(&root_task_group.siblings);
#endif

  int i;
  for_each_possible_cpu(i) {
    struct rq *rq = &rq_array[i];

    kernel_init_cfs_rq(&rq->cfs);
    kernel_init_rt_rq(&rq->rt);
    kernel_init_dl_rq(&rq->dl);

#ifdef CONFIG_FAIR_GROUP_SCHED
    INIT_LIST_HEAD(&rq->cfs.leaf_cfs_rq_list);
    rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
    kernel_init_tg_cfs_entry(&root_task_group, &rq->cfs, NULL, i, NULL);
#endif
    rq->cpu_capacity = SCHED_CAPACITY_SCALE;
    rq->cpu = i;

    INIT_LIST_HEAD(&rq->cfs_tasks);
    kernel_rq_attach_root(rq, &rd);

    kernel_fair_server_init(rq);

    rq->curr = &init_task;
  }

  pr_info("Scheduler initialized\n");
}

static int __init sched_sim(void) {
  init_kernel_symbols();
  init_sched();

  struct task_struct *tasks = create_tasks(
      (struct task_spec[]){
          {.pid = 10000, .cpu = 0},
          {.pid = 10001, .cpu = 0},
          {.pid = 10002, .cpu = 0},
      },
      3);

  struct rq *rq = &rq_array[0];

  // Enqueue tasks
  for (int i = 0; i < 3; i++) {
    tasks[i].se.vruntime = (3 - i) * 10000000;
    kernel_enqueue_task(rq, &tasks[i], 0);
    update_clock(rq);
    pr_info("Enqueued task %d on cpu %d\n", tasks[i].pid, i);
    print_rq(rq);
  }

  // Dequeue tasks
  for (int i = 0; i < 3; i++) {
    kernel_dequeue_task(rq, &tasks[i], 0);
    update_clock(rq);
    pr_info("Dequeued task %d on cpu %d\n", tasks[i].pid, i);
    print_rq(rq);
  }
  pr_info("Scheduler simulation complete\n");
  return 0;
}
module_init(sched_sim);
