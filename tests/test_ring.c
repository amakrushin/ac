#include "greatest.h"
#include "ring.h"

TEST ring_create_destroys_cleanly(void) {
    Ring *r = ring_create(8, sizeof(int));
    ASSERT(r != NULL);
    ASSERT_EQ(8u, ring_capacity(r));
    ASSERT_EQ(0u, ring_size(r));
    ASSERT(ring_empty(r));
    ASSERT_FALSE(ring_full(r));
    ring_destroy(r);
    PASS();
}

TEST ring_create_rejects_zero_args(void) {
    ASSERT(ring_create(0, sizeof(int)) == NULL);
    ASSERT(ring_create(8, 0)           == NULL);
    PASS();
}

TEST ring_push_under_capacity(void) {
    Ring *r = ring_create(4, sizeof(int));
    int v;
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ((uint64_t)(i + 1), ring_push(r, &i));
    }
    ASSERT_EQ(3u, ring_size(r));
    ASSERT_FALSE(ring_full(r));
    for (int i = 0; i < 3; i++) {
        ASSERT(ring_get(r, (size_t)i, &v));
        ASSERT_EQ(i, v);
    }
    ring_destroy(r);
    PASS();
}

TEST ring_overwrites_when_full(void) {
    Ring *r = ring_create(3, sizeof(int));
    for (int i = 0; i < 5; i++) ring_push(r, &i);

    ASSERT_EQ(3u, ring_size(r));
    ASSERT(ring_full(r));

    /* After pushing 0,1,2,3,4 into capacity-3 ring, expect [2,3,4]. */
    int v;
    int expected[] = {2, 3, 4};
    for (size_t i = 0; i < 3; i++) {
        ASSERT(ring_get(r, i, &v));
        ASSERT_EQ(expected[i], v);
    }
    ring_destroy(r);
    PASS();
}

TEST ring_serials_advance_monotonically(void) {
    Ring *r = ring_create(3, sizeof(int));
    for (int i = 0; i < 5; i++) ring_push(r, &i);

    /* 5 pushes total; ring holds last 3 (serials 3,4,5). */
    ASSERT_EQ(3u, ring_oldest_serial(r));
    ASSERT_EQ(5u, ring_newest_serial(r));

    int v;
    ASSERT(ring_get_by_serial(r, 3, &v));
    ASSERT_EQ(2, v);
    ASSERT(ring_get_by_serial(r, 5, &v));
    ASSERT_EQ(4, v);

    ASSERT_FALSE(ring_get_by_serial(r, 2, &v)); /* evicted */
    ASSERT_FALSE(ring_get_by_serial(r, 6, &v)); /* not yet pushed */

    ring_destroy(r);
    PASS();
}

TEST ring_get_out_of_range(void) {
    Ring *r = ring_create(3, sizeof(int));
    int v;
    ASSERT_FALSE(ring_get(r, 0, &v)); /* empty */
    int x = 42;
    ring_push(r, &x);
    ASSERT(ring_get(r, 0, &v));
    ASSERT_EQ(42, v);
    ASSERT_FALSE(ring_get(r, 1, &v));
    ring_destroy(r);
    PASS();
}

SUITE(ring) {
    RUN_TEST(ring_create_destroys_cleanly);
    RUN_TEST(ring_create_rejects_zero_args);
    RUN_TEST(ring_push_under_capacity);
    RUN_TEST(ring_overwrites_when_full);
    RUN_TEST(ring_serials_advance_monotonically);
    RUN_TEST(ring_get_out_of_range);
}
