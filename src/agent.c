#include "agent.h"

#include "events_cc.h"

#include <stdlib.h>
#include <string.h>

struct AgentParser {
    AgentBackend backend;

    /* Line-buffer for partial NDJSON across feed() calls. */
    char  *line_buf;
    size_t line_len;
    size_t line_cap;

    /* Status state. */
    AgentStatus status;
    char        current_tool[64];   /* truncated copy of last seen tool name */
};

const char *agent_status_name(AgentStatus s) {
    switch (s) {
    case AC_AGENT_IDLE:            return "idle";
    case AC_AGENT_THINKING:        return "thinking";
    case AC_AGENT_TOOL:            return "tool";
    case AC_AGENT_STREAMING:       return "streaming";
    case AC_AGENT_AWAITING_INPUT:  return "awaiting_input";
    case AC_AGENT_DONE:            return "done";
    case AC_AGENT_ERROR:           return "error";
    }
    return "?";
}

AgentParser *agent_parser_create(AgentBackend backend) {
    AgentParser *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->backend = backend;
    p->status  = AC_AGENT_IDLE;
    return p;
}

void agent_parser_free(AgentParser *p) {
    if (!p) return;
    free(p->line_buf);
    free(p);
}

AgentStatus agent_parser_status(const AgentParser *p) {
    return p ? p->status : AC_AGENT_IDLE;
}

const char *agent_parser_current_tool(const AgentParser *p) {
    if (!p || p->status != AC_AGENT_TOOL) return NULL;
    return p->current_tool[0] ? p->current_tool : NULL;
}

static int line_buf_reserve(AgentParser *p, size_t needed) {
    if (needed <= p->line_cap) return 0;
    size_t new_cap = p->line_cap ? p->line_cap : 256;
    while (new_cap < needed) new_cap *= 2;
    char *nb = realloc(p->line_buf, new_cap);
    if (!nb) return -1;
    p->line_buf = nb;
    p->line_cap = new_cap;
    return 0;
}

static int line_buf_append(AgentParser *p, const char *src, size_t n) {
    if (line_buf_reserve(p, p->line_len + n + 1) != 0) return -1;
    memcpy(p->line_buf + p->line_len, src, n);
    p->line_len += n;
    p->line_buf[p->line_len] = '\0';
    return 0;
}

typedef struct {
    AgentParser   *parser;
    AgentEventEmit user_emit;
    void          *user_data;
    int            count;
} EmitCtx;

static void update_status(AgentParser *p, const AgentEvent *ev) {
    switch (ev->kind) {
    case AC_EV_THINKING_DELTA:
        p->status = AC_AGENT_THINKING;
        p->current_tool[0] = '\0';
        break;
    case AC_EV_ASSISTANT_TEXT_DELTA:
        p->status = AC_AGENT_STREAMING;
        p->current_tool[0] = '\0';
        break;
    case AC_EV_TOOL_CALL_STARTED: {
        p->status = AC_AGENT_TOOL;
        const char *name = ev->as.tool_call_started.name;
        if (name) {
            size_t n = strlen(name);
            if (n >= sizeof(p->current_tool)) n = sizeof(p->current_tool) - 1;
            memcpy(p->current_tool, name, n);
            p->current_tool[n] = '\0';
        } else {
            p->current_tool[0] = '\0';
        }
        break;
    }
    case AC_EV_TOOL_CALL_FINISHED:
        /* Returns to streaming/thinking; concrete next state will be set by
         * the next assistant block. Use STREAMING as a neutral "still working". */
        p->status = AC_AGENT_STREAMING;
        p->current_tool[0] = '\0';
        break;
    case AC_EV_TURN_DONE:
        p->status = AC_AGENT_IDLE;
        p->current_tool[0] = '\0';
        break;
    case AC_EV_ERROR:
        p->status = AC_AGENT_ERROR;
        break;
    default:
        break;
    }
}

static void wrap_emit(const AgentEvent *ev, void *user) {
    EmitCtx *ctx = user;
    update_status(ctx->parser, ev);
    if (ctx->user_emit) ctx->user_emit(ev, ctx->user_data);
    ctx->count++;
}

static int dispatch_one_line(AgentParser *p, const char *line, size_t len,
                             AgentEventEmit emit, void *user) {
    /* Skip empty lines (trailing \r, blank lines from concatenated jq output). */
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
    if (len == 0) return 0;

    EmitCtx ctx = { .parser = p, .user_emit = emit, .user_data = user, .count = 0 };
    int rc = -1;
    switch (p->backend) {
    case AC_BACKEND_CC:
        rc = events_cc_parse_one(line, len, wrap_emit, &ctx);
        break;
    case AC_BACKEND_CODEX:
        /* TODO: events_codex_parse_one once that backend lands. */
        rc = -1;
        break;
    }
    if (rc < 0) return -1;
    return ctx.count;
}

int agent_parser_feed(AgentParser *p, const char *bytes, size_t len,
                      AgentEventEmit emit, void *user) {
    if (!p || (!bytes && len > 0)) return -1;
    int total = 0;
    int parse_errors = 0;

    size_t cursor = 0;
    while (cursor < len) {
        const char *nl = memchr(bytes + cursor, '\n', len - cursor);
        if (!nl) {
            /* Tail without newline; carry into the line buffer. */
            if (line_buf_append(p, bytes + cursor, len - cursor) != 0) return -1;
            cursor = len;
            break;
        }
        size_t segment = (size_t)(nl - (bytes + cursor));

        /* Build full logical line from carry + new segment. */
        const char *line_ptr;
        size_t      line_len;
        if (p->line_len > 0) {
            if (line_buf_append(p, bytes + cursor, segment) != 0) return -1;
            line_ptr = p->line_buf;
            line_len = p->line_len;
        } else {
            line_ptr = bytes + cursor;
            line_len = segment;
        }

        int n = dispatch_one_line(p, line_ptr, line_len, emit, user);
        if (n < 0)      parse_errors++;
        else            total += n;

        /* Reset carry for next line. */
        p->line_len = 0;
        if (p->line_buf) p->line_buf[0] = '\0';

        cursor += segment + 1; /* skip newline */
    }

    if (parse_errors > 0 && total == 0) return -1;
    return total;
}
