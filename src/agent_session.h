#ifndef AC_AGENT_SESSION_H
#define AC_AGENT_SESSION_H

#include "agent.h"
#include "bus.h"
#include "proc.h"

#include <stdbool.h>

typedef struct AgentSession AgentSession;

typedef struct {
    AgentBackend backend;
    const char  *id;             /* borrowed; copied into the session */
    EventBus    *bus;            /* optional sink for LogEntry */
    Proc        *proc;           /* TAKES OWNERSHIP — freed by agent_session_free */
    AgentEventEmit on_event;     /* optional callback fired from reader thread */
    void          *user;
} AgentSessionAttachOpts;

/* Wrap an already-spawned (or attach()'d) Proc into a running session.
 * Spawns the reader thread. Returns NULL on failure (frees proc on failure). */
AgentSession *agent_session_attach(const AgentSessionAttachOpts *opts);

/* Convenience: spawn a child via proc_spawn and attach. Equivalent to
 * proc_spawn() + agent_session_attach(). */
AgentSession *agent_session_spawn(AgentBackend backend, const char *id,
                                  const ProcSpawnOpts *spawn_opts,
                                  EventBus *bus);

const char  *agent_session_id(const AgentSession *s);
AgentStatus  agent_session_status(const AgentSession *s);
const char  *agent_session_current_tool(const AgentSession *s);

int  agent_session_send(AgentSession *s, const void *bytes, size_t len);
int  agent_session_close_stdin(AgentSession *s);

/* Wait for the reader thread to finish (i.e. child closed its stdout). */
int  agent_session_wait(AgentSession *s);

int  agent_session_kill(AgentSession *s, int signal);
void agent_session_free(AgentSession *s);

#endif
