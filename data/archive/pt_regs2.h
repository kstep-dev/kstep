#ifndef KSTEP_PT_REGS_H
#define KSTEP_PT_REGS_H

#include <linux/kprobes.h>

// From tools/lib/bpf/bpf_tracing.h and arch/*/lib/error-injection.h
#if defined(CONFIG_X86_64)

// PARMs
#define PT_REGS_PARM1(x) ((x)->di)

// Override the function with return without executing the original codes
asmlinkage void just_return_func(void);
asm(
  ".text\n"
	".type just_return_func, @function\n"
	".globl just_return_func\n"
	"just_return_func:\n"
	"	ret\n"
	".size just_return_func, .-just_return_func\n"
);
inline void override_function_with_ret(struct pt_regs *regs) {
  (regs)->ip = (unsigned long)&just_return_func;
}

#elif defined(CONFIG_ARM64)

#define PT_REGS_PARM1(x) ((x)->regs[0])

inline void override_function_with_ret(struct pt_regs *regs) {
  (regs)->ip = regs[30];
}


#else
#error "Only support x86_64 and arm64"
#endif

#endif