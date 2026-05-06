#include "greatest.h"
#include "bus.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_entry(LogEntry *e, const char *agent, const char *oneline, int i) {
    memset(e, 0, sizeof(*e));
    e->timestamp_ms = 1000000 + i;
    snprintf(e->agent_id, sizeof(e->agent_id), "%s", agent);
    e->kind = AC_LOG_TOOL_CALL;
    snprintf(e->oneline, sizeof(e->oneline), "%s", oneline);
}

TEST bus_basic_push_and_get(void) {
    EventBus *b = bus_create(8);
    ASSERT(b != NULL);
    ASSERT_EQ(8u, bus_capacity(b));
    ASSERT_EQ(0u, bus_size(b));

    LogEntry e;
    make_entry(&e, "cc-1", "ripgrep TODO", 1);
    uint64_t s1 = bus_push(b, &e);
    ASSERT_EQ(1u, s1);

    make_entry(&e, "codex-2", "Read main.c", 2);
    uint64_t s2 = bus_push(b, &e);
    ASSERT_EQ(2u, s2);

    ASSERT_EQ(2u, bus_size(b));
    ASSERT_EQ(1u, bus_oldest_serial(b));
    ASSERT_EQ(2u, bus_newest_serial(b));

    LogEntry out;
    ASSERT(bus_get(b, 0, &out));
    ASSERT_EQ(1u, out.serial);
    ASSERT_STR_EQ("cc-1", out.agent_id);
    ASSERT_STR_EQ("ripgrep TODO", out.oneline);

    ASSERT(bus_get_by_serial(b, 2, &out));
    ASSERT_STR_EQ("codex-2", out.agent_id);
    ASSERT_STR_EQ("Read main.c", out.oneline);

    bus_free(b);
    PASS();
}

TEST bus_overwrite_oldest_when_full(void) {
    EventBus *b = bus_create(3);
    LogEntry e;
    for (int i = 1; i <= 5; i++) {
        char line[32];
        snprintf(line, sizeof(line), "entry-%d", i);
        make_entry(&e, "cc-1", line, i);
        bus_push(b, &e);
    }
    ASSERT_EQ(3u, bus_size(b));
    ASSERT_EQ(3u, bus_oldest_serial(b));
    ASSERT_EQ(5u, bus_newest_serial(b));

    LogEntry out;
    ASSERT(bus_get(b, 0, &out));
    ASSERT_STR_EQ("entry-3", out.oneline);
    ASSERT(bus_get(b, 2, &out));
    ASSERT_STR_EQ("entry-5", out.oneline);

    /* Old serials evicted. */
    ASSERT_FALSE(bus_get_by_serial(b, 1, &out));
    ASSERT_FALSE(bus_get_by_serial(b, 2, &out));

    bus_free(b);
    PASS();
}

#define WRITERS  4
#define PER_WRITER 250

typedef struct {
    EventBus *bus;
    int       writer_id;
} WriterArg;

static void *writer_thread(void *arg_) {
    WriterArg *arg = arg_;
    LogEntry e;
    char agent[16];
    snprintf(agent, sizeof(agent), "w-%d", arg->writer_id);
    for (int i = 0; i < PER_WRITER; i++) {
        char line[32];
        snprintf(line, sizeof(line), "%d:%d", arg->writer_id, i);
        make_entry(&e, agent, line, i);
        bus_push(arg->bus, &e);
    }
    return NULL;
}

TEST bus_concurrent_pushes_no_torn_entries(void) {
    /* Capacity larger than total pushes so nothing is evicted. */
    EventBus *b = bus_create(WRITERS * PER_WRITER + 16);
    pthread_t threads[WRITERS];
    WriterArg args[WRITERS];
    for (int i = 0; i < WRITERS; i++) {
        args[i].bus = b;
        args[i].writer_id = i;
        pthread_create(&threads[i], NULL, writer_thread, &args[i]);
    }
    for (int i = 0; i < WRITERS; i++) pthread_join(threads[i], NULL);

    ASSERT_EQ((size_t)(WRITERS * PER_WRITER), bus_size(b));
    ASSERT_EQ(1u, bus_oldest_serial(b));
    ASSERT_EQ((uint64_t)(WRITERS * PER_WRITER), bus_newest_serial(b));

    /* Sweep: every entry must have a well-formed agent_id ("w-N") whose N
     * matches the integer parse of its oneline (which is "N:i"). If any
     * memcpy was torn under the mutex, this would catch it. */
    int torn = 0;
    for (size_t off = 0; off < bus_size(b); off++) {
        LogEntry out;
        if (!bus_get(b, off, &out)) { torn++; continue; }
        int agent_n = -1;
        sscanf(out.agent_id, "w-%d", &agent_n);
        int line_n = -1;
        sscanf(out.oneline, "%d:", &line_n);
        if (agent_n < 0 || line_n < 0 || agent_n != line_n) torn++;
    }
    ASSERT_EQ(0, torn);

    /* Histogram: each writer contributed exactly PER_WRITER entries. */
    int hist[WRITERS] = {0};
    for (size_t off = 0; off < bus_size(b); off++) {
        LogEntry out;
        bus_get(b, off, &out);
        int agent_n = -1;
        sscanf(out.agent_id, "w-%d", &agent_n);
        if (agent_n >= 0 && agent_n < WRITERS) hist[agent_n]++;
    }
    for (int i = 0; i < WRITERS; i++) ASSERT_EQ(PER_WRITER, hist[i]);

    bus_free(b);
    PASS();
}

SUITE(bus) {
    RUN_TEST(bus_basic_push_and_get);
    RUN_TEST(bus_overwrite_oldest_when_full);
    RUN_TEST(bus_concurrent_pushes_no_torn_entries);
}
