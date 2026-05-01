#include <string.h>

#include "utils.h"

int init_main(int argc, char **argv, char **envp);
int task_main(int argc, char **argv);

int main(int argc, char **argv, char **envp) {
  const char *name = strrchr(argv[0], '/');
  name = name ? name + 1 : argv[0];
  if (!strcmp(name, "init"))
    return init_main(argc, argv, envp);
  if (!strcmp(name, "task"))
    return task_main(argc, argv);
  panic("Unknown argv[0]: %s", argv[0]);
}
