#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 1024

int main(int argc, char *argv[]) {
  char *filename = NULL;
  int fd = STDOUT_FILENO;

  // Check for ">" in arguments
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], ">") == 0) {
      if (i + 1 < argc) {
        filename = argv[i + 1];
        argv[i] = NULL; // terminate the arguments for echo
        break;
      } else {
        fprintf(stderr, "echo: syntax error near unexpected token `newline'\n");
        return 1;
      }
    }
  }

  if (filename) {
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
      perror("open");
      return 1;
    }
  }

  // Print the arguments up to the ">" (if any)
  char buffer[MAX_LINE];
  int buffer_len = 0;
  for (int i = 1; argv[i] != NULL; ++i) {
    strncpy(buffer + buffer_len, argv[i], MAX_LINE - buffer_len);
    buffer_len += strlen(argv[i]);
    if (buffer_len >= MAX_LINE)
      break;
    if (argv[i + 1] != NULL)
      buffer[buffer_len++] = ' ';
    else
      buffer[buffer_len++] = '\n';
  }
  int written = write(fd, buffer, buffer_len);
  if (written != buffer_len) {
    perror("write");
    return 1;
  }

  if (fd != STDOUT_FILENO) {
    close(fd);
  }

  return 0;
}
