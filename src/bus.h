#ifndef AC_BUS_H
#define AC_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AC_LOG_TOOL_CALL,
    AC_LOG_RATE_LIMIT,
    AC_LOG_HOOK,
    AC_LOG_STATUS,
    AC_LOG_ERROR,
} LogKind;

typedef struct {
    uint64_t serial;             /* assigned by bus_push on success */
    int64_t  timestamp_ms;       /* unix ms; caller-provided */
    char     agent_id[16];
    LogKind  kind;
    char     oneline[160];
    char     tool_call_id[48];
    char     parent_tool_use_id[48];
    uint64_t transcript_serial;  /* opaque ref into the agent's transcript */
} LogEntry;

typedef struct EventBus EventBus;

EventBus *bus_create(size_t capacity);
void      bus_free(EventBus *b);

/* Push a copy. Returns the assigned serial (>= 1). */
uint64_t bus_push(EventBus *b, const LogEntry *e);

size_t   bus_size(const EventBus *b);
size_t   bus_capacity(const EventBus *b);

bool     bus_get(const EventBus *b, size_t offset_from_head, LogEntry *out);
bool     bus_get_by_serial(const EventBus *b, uint64_t serial, LogEntry *out);

uint64_t bus_oldest_serial(const EventBus *b);
uint64_t bus_newest_serial(const EventBus *b);

#endif
