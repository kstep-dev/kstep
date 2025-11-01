#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "../kmod/sigcode.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

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
# define MAX_CGROUP_LEVELS 8
# define MAX_ID_IN_LEVEL 65536
# define MAX_CGROUP_PATH_LENGTH 128
int nextId[MAX_CGROUP_LEVELS];

void init_cgroup_nextId() {
  for (int i = 0; i < MAX_CGROUP_LEVELS; i++) {
    nextId[i] = 0;
  }
}

char parent_cgroup_paths[MAX_CGROUP_LEVELS][MAX_ID_IN_LEVEL][MAX_CGROUP_PATH_LENGTH];

void init_cgroup_parent_paths() {
  for (int i = 0; i < MAX_CGROUP_LEVELS; i++) {
    for (int j = 0; j < MAX_ID_IN_LEVEL; j++) {
      strcpy(parent_cgroup_paths[i][j], "");
    }
  }
}

static void signal_handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  if (code == SIGCODE_FORK) {
    for (int i = 0; i < val; i++) {
      int pid = fork();
      if (pid == 0)
        return;
    }
  } else if (code == SIGCODE_SLEEP) {
    sleep(val);
  } else if (code == SIGCODE_EXIT) {
    exit(0);
  } else if (code == SIGCODE_PAUSE) {
    pause();
  } else if (code == SIGCODE_CGROUP_CREATE) {
    
    // parse the parent cgroup path si_int: {parent_level_id}_{parent_id_in_level}
    // create a child cgroup under the parent cgroup
    int parent_level_id = (val >> 16) & 0xFFFF;
    int parent_id_in_level = val & 0xFFFF;
    int child_level_id = parent_level_id + 1;
    int child_id_in_level = nextId[child_level_id];

    char parent_path[MAX_CGROUP_PATH_LENGTH];
    char child_path[MAX_CGROUP_PATH_LENGTH];

    // if the parent_level_id is 0, the parent_path is the root cgroup
    if (parent_level_id == 0) {
      if(parent_id_in_level != 0) perror("root cgroup id != 0");
      strcpy(parent_path, CGROUP_ROOT);
    } else { 
      snprintf(parent_path, sizeof(parent_path),  
        "%s/l%d_%d", 
               parent_cgroup_paths[parent_level_id][parent_id_in_level], 
               parent_level_id, 
               parent_id_in_level);
    }


    // check if the parent cgroup exists
    if (access(parent_path, F_OK) != 0) {
      perror("[SIGCODE_CGROUP_CREATE] parent_path does not exists\n");
      return;
    }

    // write +cpu to the parent cgroup.subtree_control
    char parent_cgroup_control_path[MAX_CGROUP_PATH_LENGTH];
    snprintf(parent_cgroup_control_path, sizeof(parent_cgroup_control_path), 
     "%s/cgroup.subtree_control", 
             parent_path);
    write_file(parent_cgroup_control_path, "+cpu");

    // create the child cgroup path
    snprintf(child_path, sizeof(child_path), 
      "%s/l%d_%d", parent_path, 
      child_level_id, 
      child_id_in_level);

    // create the child cgroup
    if (mkdir(child_path, 0755))
        perror("mkdir my_group failed");
    else {
      strcpy(parent_cgroup_paths[child_level_id][child_id_in_level], parent_path);
      nextId[child_level_id]++;
    }
    
  } else {
    printf("Unknown signal code: %d\n", code);
  }
}

static void set_proc_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  int nproc = sysconf(_SC_NPROCESSORS_ONLN);
  for (int i = 1; i < nproc; i++) { // skip cpu 0
    CPU_SET(i, &cpuset);
  }
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
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
    prctl(PR_SET_NAME, "test-proc");
    pause();
    loop();
    exit(0);
  }
  return 0;
}
