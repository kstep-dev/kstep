// kSTEP's I/O: the channel to the host, and the JSON records that travel on it.
//
// The channel is the virtio console port, driven by the kmod itself. The kernel's virtio_console
// is not built, so the device is unclaimed until this driver binds at module load; QEMU's side
// (virtio-serial + virtconsole, single port, no multiport) is unchanged. Owning the virtqueues is
// what lets any context send a record: a scheduler hook on an isolated CPU, interrupts off,
// runqueue locks held, adds a buffer under a raw spinlock and kicks, which is an MMIO write. The
// tty path could not be used from there (it sleeps and wakes), which is why records used to be
// buffered and written by the controller.
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_console.h>
#include <linux/virtio_ids.h>
#include <linux/wait.h>

#include "internal.h"

#define TX_BUFS 256 // records in flight to the host
#define TX_BUF 512  // one record (a kstep_json)
#define RX_BUFS 4
#define RX_BUF 256

static struct virtqueue *rx_vq, *tx_vq;
static struct completion probed;

// Buffers a virtqueue maps must be in the linear map (kmalloc), not module data (vmalloc).
static char *tx_buf;           // TX_BUFS * TX_BUF
static char *tx_free[TX_BUFS]; // stack of unused tx buffers
static unsigned int tx_nfree;
static DEFINE_RAW_SPINLOCK(tx_lock);

static char *rx_buf; // RX_BUFS * RX_BUF
static char rx_ring[4096]; // bytes received, waiting for the controller
static unsigned int rx_head, rx_tail;
static DEFINE_RAW_SPINLOCK(rx_lock);
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

static void tx_reclaim(void) { // tx_lock held
  unsigned int len;
  char *buf;

  while ((buf = virtqueue_get_buf(tx_vq, &len)))
    tx_free[tx_nfree++] = buf;
}

static void tx_done(struct virtqueue *vq) {
  unsigned long flags;

  raw_spin_lock_irqsave(&tx_lock, flags);
  tx_reclaim();
  raw_spin_unlock_irqrestore(&tx_lock, flags);
}

// Any context. The host consumes buffers in order, so a full pool means it is behind by
// TX_BUFS records; reclaiming here covers the common case of a missed tx interrupt.
void kstep_chan_write(const char *data, size_t len) {
  struct scatterlist sg;
  unsigned long flags;
  char *buf;

  if (len > TX_BUF)
    panic("record of %zu bytes exceeds the channel buffer", len);
  raw_spin_lock_irqsave(&tx_lock, flags);
  if (!tx_nfree)
    tx_reclaim();
  if (!tx_nfree)
    panic("channel: %d records in flight, the host is not reading", TX_BUFS);
  buf = tx_free[--tx_nfree];
  memcpy(buf, data, len);
  sg_init_one(&sg, buf, len);
  if (virtqueue_add_outbuf(tx_vq, &sg, 1, buf, GFP_ATOMIC) < 0)
    panic("channel: virtqueue_add_outbuf failed");
  virtqueue_kick(tx_vq);
  raw_spin_unlock_irqrestore(&tx_lock, flags);
}

static void rx_post(char *buf) {
  struct scatterlist sg;

  sg_init_one(&sg, buf, RX_BUF);
  if (virtqueue_add_inbuf(rx_vq, &sg, 1, buf, GFP_ATOMIC) < 0)
    panic("channel: virtqueue_add_inbuf failed");
}

static void rx_done(struct virtqueue *vq) {
  unsigned long flags;
  unsigned int len;
  char *buf;

  raw_spin_lock_irqsave(&rx_lock, flags);
  while ((buf = virtqueue_get_buf(vq, &len))) {
    for (unsigned int i = 0; i < len; i++) {
      if (rx_head - rx_tail == sizeof(rx_ring))
        panic("channel: input overflow");
      rx_ring[rx_head++ % sizeof(rx_ring)] = buf[i];
    }
    rx_post(buf);
  }
  virtqueue_kick(vq);
  raw_spin_unlock_irqrestore(&rx_lock, flags);
  wake_up(&rx_wait);
}

// The controller: the next line of input, without its newline, sleeping until one is complete.
// Longer lines are cut to max - 1 bytes.
void kstep_chan_readline(char *line, size_t max) {
  unsigned long flags;
  size_t len = 0;
  char c;

  do {
    wait_event(rx_wait, READ_ONCE(rx_head) != READ_ONCE(rx_tail));
    raw_spin_lock_irqsave(&rx_lock, flags);
    c = rx_ring[rx_tail++ % sizeof(rx_ring)];
    raw_spin_unlock_irqrestore(&rx_lock, flags);
    if (c != '\n' && len + 1 < max)
      line[len++] = c;
  } while (c != '\n');
  line[len] = '\0';
}

static int chan_probe(struct virtio_device *vdev) {
  struct virtqueue *vqs[2];
  struct virtqueue_info info[2] = {{"rx", rx_done}, {"tx", tx_done}};
  int err = virtio_find_vqs(vdev, 2, vqs, info, NULL);

  if (err)
    return err;
  rx_vq = vqs[0];
  tx_vq = vqs[1];
  tx_buf = kmalloc(TX_BUFS * TX_BUF, GFP_KERNEL);
  rx_buf = kmalloc(RX_BUFS * RX_BUF, GFP_KERNEL);
  if (!tx_buf || !rx_buf)
    return -ENOMEM;
  for (int i = 0; i < TX_BUFS; i++)
    tx_free[tx_nfree++] = tx_buf + i * TX_BUF;
  virtio_device_ready(vdev);
  for (int i = 0; i < RX_BUFS; i++)
    rx_post(rx_buf + i * RX_BUF);
  virtqueue_kick(rx_vq);
  complete(&probed);
  return 0;
}

