#define _GNU_SOURCE

#include <ctype.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_PATH 1024
#define PROMPT "\033[0;32m# \033[0m"

#define CGROUP_ROOT "/sys/fs/cgroup"

// mount cgroup filesystem
int mount_cgroup_filesystem() {
  if (mount("none", CGROUP_ROOT, "cgroup2", 0, NULL) < 0) {
    if (errno == EBUSY) {
        printf("cgroup2 already mounted on %s\n", CGROUP_ROOT);
    } else {
        perror("mount");
        return 1;
    }
  } else {
    printf("Mounted cgroup2 on %s\n", CGROUP_ROOT);
  }
  return 0;
}

// Mount filesystems
int mount_filesystems() {
  if (mount("none", "/proc", "proc", 0, "") != 0) {
    perror("mount");
    return 1;
  }
  if (mount("none", "/sys", "sysfs", 0, "") != 0) {
    perror("mount");
    return 1;
  }
  if (mount("none", "/sys/kernel/debug", "debugfs", 0, "") != 0) {
    perror("mount");
    return 1;
  }
  if (mount("none", "/sys/fs/cgroup", "cgroup2", 0, "") != 0) {
    perror("mount");
    return 1;
  }
  return 0;
}

void set_cpu_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

// Built-in shell commands
int builtin_cd(char **args) {
  if (args[1] == NULL) {
    fprintf(stderr, "cd: expected argument\n");
    return 1;
  }
  if (chdir(args[1]) != 0) {
    perror("cd");
  }
  return 0;
}

int builtin_pwd() {
  char cwd[MAX_PATH];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    printf("%s\n", cwd);
  }
  return 0;
}

int builtin_exit() { return reboot(RB_POWER_OFF); }

int builtin_help() {
  printf("Built-in commands:\n");
  printf("  help            - Show this help\n");
  printf("  cd <dir>        - Change directory\n");
  printf("  pwd             - Print working directory\n");
  printf("  Ctrl+C / exit   - Exit the shell\n");
  printf("External commands:\n");
  printf("  ps              - List processes\n");
  printf("  ls <dir>        - List directory:         `ls /proc`\n");
  printf("  cat <file>      - Print file contents:    `cat "
         "/sys/kernel/debug/sched/debug`\n");
  printf("  insmod <file>   - Load a kernel module:   `insmod trace.ko`\n");
  printf("  rmmod <module>  - Unload a kernel module: `rmmod trace`\n");
  return 0;
}

// Array of built-in commands
struct builtin {
  char *name;
  int (*func)(char **);
};

struct builtin builtins[] = {
    {"cd", builtin_cd},
    {"pwd", builtin_pwd},
    {"exit", builtin_exit},
    {"help", builtin_help},
    {},
};

// Check if command is built-in
int is_builtin(char *cmd) {
  for (int i = 0; builtins[i].name != NULL; i++) {
    if (strcmp(cmd, builtins[i].name) == 0) {
      return i;
    }
  }
  return -1;
}

// Parse input line into arguments
char **parse_line(char *line) {
  static char *args[MAX_ARGS];
  int argc = 0;
  char *token = strtok(line, " \t\n\r");

  while (token != NULL && argc < MAX_ARGS - 1) {
    args[argc++] = token;
    token = strtok(NULL, " \t\n\r");
  }
  args[argc] = NULL;
  return args;
}

// Execute external command
int execute_external(char **args) {
  pid_t pid = fork();

  if (pid == 0) {
    // Child process
    execvp(args[0], args);
    perror("execvp");
    _exit(1);
  } else if (pid > 0) {
    // Parent process
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
      int exit_status = WEXITSTATUS(status);
      if (exit_status != 0) {
        printf("Command %s exited with status %d\n", args[0], exit_status);
      }
      return exit_status;
    } else {
      printf("Command %s exited with unknown status %d\n", args[0], status);
      return 1;
    }
  } else {
    perror("fork");
    return 1;
  }
  return 0;
}

// Main shell loop
void shell_loop() {
  builtin_help();

  while (1) {
    printf(PROMPT);
    fflush(stdout);

    char line[MAX_LINE];
    if (fgets(line, sizeof(line), stdin) == NULL) {
      printf("\n");
      break;
    }

    char **args = parse_line(line);
    if (args[0] == NULL)
      continue;

    int index = is_builtin(args[0]);
    if (index >= 0) {
      builtins[index].func(args);
    } else {
      execute_external(args);
    }
  }
  builtin_exit();
}

int system(char *cmd) {
  printf(PROMPT "%s\n", cmd);
  char line[MAX_LINE];
  strncpy(line, cmd, MAX_LINE);
  char **args = parse_line(line);
  return execute_external(args);
}

void run_init_sh() {
  FILE *file = fopen("init.sh", "r");
  if (file == NULL) {
    perror("fopen");
    return;
  }
  char line[MAX_LINE];
  while (fgets(line, sizeof(line), file) != NULL) {
    // Remove trailing newline
    line[strcspn(line, "\n")] = 0;

    // Trim leading whitespace
    char *cmd = line;
    while (isspace(*cmd)) {
      cmd++;
    }

    // Ignore empty lines and comments
    if (cmd[0] == '\0' || cmd[0] == '#') {
      continue;
    }

    if (system(cmd) != 0) {
      fprintf(stderr,
              "init.sh: command `%s` failed, aborting initialization.\n", cmd);
      break;
    }
  }
  fclose(file);
}

void run_sched_test(int argc, char *argv[], char *envp[]) {
  char *cmdline[MAX_ARGS] = {
      "insmod",
      "schedtest.ko",
  };
  int cmdline_len = 2;
  for (int i = 2; i < argc; i++) {
    cmdline[cmdline_len++] = argv[i];
  }
  for (int i = 2; envp[i] != NULL; i++) {
    cmdline[cmdline_len++] = envp[i];
  }
  cmdline[cmdline_len] = NULL;
  execute_external(cmdline);
}

int main(int argc, char *argv[], char *envp[]) {
  printf("\n");
  printf("Welcome to SchedTest\n");

  // Basic setup
  mount_filesystems();
  mount_cgroup_filesystem();
  set_cpu_affinity();
  signal(SIGCHLD, SIG_IGN);

  run_sched_test(argc, argv, envp);
  run_init_sh();
  shell_loop();
  builtin_exit();
  return 0;
}
