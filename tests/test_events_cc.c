#include "greatest.h"
#include "events_cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 64

typedef struct {
    AgentEventKind kinds[MAX_EVENTS];
    char           buf[MAX_EVENTS][256];
    int            count;
} Captured;

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void capture_emit(const AgentEvent *ev, void *user) {
    Captured *c = user;
    if (c->count >= MAX_EVENTS) return;
    c->kinds[c->count] = ev->kind;
    char *summary = c->buf[c->count];
    summary[0] = '\0';
    switch (ev->kind) {
    case AC_EV_SYSTEM_INIT:
        snprintf(summary, 256, "model=%s session=%s",
                 ev->as.system_init.model       ? ev->as.system_init.model       : "?",
                 ev->as.system_init.session_id  ? ev->as.system_init.session_id  : "?");
        break;
    case AC_EV_HOOK:
        snprintf(summary, 256, "%s name=%s",
                 ev->as.hook.is_response ? "hook_response" : "hook_started",
                 ev->as.hook.hook_name ? ev->as.hook.hook_name : "?");
        break;
    case AC_EV_THINKING_DELTA:
        copy_str(summary, 256, ev->as.thinking_delta.text);
        break;
    case AC_EV_TOOL_CALL_STARTED:
        snprintf(summary, 256, "name=%s args=%s",
                 ev->as.tool_call_started.name      ? ev->as.tool_call_started.name      : "?",
                 ev->as.tool_call_started.args_json ? ev->as.tool_call_started.args_json : "?");
        break;
    case AC_EV_TOOL_CALL_FINISHED:
        snprintf(summary, 256, "ok=%d call=%s summary=%.80s",
                 (int)ev->as.tool_call_finished.ok,
                 ev->as.tool_call_finished.call_id ? ev->as.tool_call_finished.call_id : "?",
                 ev->as.tool_call_finished.result_summary ? ev->as.tool_call_finished.result_summary : "?");
        break;
    case AC_EV_RATE_LIMIT:
        snprintf(summary, 256, "window=%s reset=%llu",
                 ev->as.rate_limit.window ? ev->as.rate_limit.window : "?",
                 (unsigned long long)ev->as.rate_limit.reset_at);
        break;
    default: break;
    }
    c->count++;
}

/* Reads a NDJSON file line-by-line and feeds each line to the parser. */
static int feed_ndjson_file(const char *path, Captured *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int total = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n == 0) continue;
        if (line[n - 1] == '\n') { line[n - 1] = '\0'; n--; }
        if (n == 0) continue;
        int emitted = events_cc_parse_one(line, (size_t)n, capture_emit, out);
        if (emitted < 0) { fclose(f); free(line); return -2; }
        total += emitted;
    }
    free(line);
    fclose(f);
    return total;
}

TEST review_commit_fixture_produces_expected_event_sequence(void) {
    Captured c = { .count = 0 };
    int total = feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    ASSERT(total >= 0);

    /* Fixture (truncated capture) contains 7 NDJSON objects:
     *   system/hook_started, system/hook_response, system/init,
     *   assistant(thinking), assistant(tool_use), user(tool_result), rate_limit_event
     * → expected 7 emitted events in this order. */
    ASSERT_EQ(7, c.count);

    ASSERT_EQ(AC_EV_HOOK,                c.kinds[0]);
    ASSERT_EQ(AC_EV_HOOK,                c.kinds[1]);
    ASSERT_EQ(AC_EV_SYSTEM_INIT,         c.kinds[2]);
    ASSERT_EQ(AC_EV_THINKING_DELTA,      c.kinds[3]);
    ASSERT_EQ(AC_EV_TOOL_CALL_STARTED,   c.kinds[4]);
    ASSERT_EQ(AC_EV_TOOL_CALL_FINISHED,  c.kinds[5]);
    ASSERT_EQ(AC_EV_RATE_LIMIT,          c.kinds[6]);
    PASS();
}

TEST system_init_captures_model_and_session(void) {
    Captured c = { .count = 0 };
    feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    ASSERT_STR_EQ("model=claude-opus-4-7[1m] session=7a598311-b7ff-46a4-aa84-ebb37415271f", c.buf[2]);
    PASS();
}

TEST tool_call_started_captures_name_and_args(void) {
    Captured c = { .count = 0 };
    feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    /* assistant tool_use was Skill with input { "skill": "mr" } */
    ASSERT(strstr(c.buf[4], "name=Skill") != NULL);
    ASSERT(strstr(c.buf[4], "\"skill\":\"mr\"") != NULL);
    PASS();
}

TEST tool_call_finished_marks_error_when_is_error_true(void) {
    Captured c = { .count = 0 };
    feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    /* user/tool_result had is_error: true (Skill denied in dontAsk mode). */
    ASSERT(strstr(c.buf[5], "ok=0") != NULL);
    ASSERT(strstr(c.buf[5], "call=toolu_012Y79s23oYg2BrHPveZpEzp") != NULL);
    PASS();
}

TEST thinking_signature_is_stripped(void) {
    Captured c = { .count = 0 };
    feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    /* The fixture has a thinking block with empty `thinking` string and a long
     * `signature`. We should never see signature contents in the emitted text. */
    ASSERT(strstr(c.buf[3], "EqgUClkIDR") == NULL); /* prefix of the signature */
    PASS();
}

TEST rate_limit_window_is_five_hour(void) {
    Captured c = { .count = 0 };
    feed_ndjson_file("tests/fixtures/cc/review_commit.ndjson", &c);
    ASSERT(strstr(c.buf[6], "window=five_hour") != NULL);
    PASS();
}

SUITE(events_cc) {
    RUN_TEST(review_commit_fixture_produces_expected_event_sequence);
    RUN_TEST(system_init_captures_model_and_session);
    RUN_TEST(tool_call_started_captures_name_and_args);
    RUN_TEST(tool_call_finished_marks_error_when_is_error_true);
    RUN_TEST(thinking_signature_is_stripped);
    RUN_TEST(rate_limit_window_is_five_hour);
}