static void chan_remove(struct virtio_device *vdev) {}

static const struct virtio_device_id chan_ids[] = {{VIRTIO_ID_CONSOLE, VIRTIO_DEV_ANY_ID}, {0}};

static struct virtio_driver chan_driver = {
    .driver.name = "kstep_chan",
    .id_table = chan_ids,
    .probe = chan_probe,
    .remove = chan_remove,
};

void kstep_io_init(void) {
  init_completion(&probed);
  if (register_virtio_driver(&chan_driver))
    panic("channel: register_virtio_driver failed");
  if (!wait_for_completion_timeout(&probed, HZ))
    panic("channel: no virtio console device (is CONFIG_VIRTIO_CONSOLE off and virtconsole attached?)");
}

// ---- JSON records ----

static void kstep_json_append_char(struct kstep_json *json, char c) {
  if (json->len + 1 >= sizeof(json->buf))
    panic("json buffer overflow");
  json->buf[json->len++] = c;
}

static void kstep_json_append_buf(struct kstep_json *json, const char *buf,
                                  size_t len) {
  if (json->len + len >= sizeof(json->buf))
    panic("json buffer overflow");
  memcpy(json->buf + json->len, buf, len);
  json->len += len;
}

static void kstep_json_append_str(struct kstep_json *json, const char *str) {
  kstep_json_append_char(json, '"');
  kstep_json_append_buf(json, str, strlen(str));
  kstep_json_append_char(json, '"');
}

static void kstep_json_field_vfmt(struct kstep_json *json, const char *key,
                                   const char *val_fmt, va_list args) {
  kstep_json_append_str(json, key);
  kstep_json_append_char(json, ':');

  int rem = sizeof(json->buf) - json->len;
  int len = vsnprintf(json->buf + json->len, rem, val_fmt, args);
  if (len < 0 || len >= rem)
    panic("json formatting failed");
  json->len += len;
  kstep_json_append_char(json, ',');
}

void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *val_fmt, ...) {
  va_list args;
  va_start(args, val_fmt);
  kstep_json_field_vfmt(json, key, val_fmt, args);
  va_end(args);
}

void kstep_json_field_str(struct kstep_json *json, const char *key,
                          const char *val) {
  kstep_json_append_str(json, key);
  kstep_json_append_char(json, ':');
  kstep_json_append_str(json, val);
  kstep_json_append_char(json, ',');
}

void kstep_json_field_u64(struct kstep_json *json, const char *key, u64 val) {
  kstep_json_field_fmt(json, key, "%llu", val);
}

void kstep_json_field_s64(struct kstep_json *json, const char *key, s64 val) {
  kstep_json_field_fmt(json, key, "%lld", val);
}

void kstep_json_begin(struct kstep_json *json) {
  json->len = 0;
  kstep_json_append_char(json, '{');
  kstep_json_field_u64(json, "timestamp", kstep_jiffies_get());
}

void kstep_json_field_bool(struct kstep_json *json, const char *key, bool val) {
  kstep_json_field_fmt(json, key, "%s", val ? "true" : "false");
}

void kstep_json_end(struct kstep_json *json) {
  if (json->len > 0 && json->buf[json->len - 1] == ',')
    json->len--;
  kstep_json_append_char(json, '}');
  kstep_json_append_char(json, '\n');
  kstep_chan_write(json->buf, json->len); // from any context: the channel is ours (above)
}

void kstep_json_print_2kv(const char *key1, const char *val1, const char *key2,
                          const char *val2_fmt, ...) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, key1, val1);

  va_list args;
  va_start(args, val2_fmt);
  kstep_json_field_vfmt(&json, key2, val2_fmt, args);
  va_end(args);

  kstep_json_end(&json);
}

void kstep_print_sched_debug(void) {
  KSYM_IMPORT(sysrq_sched_debug_show);
  KSYM_sysrq_sched_debug_show();
}

void kstep_output_curr_task(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct task_struct *curr = cpu_rq(cpu)->curr;
    struct kstep_json json;
    kstep_json_begin(&json);
    kstep_json_field_str(&json, "type", "curr_task");
    kstep_json_field_u64(&json, "cpu", cpu);
    kstep_json_field_u64(&json, "pid", task_pid_nr(curr));
    kstep_json_end(&json);
  }
}

void kstep_output_nr_running(void) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "nr_running");
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    char key[8];
    snprintf(key, sizeof(key), "cpu%d", cpu);
    kstep_json_field_u64(&json, key, cpu_rq(cpu)->nr_running);
  }
  kstep_json_end(&json);
}

void kstep_output_balance(int cpu, struct sched_domain *sd) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "load_balance");
  kstep_json_field_u64(&json, "dst_cpu", cpu);
  kstep_json_field_fmt(&json, "span", "\"%*pbl\"",
                       cpumask_pr_args(sched_domain_span(sd)));
  kstep_json_field_str(&json, "name", sd->name);
  kstep_json_end(&json);
}
