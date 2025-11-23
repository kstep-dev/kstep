#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINE 1024

int main() {
  printf("%-8s %-8s %-8s %s\n", "PID", "PPID", "STAT", "CMD");
  printf("%-8s %-8s %-8s %s\n", "---", "----", "----", "---");

  DIR *proc_dir = opendir("/proc");
  if (proc_dir == NULL) {
    perror("opendir /proc");
    return 1;
  }

  struct dirent *entry;
  while ((entry = readdir(proc_dir)) != NULL) {
    // Skip non-numeric directories
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }

    char stat_path[512];
    snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", entry->d_name);

    FILE *stat_file = fopen(stat_path, "r");
    if (stat_file == NULL) {
      continue;
    }

    char line[MAX_LINE];
    if (fgets(line, sizeof(line), stat_file) != NULL) {
      int pid, ppid;
      char state;
      char comm[256];

      // Parse the stat file format
      sscanf(line, "%d %s %c %d", &pid, comm, &state, &ppid);

      // Remove parentheses from comm
      if (comm[0] == '(' && comm[strlen(comm) - 1] == ')') {
        memmove(comm, comm + 1, strlen(comm) - 2);
        comm[strlen(comm) - 2] = '\0';
      }

      printf("%-8d %-8d %-8c %s\n", pid, ppid, state, comm);
    }

    fclose(stat_file);
  }

  closedir(proc_dir);
  return 0;
}
