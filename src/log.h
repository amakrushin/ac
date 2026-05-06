#ifndef AC_LOG_H
#define AC_LOG_H

#include <stdarg.h>
#include <stdbool.h>

/* Open the global log file. Pass NULL to disable logging (the default).
 * If `path` already exists, the new log is appended. Returns true on success. */
bool log_open(const char *path);
void log_close(void);

bool log_enabled(void);
const char *log_path(void);

void log_writef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vwritef(const char *fmt, va_list ap);

#define LOG(...) do { if (log_enabled()) log_writef(__VA_ARGS__); } while (0)

#endif
