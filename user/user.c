#include <string.h>

#include "utils.h"

int init_main(int argc, char **argv, char **envp);
int task_main(int argc, char **argv);

int main(int argc, char **argv, char **envp) {
  if (argc > 0 && !strcmp(argv[0], "task"))
    return task_main(argc, argv);
  return init_main(argc, argv, envp);
}
