#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>

#define OUTPUT_H_FILE "rq_fields_gen.h"
#define OUTPUT_C_FILE "rq_fields_gen.c"

// Check if a BTF type is an integer type (signed or unsigned)
static int is_integer_type(const struct btf *btf, int type_id, int *size, int *is_signed) {
    const struct btf_type *t;
    
    // Skip modifiers (const, volatile, typedef, etc.)
    while (type_id > 0) {
        t = btf__type_by_id(btf, type_id);
        if (!t) return 0;
        
        __u32 kind = btf_kind(t);
        switch (kind) {
            case BTF_KIND_TYPEDEF:
            case BTF_KIND_CONST:
            case BTF_KIND_VOLATILE:
            case BTF_KIND_RESTRICT:
                type_id = t->type;
                continue;
            case BTF_KIND_INT: {
                __u32 info = *(__u32 *)(t + 1);
                *size = BTF_INT_BITS(info) / 8;
                *is_signed = !!(BTF_INT_ENCODING(info) & BTF_INT_SIGNED);
                return 1;
            }
            case BTF_KIND_ENUM:
                *size = t->size;
                *is_signed = 1;
                return 1;
            default:
                return 0;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *btf_path = NULL;
    const char *output_dir = NULL;

    if (argc > 1) btf_path = argv[1];
    if (argc > 2) output_dir = argv[2];

    char output_h_path[256], output_c_path[256];
    if (output_dir) {
        snprintf(output_h_path, sizeof(output_h_path), "%s/%s", output_dir, OUTPUT_H_FILE);
        snprintf(output_c_path, sizeof(output_c_path), "%s/%s", output_dir, OUTPUT_C_FILE);
    } else {
        snprintf(output_h_path, sizeof(output_h_path), "%s", OUTPUT_H_FILE);
        snprintf(output_c_path, sizeof(output_c_path), "%s", OUTPUT_C_FILE);
    }
    
    struct btf *btf = btf__parse(btf_path, NULL);
    if (libbpf_get_error(btf)) {
        fprintf(stderr, "btf__parse failed for %s\n", btf_path);
        return 1;
    }

    int rq_id = btf__find_by_name_kind(btf, "rq", BTF_KIND_STRUCT);
    if (rq_id < 0) {
        fprintf(stderr, "struct rq not found in BTF\n");
        btf__free(btf);
        return 1;
    }

    const struct btf_type *rq_t = btf__type_by_id(btf, rq_id);
    __u16 vlen = btf_vlen(rq_t);
    const struct btf_member *m = btf_members(rq_t);

    FILE *out = fopen(output_h_path, "w");
    if (!out) {
        perror("fopen");
        btf__free(btf);
        return 1;
    }

    // Write header
    fprintf(out, "/* Auto-generated from BTF - DO NOT EDIT */\n");
    fprintf(out, "#ifndef RQ_FIELDS_H\n");
    fprintf(out, "#define RQ_FIELDS_H\n\n");
    fprintf(out, "#include <linux/stddef.h>\n\n");
    
    fprintf(out, "struct rq_int_field {\n");
    fprintf(out, "    const char *name;\n");
    fprintf(out, "    unsigned int offset;\n");
    fprintf(out, "    unsigned int size;\n");
    fprintf(out, "    int is_signed;\n");
    fprintf(out, "};\n\n");
    
    fprintf(out, "static const struct rq_int_field rq_int_fields[] = {\n");

    int count = 0;
    for (int i = 0; i < vlen; i++) {
        const char *mname = btf__name_by_offset(btf, m[i].name_off);
        if (!mname || !mname[0]) continue;
        
        __u32 bit_off = btf_member_bit_offset(rq_t, i);
        __u32 byte_off = bit_off / 8;
        
        // Skip bitfields for simplicity
        if (btf_member_bitfield_size(rq_t, i)) continue;
        
        int size, is_signed;
        if (is_integer_type(btf, m[i].type, &size, &is_signed)) {
            fprintf(out, "    {\"%s\", %u, %d, %d},\n", 
                    mname, byte_off, size, is_signed);
            count++;
        }
    }
    
    fprintf(out, "    {NULL, 0, 0, 0}  /* sentinel */\n");
    fprintf(out, "};\n\n");
    
    fprintf(out, "#define RQ_INT_FIELD_COUNT %d\n\n", count);
    
    // Generate a helper macro for printing all fields in one pr_info
    fprintf(out, "/* Helper macro to print all int fields of an rq pointer in one call */\n");
    fprintf(out, "#define TRACE_RQ_INT_FIELDS(rq_ptr) \\\n");
    fprintf(out, "    pr_info(\"rq: {");
    
    // Build format string
    for (int i = 0; i < vlen; i++) {
        const char *mname = btf__name_by_offset(btf, m[i].name_off);
        if (!mname || !mname[0]) continue;
        if (btf_member_bitfield_size(rq_t, i)) continue;
        
        int size, is_signed;
        if (is_integer_type(btf, m[i].type, &size, &is_signed)) {
            fprintf(out, "%s=%%lld", mname);
            // Check if more fields follow
            int has_more = 0;
            for (int j = i + 1; j < vlen; j++) {
                const char *next_name = btf__name_by_offset(btf, m[j].name_off);
                if (!next_name || !next_name[0]) continue;
                if (btf_member_bitfield_size(rq_t, j)) continue;
                int ns, nsg;
                if (is_integer_type(btf, m[j].type, &ns, &nsg)) { has_more = 1; break; }
            }
            if (has_more) fprintf(out, ", ");
        }
    }
    fprintf(out, "}\\n\"");
    
    // Build arguments
    for (int i = 0; i < vlen; i++) {
        const char *mname = btf__name_by_offset(btf, m[i].name_off);
        if (!mname || !mname[0]) continue;
        if (btf_member_bitfield_size(rq_t, i)) continue;
        
        __u32 bit_off = btf_member_bit_offset(rq_t, i);
        __u32 byte_off = bit_off / 8;
        
        int size, is_signed;
        if (is_integer_type(btf, m[i].type, &size, &is_signed)) {
            const char *cast_type;
            switch (size) {
                case 1: cast_type = is_signed ? "signed char" : "unsigned char"; break;
                case 2: cast_type = is_signed ? "short" : "unsigned short"; break;
                case 4: cast_type = is_signed ? "int" : "unsigned int"; break;
                default: cast_type = "long long"; break;
            }
            fprintf(out, ", \\\n        (long long)*(%s *)((char *)(rq_ptr) + %u)", cast_type, byte_off);
        }
    }
    fprintf(out, ")\n\n");
    
    fprintf(out, "#endif /* RQ_FIELDS_H */\n");
    
    fclose(out);
    btf__free(btf);

    // Write a .c file that contains the kprobe handler and init
    FILE *c_out = fopen(output_c_path, "w");
    if (!c_out) {
        perror("fopen");
        return 1;
    }
    fprintf(c_out, "#include \"%s\"\n", OUTPUT_H_FILE);
    fprintf(c_out, "#include \"internal.h\"\n\n");

    // Include kprobes.h
    fprintf(c_out, "#include <linux/kprobes.h>\n\n");
    
    // Kprobe handler (for now, only sched_tick is supported)
    // TODO: add other sched functions support
    fprintf(c_out, "static int sched_tick_handler(struct kprobe *kp, struct pt_regs *regs) {\n");
    fprintf(c_out, "    /* Print caller: return address is at top of stack on x86_64 */\n");
    fprintf(c_out, "    pr_info(\"sched_tick called from: %%pS\\n\", (void *)*(unsigned long *)regs->sp);\n");
    fprintf(c_out, "    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {\n");
    fprintf(c_out, "        TRACE_RQ_INT_FIELDS(cpu_rq(cpu));\n");
    fprintf(c_out, "    }\n");
    fprintf(c_out, "    return 0;\n");
    fprintf(c_out, "}\n\n");
    
    // Kprobe struct
    fprintf(c_out, "static struct kprobe kstep_rq_trace_kp = {\n");
    fprintf(c_out, "    .symbol_name = \"sched_tick\",\n");
    fprintf(c_out, "    .pre_handler = sched_tick_handler,\n");
    fprintf(c_out, "};\n\n");
    
    // Init function
    fprintf(c_out, "int kstep_rq_trace_init(void) {\n");
    fprintf(c_out, "    int ret = register_kprobe(&kstep_rq_trace_kp);\n");
    fprintf(c_out, "    if (ret < 0) {\n");
    fprintf(c_out, "        pr_err(\"Failed to register kprobe: %%d\\n\", ret);\n");
    fprintf(c_out, "        return ret;\n");
    fprintf(c_out, "    }\n");
    fprintf(c_out, "    return 0;\n");
    fprintf(c_out, "}\n\n");
    
    fclose(c_out);
    
    printf("Generated %s with %d integer fields from struct rq\n", output_h_path, count);
    printf("Generated %s with helper macro for printing rq fields\n", output_c_path);
    return 0;
}
