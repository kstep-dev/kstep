#include <stdio.h>
#include <string.h>

#define MAX_LINE 1024

int main(int argc, char *argv[]) {
  int redirect = 0;
  char *filename = NULL;
  FILE *out = stdout;

  // Check for ">" in arguments
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], ">") == 0) {
      if (i + 1 < argc) {
        redirect = 1;
        filename = argv[i + 1];
        argv[i] = NULL; // terminate the arguments for echo
        break;
      } else {
        fprintf(stderr, "echo: syntax error near unexpected token `newline'\n");
        return 1;
      }
    }
  }

  if (redirect) {
    out = fopen(filename, "w");
    if (!out) {
      perror("echo");
      return 1;
    }
  }

  // Print the arguments up to the ">" (if any)
  for (int i = 1; argv[i] != NULL; ++i) {
    fputs(argv[i], out);
    if (argv[i + 1] != NULL)
      fputc(' ', out);
  }
  fputc('\n', out);

  if (redirect && out != stdout) {
    fclose(out);
  }

  return 0;
}
