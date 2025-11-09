#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "../kmod/sigcode.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

# define MAX_CGROUP_LEVELS 8
# define MAX_ID_IN_LEVEL 65536
# define MAX_CGROUP_PATH_LENGTH 128

void write_file(const char *path, const char *value) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    perror("open cgroup file failed");
    return;
  }
  if (write(fd, value, strlen(value)) < 0) {
    perror("write cgroup file failed");
  }
  close(fd);
}

int nextId[MAX_CGROUP_LEVELS];
char parent_cgroup_paths[MAX_CGROUP_LEVELS][MAX_ID_IN_LEVEL][MAX_CGROUP_PATH_LENGTH];

static void init_cgroup_nextId() {
  for (int i = 0; i < MAX_CGROUP_LEVELS; i++) {
    nextId[i] = 0;
  }
}

static void init_cgroup_parent_paths() {
  for (int i = 0; i < MAX_CGROUP_LEVELS; i++) {
    for (int j = 0; j < MAX_ID_IN_LEVEL; j++) {
      strcpy(parent_cgroup_paths[i][j], "");
    }
  }
}

static int form_subtree_control_path(char *path, size_t path_size, const char *parent_path) {
  size_t written = snprintf(path, path_size, 
                            "%s/cgroup.subtree_control", 
                            parent_path);
  return written > 0 && written < path_size;
}

static int form_cpuset_path(char *path, size_t path_size, const char *parent_path) {
  size_t written = snprintf(path, path_size, 
                            "%s/cpuset.cpus", 
                            parent_path);
  return written > 0 && written < path_size;
}

static int form_weight_path(char *path, size_t path_size, const char *parent_path) {
  size_t written = snprintf(path, path_size, 
                            "%s/cpu.weight", 
                            parent_path);
  return written > 0 && written < path_size;
}

static int make_child_cgroup_path(char *path, size_t path_size, 
                                  const char *parent_path, int child_level_id, int child_id_in_level) {
  size_t written = snprintf(path, path_size, 
                            "%s/l%d_%d", parent_path, 
                            child_level_id, child_id_in_level);
  return written > 0 && written < path_size;
}

static int form_cpuset_buffer(char *buf, size_t buf_size) {
  size_t len = snprintf(buf, buf_size, 
                        "%ld-%ld", 1L, sysconf(_SC_NPROCESSORS_ONLN) - 1L);
  return len > 0 && len < buf_size;
}

