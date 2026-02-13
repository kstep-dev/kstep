#include <linux/fs.h>     // filp_open, filp_close
#include <linux/kcov.h>
#include <linux/slab.h>   // kmalloc, kfree, krealloc
#include <linux/string.h> // strlen, memcmp, strstr

#include "driver.h"
#include "internal.h"

struct kcov_unique_pcs {
  char **pcs;
  size_t len;
  size_t cap;
};


#define KCOV_TYPE_MAX (KCOV_TYPE_KMOD + 1)
static struct kcov_unique_pcs unique[KCOV_TYPE_MAX] = {};
static struct file *module_kcov_file = NULL;

#define MODULE_KCOV_WORDS (1u << 17)

// Start coverage collection for the current module
void kcov_start(void) {
  if (module_kcov_file)
    return;

  module_kcov_file = filp_open("/sys/kernel/debug/kcov", O_RDWR, 0);
  if (IS_ERR(module_kcov_file))
    panic("Failed to open /sys/kernel/debug/kcov");
  if (vfs_ioctl(module_kcov_file, KCOV_INIT_TRACE, MODULE_KCOV_WORDS))
    panic("KCOV_INIT_TRACE failed for module coverage");

  if (vfs_ioctl(module_kcov_file, KCOV_ENABLE, KCOV_TRACE_PC))
    panic("KCOV_ENABLE failed for module coverage");
}

// Stop coverage collection for the current module
void kcov_stop(void) {
  unsigned long *area = current->kcov_area;
  unsigned long words = current->kcov_size;

  if (vfs_ioctl(module_kcov_file, KCOV_DISABLE, 0))
    panic("KCOV_DISABLE failed for module coverage");

  if (!area || words <= 1)
    return;

  unsigned long n = area[0];
  if (n > words - 1)
    n = words - 1;

  for (unsigned long i = 0; i < n; i++)
    kcov_collect_pcs(area[i + 1], KCOV_TYPE_KMOD);

  module_kcov_file = NULL;
}

// Helper function to manage unique PCs for the current module and userspace tasks
static void kcov_unique_pcs_add(const char *pc, size_t pc_len, enum kcov_type type) {
  struct kcov_unique_pcs *set = &unique[type];
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

void kcov_collect_pcs(unsigned long pc, enum kcov_type type) {
  char pc_hex[sizeof("ffffffffffffffff")];
  int len = scnprintf(pc_hex, sizeof(pc_hex), "%lx", pc);
  kcov_unique_pcs_add(pc_hex, len, type);
}

static void kcov_collect_pcs_file_internal(const char *kcov_file_path,
                                           enum kcov_type type) {
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
        kcov_unique_pcs_add(line, line_len, type);
        line_len = 0;
        continue;
      }
      if (line_len + 1 < 256)
        line[line_len++] = buf[i];
    }
  }
  if (line_len > 0)
    kcov_unique_pcs_add(line, line_len, type);

  kfree(buf);
  kfree(line);
  filp_close(file, NULL);
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

void kcov_collect_pcs_file(const char *kcov_file_path, enum kcov_type type) {
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

  kcov_collect_pcs_file_internal(kcov_file_path, type);
}


// Helper function to convert unique PCs to JSON
static void kcov_unique_pcs_json(enum kcov_type type) {
  struct kcov_unique_pcs *set = &unique[type];
  struct kstep_json *json = kstep_json_begin();
  struct kstep_json_list *list = 
      kstep_json_list_field_begin(json, 
        type == KCOV_TYPE_USER ? "user_kcov_pcs" : 
        type == KCOV_TYPE_KMOD ? "kmod_kcov_pcs" : 
        NULL);

  for (size_t i = 0; i < set->len; i++)
    kstep_json_list_append_string(list, set->pcs[i], strlen(set->pcs[i]));

  kstep_json_list_end(list);
  kstep_json_end(json);
}

void kcov_flush_json(void) {
  for (enum kcov_type type = 0; type < KCOV_TYPE_MAX; type++)
    kcov_unique_pcs_json(type);
}
