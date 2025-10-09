#ifndef LOGGING_H
#define LOGGING_H

#include <linux/printk.h>

#ifndef TRACE_LEVEL
#define TRACE_LEVEL LOGLEVEL_INFO
#endif

#define TRACE_PRINT(level, fmt, ...)                                           \
  (void)(LOGLEVEL_##level <= TRACE_LEVEL &&                                    \
         (printk(KERN_##level "[%12s:%-3d] %16s: " fmt, __FILE_NAME__,         \
                 __LINE__, __func__, ##__VA_ARGS__),                           \
          0))

#define TRACE_DEBUG(fmt, ...) TRACE_PRINT(DEBUG, fmt, ##__VA_ARGS__)
#define TRACE_INFO(fmt, ...) TRACE_PRINT(INFO, fmt, ##__VA_ARGS__)
#define TRACE_ERROR(fmt, ...) TRACE_PRINT(ERR, fmt, ##__VA_ARGS__)

#define TRACE_CONT(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)

#endif
