#include "agent_session.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct AgentSession {
    AgentBackend backend;
    char         id[16];
    Proc        *proc;
    AgentParser *parser;
    EventBus    *bus;
    AgentEventEmit user_emit;
    void          *user_data;

    pthread_t       reader;
    pthread_mutex_t mu;        /* guards parser */
    bool            reader_started;
    atomic_bool     stopping;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static LogKind log_kind_for(AgentEventKind k) {
    switch (k) {
    case AC_EV_TOOL_CALL_STARTED:
    case AC_EV_TOOL_CALL_FINISHED:  return AC_LOG_TOOL_CALL;
    case AC_EV_RATE_LIMIT:          return AC_LOG_RATE_LIMIT;
    case AC_EV_HOOK:                return AC_LOG_HOOK;
    case AC_EV_ERROR:               return AC_LOG_ERROR;
    default:                        return AC_LOG_STATUS;
    }
}

static void copy_truncated(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void on_event(const AgentEvent *ev, void *user) {
    AgentSession *s = user;

    /* Push a LogEntry to the bus for entries the dashboard cares about. */
    if (s->bus) {
        LogEntry e = (LogEntry){0};
        e.timestamp_ms = now_ms();
        copy_truncated(e.agent_id, sizeof(e.agent_id), s->id);
        e.kind = log_kind_for(ev->kind);

        switch (ev->kind) {
        case AC_EV_TOOL_CALL_STARTED:
            snprintf(e.oneline, sizeof(e.oneline), "%s %s",
                     ev->as.tool_call_started.name      ? ev->as.tool_call_started.name      : "?",
                     ev->as.tool_call_started.args_json ? ev->as.tool_call_started.args_json : "");
            copy_truncated(e.tool_call_id, sizeof(e.tool_call_id),
                           ev->as.tool_call_started.call_id);
            copy_truncated(e.parent_tool_use_id, sizeof(e.parent_tool_use_id),
                           ev->as.tool_call_started.parent_tool_use_id);
            bus_push(s->bus, &e);
            break;
        case AC_EV_TOOL_CALL_FINISHED:
            snprintf(e.oneline, sizeof(e.oneline), "%s %s",
                     ev->as.tool_call_finished.ok ? "✓" : "✗",
                     ev->as.tool_call_finished.result_summary
                        ? ev->as.tool_call_finished.result_summary : "");
            copy_truncated(e.tool_call_id, sizeof(e.tool_call_id),
                           ev->as.tool_call_finished.call_id);
            copy_truncated(e.parent_tool_use_id, sizeof(e.parent_tool_use_id),
                           ev->as.tool_call_finished.parent_tool_use_id);
            bus_push(s->bus, &e);
            break;
        case AC_EV_RATE_LIMIT:
            snprintf(e.oneline, sizeof(e.oneline), "rate-limit window=%s",
                     ev->as.rate_limit.window ? ev->as.rate_limit.window : "?");
            bus_push(s->bus, &e);
            break;
        case AC_EV_HOOK:
            snprintf(e.oneline, sizeof(e.oneline), "%s %s",
                     ev->as.hook.is_response ? "hook←" : "hook→",
                     ev->as.hook.hook_name ? ev->as.hook.hook_name : "?");
            bus_push(s->bus, &e);
            break;
        case AC_EV_ERROR:
            snprintf(e.oneline, sizeof(e.oneline), "error: %s",
                     ev->as.error_.message ? ev->as.error_.message : "?");
            bus_push(s->bus, &e);
            break;
        case AC_EV_ASSISTANT_TEXT_DELTA: {
            const char *t = ev->as.text_delta.text;
            if (t && *t) {
                snprintf(e.oneline, sizeof(e.oneline), "say: %.140s", t);
                e.kind = AC_LOG_STATUS;
                bus_push(s->bus, &e);
            }
            break;
        }
        case AC_EV_THINKING_DELTA: {
            const char *t = ev->as.thinking_delta.text;
            if (t && *t) {
                snprintf(e.oneline, sizeof(e.oneline), "thinking: %.140s", t);
                e.kind = AC_LOG_STATUS;
                bus_push(s->bus, &e);
            }
            break;
        }
        case AC_EV_SYSTEM_INIT:
            snprintf(e.oneline, sizeof(e.oneline), "init model=%s",
                     ev->as.system_init.model ? ev->as.system_init.model : "?");
            e.kind = AC_LOG_STATUS;
            bus_push(s->bus, &e);
            break;
        case AC_EV_TURN_DONE:
            snprintf(e.oneline, sizeof(e.oneline), "turn done");
            e.kind = AC_LOG_STATUS;
            bus_push(s->bus, &e);
            break;
        default:
            /* StatusChange and AssistantTextDone don't get their own log entries. */
            break;
        }
    }

    if (s->user_emit) s->user_emit(ev, s->user_data);
}

static void *reader_loop(void *arg) {
    AgentSession *s = arg;
    char buf[4096];
    while (!atomic_load(&s->stopping)) {
        ssize_t n = proc_read_stdout(s->proc, buf, sizeof(buf));
        if (n == 0) break;          /* EOF */
        if (n < 0) {
            if (atomic_load(&s->stopping)) break;
            break;                   /* on any read error, give up */
        }
        pthread_mutex_lock(&s->mu);
        agent_parser_feed(s->parser, buf, (size_t)n, on_event, s);
        pthread_mutex_unlock(&s->mu);
    }
    return NULL;
}

AgentSession *agent_session_attach(const AgentSessionAttachOpts *opts) {
    if (!opts || !opts->proc) return NULL;
    AgentSession *s = calloc(1, sizeof(*s));
    if (!s) { proc_free(opts->proc); return NULL; }

    s->backend = opts->backend;
    s->bus     = opts->bus;
    s->proc    = opts->proc;
    s->user_emit = opts->on_event;
    s->user_data = opts->user;
    if (opts->id) copy_truncated(s->id, sizeof(s->id), opts->id);
    else          copy_truncated(s->id, sizeof(s->id), "agent");

    s->parser = agent_parser_create(opts->backend);
    if (!s->parser) goto fail;
    if (pthread_mutex_init(&s->mu, NULL) != 0) goto fail;

    atomic_init(&s->stopping, false);

    if (pthread_create(&s->reader, NULL, reader_loop, s) != 0) {
        pthread_mutex_destroy(&s->mu);
        goto fail;
    }
    s->reader_started = true;
    return s;

fail:
    if (s->parser) agent_parser_free(s->parser);
    proc_free(s->proc);
    free(s);
    return NULL;
}

AgentSession *agent_session_spawn(AgentBackend backend, const char *id,
                                  const ProcSpawnOpts *spawn_opts,
                                  EventBus *bus) {
    Proc *p = proc_spawn(spawn_opts);
    if (!p) return NULL;
    AgentSessionAttachOpts opts = {
        .backend = backend, .id = id, .bus = bus, .proc = p,
    };
    return agent_session_attach(&opts);
}

const char *agent_session_id(const AgentSession *s) {
    return s ? s->id : "";
}

AgentStatus agent_session_status(const AgentSession *s) {
    if (!s) return AC_AGENT_IDLE;
    pthread_mutex_t *mu = (pthread_mutex_t *)&s->mu;
    pthread_mutex_lock(mu);
    AgentStatus st = agent_parser_status(s->parser);
    pthread_mutex_unlock(mu);
    return st;
}

const char *agent_session_current_tool(const AgentSession *s) {
    /* Returns a borrowed pointer that may be invalidated by the reader
     * thread; for the smoke flow we accept this and copy on the UI side. */
    if (!s) return NULL;
    return agent_parser_current_tool(s->parser);
}

int agent_session_send(AgentSession *s, const void *bytes, size_t len) {
    if (!s) return -1;
    return (int)proc_write_stdin(s->proc, bytes, len);
}

int agent_session_close_stdin(AgentSession *s) {
    if (!s) return -1;
    return proc_close_stdin(s->proc);
}

int agent_session_kill(AgentSession *s, int signal) {
    if (!s) return -1;
    return proc_kill(s->proc, signal);
}

int agent_session_wait(AgentSession *s) {
    if (!s) return -1;
    if (s->reader_started) {
        pthread_join(s->reader, NULL);
        s->reader_started = false;
    }
    return proc_wait(s->proc, NULL);
}

void agent_session_free(AgentSession *s) {
    if (!s) return;
    atomic_store(&s->stopping, true);
    if (s->reader_started) {
        /* Closing stdin so the child notices EOF on its input is the
         * benign path; if the child's already gone, this is harmless. */
        proc_close_stdin(s->proc);
        pthread_join(s->reader, NULL);
        s->reader_started = false;
    }
    pthread_mutex_destroy(&s->mu);
    agent_parser_free(s->parser);
    proc_free(s->proc);
    free(s);
}
