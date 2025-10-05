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
  return 0;
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
    builtin_exit();
  } else if (pid > 0) {
    // Parent process
    int status;
    waitpid(pid, &status, 0);
  } else {
    perror("fork");
    return 1;
  }
  return 1;
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

void system(char *cmd) {
  printf(PROMPT "%s\n", cmd);
  char line[MAX_LINE];
  strncpy(line, cmd, MAX_LINE);
  char **args = parse_line(line);
  execute_external(args);
}

int main() {
  printf("\n");
  printf("Welcome to UML Simple Root Filesystem\n");

  mount_filesystems();
  system("insmod freeze.ko cpu=1");
  system("cat /sys/kernel/debug/sched/debug");

  // builtin_exit();

  shell_loop();
  builtin_exit();
  return 0;
}
