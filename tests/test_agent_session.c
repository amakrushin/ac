#include "greatest.h"
#include "agent_session.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Drives an AgentSession with a fake Proc whose "stdout" is a pipe we
 * write canned NDJSON into from the test thread. */
TEST agent_session_drives_through_fixture_into_bus(void) {
    int pipe_to_session[2];   /* parent writes "child stdout" here */
    int pipe_from_session[2]; /* "child stdin" — unused */
    ASSERT_EQ(0, pipe(pipe_to_session));
    ASSERT_EQ(0, pipe(pipe_from_session));

    /* Proc reads its "stdout" from pipe_to_session[0]; writes its "stdin" to
     * pipe_from_session[1]. */
    Proc *p = proc_attach(pipe_from_session[1], pipe_to_session[0]);
    ASSERT(p != NULL);

    EventBus *bus = bus_create(64);
    AgentSessionAttachOpts opts = {
        .backend = AC_BACKEND_CC,
        .id      = "cc-1",
        .bus     = bus,
        .proc    = p,
    };
    AgentSession *s = agent_session_attach(&opts);
    ASSERT(s != NULL);

    /* Slurp the fixture and write it to the pipe (simulating cc's stdout). */
    FILE *f = fopen("tests/fixtures/cc/review_commit.ndjson", "rb");
    ASSERT(f != NULL);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ssize_t w = write(pipe_to_session[1], buf, n);
        ASSERT((size_t)w == n);
    }
    fclose(f);

    /* Close write end → child sees EOF → reader thread exits. */
    close(pipe_to_session[1]);
    close(pipe_from_session[0]); /* unused parent end */

    /* Wait for reader thread. */
    agent_session_wait(s);

    /* Bus should have: hook×2, system_init, tool_use, tool_result, rate_limit
     * = 6 LogEntries (thinking_delta is skipped because the fixture's text was empty;
     * AssistantTextDelta and TurnDone weren't in the fixture). */
    ASSERT_EQ(6u, bus_size(bus));

    LogEntry e;
    ASSERT(bus_get(bus, 0, &e));
    ASSERT_EQ(AC_LOG_HOOK, e.kind);
    ASSERT_STR_EQ("cc-1", e.agent_id);

    ASSERT(bus_get(bus, 2, &e));
    ASSERT_EQ(AC_LOG_STATUS, e.kind);
    ASSERT(strstr(e.oneline, "init model=") != NULL);

    ASSERT(bus_get(bus, 3, &e));
    ASSERT_EQ(AC_LOG_TOOL_CALL, e.kind);
    ASSERT(strstr(e.oneline, "Skill") != NULL);

    ASSERT(bus_get(bus, 4, &e));
    ASSERT_EQ(AC_LOG_TOOL_CALL, e.kind);
    ASSERT(strstr(e.oneline, "✗") != NULL); /* is_error=true */

    ASSERT(bus_get(bus, 5, &e));
    ASSERT_EQ(AC_LOG_RATE_LIMIT, e.kind);
    ASSERT(strstr(e.oneline, "five_hour") != NULL);

    /* After full fixture: thinking → tool → streaming. */
    ASSERT_EQ(AC_AGENT_STREAMING, agent_session_status(s));

    agent_session_free(s);
    bus_free(bus);
    PASS();
}

SUITE(agent_session) {
    RUN_TEST(agent_session_drives_through_fixture_into_bus);
}
