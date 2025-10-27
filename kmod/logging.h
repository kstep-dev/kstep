#ifndef LOGGING_H
#define LOGGING_H

#include <linux/printk.h>

#ifndef TRACE_LEVEL
#define TRACE_LEVEL LOGLEVEL_INFO
#endif

#define TERM_GRAY "\033[90m"
#define TERM_RED "\033[91m"
#define TERM_GREEN "\033[92m"
#define TERM_YELLOW "\033[93m"
#define TERM_BLUE "\033[94m"
#define TERM_MAGENTA "\033[95m"
#define TERM_CYAN "\033[96m"
#define TERM_RESET "\033[0m"

#define TERM_COLOR_DEBUG TERM_BLUE
#define TERM_COLOR_INFO TERM_GREEN
#define TERM_COLOR_ERR TERM_RED

#define TRACE_PRINT(level, fmt, ...)                                           \
  (void)(LOGLEVEL_##level <= TRACE_LEVEL &&                                    \
         (printk(KERN_INFO TERM_COLOR_##level                                  \
                 "[%20s:%-3d] %24s: " fmt TERM_RESET "\n",                     \
                 __FILE_NAME__, __LINE__, __func__, ##__VA_ARGS__),            \
          0))

#define TRACE_DEBUG(fmt, ...) TRACE_PRINT(DEBUG, fmt, ##__VA_ARGS__)
#define TRACE_INFO(fmt, ...) TRACE_PRINT(INFO, fmt, ##__VA_ARGS__)
#define TRACE_ERR(fmt, ...) TRACE_PRINT(ERR, fmt, ##__VA_ARGS__)

#endif
