#ifndef AC_AGENT_H
#define AC_AGENT_H

#include "events.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AC_BACKEND_CC,
    AC_BACKEND_CODEX,
} AgentBackend;

typedef enum {
    AC_AGENT_IDLE,
    AC_AGENT_THINKING,
    AC_AGENT_TOOL,
    AC_AGENT_STREAMING,
    AC_AGENT_AWAITING_INPUT,
    AC_AGENT_DONE,
    AC_AGENT_ERROR,
} AgentStatus;

const char *agent_status_name(AgentStatus s);

typedef struct AgentParser AgentParser;

AgentParser *agent_parser_create(AgentBackend backend);
void         agent_parser_free(AgentParser *p);

/*
 * Feed `len` bytes of raw NDJSON output from the child. Buffers any partial
 * trailing line internally. Each complete top-level JSON object is dispatched
 * to the backend-specific parser, and emitted AgentEvents are passed through
 * to `emit`. The parser also updates internal AgentStatus on relevant events.
 *
 * Returns: total events emitted, or -1 on a JSON parse error (parser state
 * recovers — bad lines are dropped, the line buffer is reset to the start of
 * the next line).
 */
int agent_parser_feed(AgentParser *p, const char *bytes, size_t len,
                      AgentEventEmit emit, void *user);

AgentStatus agent_parser_status(const AgentParser *p);

/* Name of the currently-executing tool when status == AC_AGENT_TOOL,
 * else NULL. Borrowed pointer; valid until the next agent_parser_feed. */
const char *agent_parser_current_tool(const AgentParser *p);

#endif
