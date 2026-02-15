#include <linux/fs.h>     // filp_open, filp_close
#include <linux/slab.h>   // kmalloc, kfree, krealloc
#include <linux/string.h> // strlen, memcmp, strstr

#include "driver.h"
#include "internal.h"

struct kcov_unique_pcs {
  char **pcs;
  size_t len;
  size_t cap;
};

static struct kcov_unique_pcs unique = {};

static void kcov_unique_pcs_add(struct kcov_unique_pcs *set, const char *pc,
                                 size_t pc_len) {
  if (!pc_len)
    return;
  if (pc_len == 3 && memcmp(pc, "END", 3) == 0)
    return;

  for (size_t i = 0; i < set->len; i++) {
    if (strlen(set->pcs[i]) == pc_len && memcmp(set->pcs[i], pc, pc_len) == 0)
      return;
  }

  if (set->len == set->cap) {
    size_t new_cap = set->cap == 0 ? 64 : set->cap * 2;
    char **new_pcs = krealloc(set->pcs, new_cap * sizeof(*new_pcs), GFP_KERNEL);
    if (!new_pcs)
      panic("Failed to allocate unique KCOV set");
    set->pcs = new_pcs;
    set->cap = new_cap;
  }

  char *copy = kmalloc(pc_len + 1, GFP_KERNEL);
  if (!copy)
    panic("Failed to allocate unique KCOV entry");
  memcpy(copy, pc, pc_len);
  copy[pc_len] = '\0';
  set->pcs[set->len++] = copy;
}

static void kcov_collect_pcs_file(const char *kcov_file_path, struct kcov_unique_pcs *set) {
  struct file *file = filp_open(kcov_file_path, O_RDONLY, 0);
  if (IS_ERR(file))
    panic("Failed to open %s", kcov_file_path);

  char *buf = kmalloc(512, GFP_KERNEL);
  char *line = kmalloc(256, GFP_KERNEL);
  if (!buf || !line)
    panic("Failed to allocate buffer for reading %s", kcov_file_path);

  size_t line_len = 0;
  loff_t pos = 0;
  while (1) {
    ssize_t nread = kernel_read(file, buf, 512, &pos);
    if (nread <= 0)
      break;

    for (ssize_t i = 0; i < nread; i++) {
      if (buf[i] == '\n') {
        kcov_unique_pcs_add(set, line, line_len);
        line_len = 0;
        continue;
      }
      if (line_len + 1 < 256)
        line[line_len++] = buf[i];
    }
  }
  if (line_len > 0)
    kcov_unique_pcs_add(set, line, line_len);

  kfree(buf);
  kfree(line);
  filp_close(file, NULL);
}

static void kcov_unique_pcs_json(const struct kcov_unique_pcs *set) {
  struct kstep_json *json = kstep_json_begin();
  kstep_json_list_begin(json, "fork_kcov_pcs");

  for (size_t i = 0; i < set->len; i++)
    kstep_json_list_append_str(json, set->pcs[i], strlen(set->pcs[i]));

  kstep_json_list_end(json);
  kstep_json_end(json);
}

static void kcov_unique_pcs_free(struct kcov_unique_pcs *set) {
  for (size_t i = 0; i < set->len; i++)
    kfree(set->pcs[i]);
  kfree(set->pcs);
  set->pcs = NULL;
  set->len = 0;
  set->cap = 0;
}

static bool kcov_file_contains_end(const char *kcov_file_path) {
  struct file *file = filp_open(kcov_file_path, O_RDONLY, 0);
  if (IS_ERR(file))
    return false;

  char buf[512];
  loff_t pos = 0;
  bool found_end = false;

  while (1) {
    ssize_t nread = kernel_read(file, buf, sizeof(buf) - 1, &pos);
    if (nread <= 0)
      break;
    buf[nread] = '\0';
    if (strstr(buf, "END") != NULL) {
      found_end = true;
      break;
    }
  }

  filp_close(file, NULL);
  return found_end;
}

void kcov_collect_pcs(const char *kcov_file_path) {
  bool found_kcov_file = false;
  
  for (int j = 0; j < 10000; j++) {
    if (kcov_file_contains_end(kcov_file_path)) {
      found_kcov_file = true;
      break;
    }
    kstep_sleep();
  }
  if (!found_kcov_file) {
    pr_warn("Timed out waiting for %s\n", kcov_file_path);
    return;
  }

  kcov_collect_pcs_file(kcov_file_path, &unique);
}

void kcov_flush_json(void) {
  kcov_unique_pcs_json(&unique);
  kcov_unique_pcs_free(&unique);
}