static void signal_handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  int val2 = info->si_pid;
  int val3 = info->si_uid;
  if (code == SIGCODE_CGROUP_CREATE) {
    
    // parse the parent cgroup path si_int: {parent_level_id}_{parent_id_in_level}
    // create a child cgroup under the parent cgroup
    int parent_level_id = (val >> 16) & 0xFFFF;
    int parent_id_in_level = val & 0xFFFF;
    int child_level_id = parent_level_id + 1;
    int child_id_in_level = nextId[child_level_id];

    char parent_path[MAX_CGROUP_PATH_LENGTH];
    char child_path[MAX_CGROUP_PATH_LENGTH];
    char parent_cgroup_control_path[MAX_CGROUP_PATH_LENGTH];
    char cpuset_path[MAX_CGROUP_PATH_LENGTH];
    char cpus[10];

    // if the parent_level_id is 0, the parent_path is the root cgroup
    if (parent_level_id == 0) {
      if(parent_id_in_level != 0) { perror("root cgroup id != 0"); return; }
      strcpy(parent_path, CGROUP_ROOT);
    } else if (!make_child_cgroup_path(parent_path, sizeof(parent_path), 
                                       parent_cgroup_paths[parent_level_id][parent_id_in_level], 
                                       parent_level_id, parent_id_in_level)) {
      perror("make_child_cgroup_path failed");
      return;
    }

    // check if the parent cgroup exists
    if (access(parent_path, F_OK) != 0) {
      perror("[SIGCODE_CGROUP_CREATE] parent_path does not exists\n");
      return;
    }

    // write +cpu +cpuset to the parent cgroup.subtree_control
    if (!form_subtree_control_path(parent_cgroup_control_path, 
                                   sizeof(parent_cgroup_control_path), 
                                   parent_path)) {
      perror("form_subtree_control_path failed");
      return;
    }
    write_file(parent_cgroup_control_path, "+cpu +cpuset");

    // create the child cgroup path
    if (!make_child_cgroup_path(child_path, sizeof(child_path), 
                                parent_path, child_level_id, child_id_in_level)) {
      perror("make_child_cgroup_path failed");
      return;
    }

    // create the child cgroup
    if (mkdir(child_path, 0755))
        perror("mkdir my_group failed");
    else {
      strcpy(parent_cgroup_paths[child_level_id][child_id_in_level], parent_path);
      nextId[child_level_id]++;
    }

    // set the cpuset to our testing cpu range
    if (!form_cpuset_path(cpuset_path, sizeof(cpuset_path), child_path)) {
      perror("form_cpuset_path failed");
      return;
    }
    if (!form_cpuset_buffer(cpus, sizeof(cpus))) {
      perror("form_cpuset_buffer failed");
      return;
    }
    write_file(cpuset_path, cpus);

  } else if (code == SIGCODE_REWEIGHT_CGROUP) {
    int level_id = (val >> 16) & 0xFFFF;
    int id_in_level = val & 0xFFFF;

    char cgroup_path[MAX_CGROUP_PATH_LENGTH];
    char weight_path[MAX_CGROUP_PATH_LENGTH];
    if (!make_child_cgroup_path(cgroup_path, sizeof(cgroup_path), 
                                parent_cgroup_paths[level_id][id_in_level], 
                                level_id, id_in_level)) {
      perror("make_child_cgroup_path failed");
      return;
    }
    if (!form_weight_path(weight_path, sizeof(weight_path), cgroup_path)) {
      perror("form_weight_path failed");
      return;
    }

    char buf[10];
    size_t len = snprintf(buf, sizeof(buf), "%d", val2);
    if (len > 0 && len < sizeof(buf)) {
      write_file(weight_path, buf);
    } else {
      perror("snprintf failed");
      return;
    }

  } else if (code == SIGCODE_SETCPU_CGROUP) {
    int level_id = (val >> 16) & 0xFFFF;
    int id_in_level = val & 0xFFFF;

    char cgroup_path[MAX_CGROUP_PATH_LENGTH];
    char cpuset_path[MAX_CGROUP_PATH_LENGTH];
    if (!make_child_cgroup_path(cgroup_path, sizeof(cgroup_path), 
                                parent_cgroup_paths[level_id][id_in_level], 
                                level_id, id_in_level)) {
      perror("make_child_cgroup_path failed");
      return;
    }
    // set the cpuset to our testing cpu range
    if (!form_cpuset_path(cpuset_path, sizeof(cpuset_path), cgroup_path)) {
      perror("form_cpuset_path failed");
      return;
    }
    char buf[10];
    size_t len;
    if (val3 == 0)
      len = snprintf(buf, sizeof(buf), "%d", val2);
    else
      len = snprintf(buf, sizeof(buf), "%d-%d", val2, val3);
    if (len > 0 && len < sizeof(buf)) {
      write_file(cpuset_path, buf);
    } else {
      perror("snprintf failed");
      return;
    }
  }
  else {
    printf("Unknown signal code: %d\n", code);
  }
}

static void set_proc_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset); // bind to cpu 0
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

int main() {
  struct sigaction sa = {.sa_sigaction = signal_handler,
                         .sa_flags = SA_SIGINFO};
  sigaction(SIGUSR1, &sa, NULL);
  init_cgroup_nextId();
  init_cgroup_parent_paths();

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  } else if (pid == 0) {
    // Child process
    set_proc_affinity();
    prctl(PR_SET_NAME, "cgroup-proc");
    while (1) {
        pause();
    }
    exit(0);
  }
  return 0;
}
