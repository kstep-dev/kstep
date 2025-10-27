// Forward declarations
struct task_struct;
enum sigcode;
struct cpumask;

void send_sigcode(struct task_struct *p, enum sigcode code, int val);
struct task_struct *poll_task(const char *comm);

extern const struct cpumask *cpu_controlled_mask;
void cpu_controlled_mask_init(void);
