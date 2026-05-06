#include "greatest.h"
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KINDS 64

typedef struct {
    AgentEventKind kinds[MAX_KINDS];
    int            count;
} KindLog;

static void emit_kind(const AgentEvent *ev, void *user) {
    KindLog *log = user;
    if (log->count < MAX_KINDS) log->kinds[log->count++] = ev->kind;
}

/* Read fixture into a heap buffer; caller frees. Sets *len_out. */
static char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

TEST agent_parser_full_fixture_one_feed(void) {
    AgentParser *p = agent_parser_create(AC_BACKEND_CC);
    ASSERT(p != NULL);
    ASSERT_EQ(AC_AGENT_IDLE, agent_parser_status(p));

    size_t len = 0;
    char *bytes = slurp("tests/fixtures/cc/review_commit.ndjson", &len);
    ASSERT(bytes != NULL);

    KindLog log = { .count = 0 };
    int n = agent_parser_feed(p, bytes, len, emit_kind, &log);
    ASSERT_EQ(7, n);
    ASSERT_EQ(7, log.count);

    free(bytes);
    agent_parser_free(p);
    PASS();
}

TEST agent_parser_status_transitions_through_fixture(void) {
    AgentParser *p = agent_parser_create(AC_BACKEND_CC);
    size_t len = 0;
    char *bytes = slurp("tests/fixtures/cc/review_commit.ndjson", &len);
    ASSERT(bytes != NULL);

    /* Feed line by line and snapshot status after each. */
    AgentStatus snap[8];
    int snap_count = 0;
    size_t cursor = 0;
    while (cursor < len && snap_count < 8) {
        char *nl = memchr(bytes + cursor, '\n', len - cursor);
        size_t line_len = nl ? (size_t)(nl - (bytes + cursor)) : (len - cursor);
        agent_parser_feed(p, bytes + cursor, line_len, NULL, NULL);
        agent_parser_feed(p, "\n", 1, NULL, NULL);
        snap[snap_count++] = agent_parser_status(p);
        cursor += line_len + (nl ? 1 : 0);
    }

    /* Sequence: hook_started, hook_response, system_init, thinking, tool_use, tool_result, rate_limit
     * Status snapshots (after each feed): IDLE,IDLE,IDLE,THINKING,TOOL,STREAMING,STREAMING */
    ASSERT_EQ(AC_AGENT_IDLE,      snap[0]);
    ASSERT_EQ(AC_AGENT_IDLE,      snap[1]);
    ASSERT_EQ(AC_AGENT_IDLE,      snap[2]);
    ASSERT_EQ(AC_AGENT_THINKING,  snap[3]);
    ASSERT_EQ(AC_AGENT_TOOL,      snap[4]);
    ASSERT_EQ(AC_AGENT_STREAMING, snap[5]);
    ASSERT_EQ(AC_AGENT_STREAMING, snap[6]);

    free(bytes);
    agent_parser_free(p);
    PASS();
}

TEST agent_parser_remembers_current_tool_name(void) {
    AgentParser *p = agent_parser_create(AC_BACKEND_CC);
    size_t len = 0;
    char *bytes = slurp("tests/fixtures/cc/review_commit.ndjson", &len);

    /* Feed up through and including the tool_use line (5th line). */
    size_t cursor = 0;
    int line_idx = 0;
    while (cursor < len && line_idx < 5) {
        char *nl = memchr(bytes + cursor, '\n', len - cursor);
        size_t line_len = nl ? (size_t)(nl - (bytes + cursor)) + 1 : (len - cursor);
        agent_parser_feed(p, bytes + cursor, line_len, NULL, NULL);
        cursor += line_len;
        line_idx++;
    }
    ASSERT_EQ(AC_AGENT_TOOL, agent_parser_status(p));
    ASSERT_STR_EQ("Skill", agent_parser_current_tool(p));

    free(bytes);
    agent_parser_free(p);
    PASS();
}

TEST agent_parser_handles_byte_split_at_arbitrary_boundary(void) {
    AgentParser *p = agent_parser_create(AC_BACKEND_CC);
    size_t len = 0;
    char *bytes = slurp("tests/fixtures/cc/review_commit.ndjson", &len);

    /* Drip-feed in 17-byte chunks (deliberately not aligned to lines). */
    KindLog log = { .count = 0 };
    size_t cursor = 0;
    int parse_errors = 0;
    while (cursor < len) {
        size_t chunk = len - cursor < 17 ? len - cursor : 17;
        int n = agent_parser_feed(p, bytes + cursor, chunk, emit_kind, &log);
        if (n < 0) parse_errors++;
        cursor += chunk;
    }
    ASSERT_EQ(0, parse_errors);
    ASSERT_EQ(7, log.count);

    free(bytes);
    agent_parser_free(p);
    PASS();
}

TEST agent_parser_drops_unparseable_line_and_recovers(void) {
    AgentParser *p = agent_parser_create(AC_BACKEND_CC);
    KindLog log = { .count = 0 };

    const char *good_init    = "{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"x\",\"session_id\":\"s\"}\n";
    const char *garbage      = "this is not json\n";
    const char *good_done    = "{\"type\":\"result\",\"subtype\":\"success\"}\n";

    int n1 = agent_parser_feed(p, good_init,  strlen(good_init),  emit_kind, &log);
    int n2 = agent_parser_feed(p, garbage,    strlen(garbage),    emit_kind, &log);
    int n3 = agent_parser_feed(p, good_done,  strlen(good_done),  emit_kind, &log);

    ASSERT_EQ(1, n1);
    ASSERT(n2 < 0);   /* parse failed for the garbage line */
    ASSERT_EQ(1, n3);
    ASSERT_EQ(2, log.count);
    ASSERT_EQ(AC_EV_SYSTEM_INIT, log.kinds[0]);
    ASSERT_EQ(AC_EV_TURN_DONE,   log.kinds[1]);
    ASSERT_EQ(AC_AGENT_IDLE, agent_parser_status(p));

    agent_parser_free(p);
    PASS();
}

SUITE(agent) {
    RUN_TEST(agent_parser_full_fixture_one_feed);
    RUN_TEST(agent_parser_status_transitions_through_fixture);
    RUN_TEST(agent_parser_remembers_current_tool_name);
    RUN_TEST(agent_parser_handles_byte_split_at_arbitrary_boundary);
    RUN_TEST(agent_parser_drops_unparseable_line_and_recovers);
}
