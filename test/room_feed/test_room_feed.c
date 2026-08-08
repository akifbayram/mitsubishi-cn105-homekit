/* Host tests for the room-temperature feed state machine — the change/keepalive/
 * stale logic both sources share. Split out of link_sensor.cpp so it is
 * provable without a CN105, a radio, or FreeRTOS. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "room_feed.h"

#define STALE_MS 600000u

static void test_first_reading_sends(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
}

static void test_unchanged_reading_holds_until_keepalive(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* same value, 10s later: nothing to say */
    assert(room_feed_step(&f, true, true, 220, 0, 11000, STALE_MS) == ROOM_FEED_NONE);
    /* 20s keepalive elapsed */
    assert(room_feed_step(&f, true, true, 220, 0, 21001, STALE_MS) == ROOM_FEED_SEND);
}

static void test_change_resends_early(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* 0.5C jump 6s later — past the min resend gap */
    assert(room_feed_step(&f, true, true, 225, 0, 7000, STALE_MS) == ROOM_FEED_SEND);
}

static void test_change_respects_min_resend_gap(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* same jump 1s later — too soon, do not spam the CN105 */
    assert(room_feed_step(&f, true, true, 225, 0, 2000, STALE_MS) == ROOM_FEED_NONE);
}

static void test_stale_clears_once(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* reading is now older than the stale window */
    assert(room_feed_step(&f, true, true, 220, STALE_MS + 1, 700000, STALE_MS) == ROOM_FEED_CLEAR);
    /* ...and does not clear again every pass */
    assert(room_feed_step(&f, true, true, 220, STALE_MS + 2, 701000, STALE_MS) == ROOM_FEED_NONE);
}

static void test_recovery_after_stale_sends_again(void) {
    room_feed_t f; room_feed_init(&f);
    room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS);
    assert(room_feed_step(&f, true, true, 220, STALE_MS + 1, 700000, STALE_MS) == ROOM_FEED_CLEAR);
    /* fresh reading arrives */
    assert(room_feed_step(&f, true, true, 221, 0, 701000, STALE_MS) == ROOM_FEED_SEND);
}

static void test_deselect_clears_once(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    assert(room_feed_step(&f, false, true, 220, 0, 2000, STALE_MS) == ROOM_FEED_CLEAR);
    assert(room_feed_step(&f, false, true, 220, 0, 3000, STALE_MS) == ROOM_FEED_NONE);
}

static void test_never_selected_never_clears(void) {
    /* A source that was never feeding must not issue a clear — that would
     * stomp on whichever source IS feeding. */
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, false, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_NONE);
    assert(room_feed_step(&f, false, true, 220, 0, 2000, STALE_MS) == ROOM_FEED_NONE);
}

static void test_no_reading_while_selected_is_not_a_send(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, false, 0, 0, 1000, STALE_MS) == ROOM_FEED_NONE);
}

/* have_reading going false is a distinct path from deselect (selected=false)
 * and from stale (have_reading=true but age>=stale_ms) — pin it separately so
 * a change that only fixes two of the three OR'd conditions in room_feed_step
 * doesn't slip through. */
static void test_reading_lost_clears_once(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    assert(room_feed_step(&f, true, false, 0, 0, 2000, STALE_MS) == ROOM_FEED_CLEAR);
    assert(room_feed_step(&f, true, false, 0, 0, 3000, STALE_MS) == ROOM_FEED_NONE);
}

/* Boundary tests: every other case above lands strictly past its threshold
 * (+1ms or more), which would not notice a >= -> > mutation. Land exactly on
 * the threshold instead. */
static void test_stale_boundary_exact(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* age_ms == stale_ms exactly: still stale (>=), not "just under" */
    assert(room_feed_step(&f, true, true, 220, STALE_MS, 700000, STALE_MS) == ROOM_FEED_CLEAR);
}

static void test_keepalive_boundary_exact(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* unchanged value, since == keepalive exactly: still due (>=) */
    assert(room_feed_step(&f, true, true, 220, 0, 1000 + ROOM_FEED_KEEPALIVE_MS, STALE_MS) == ROOM_FEED_SEND);
}

static void test_resend_min_gap_boundary_exact(void) {
    room_feed_t f; room_feed_init(&f);
    assert(room_feed_step(&f, true, true, 220, 0, 1000, STALE_MS) == ROOM_FEED_SEND);
    /* changed value, since == resend-min exactly: allowed (>=), not blocked */
    assert(room_feed_step(&f, true, true, 225, 0, 1000 + ROOM_FEED_RESEND_MIN_MS, STALE_MS) == ROOM_FEED_SEND);
}

int main(void) {
    test_first_reading_sends();
    test_unchanged_reading_holds_until_keepalive();
    test_change_resends_early();
    test_change_respects_min_resend_gap();
    test_stale_clears_once();
    test_recovery_after_stale_sends_again();
    test_deselect_clears_once();
    test_never_selected_never_clears();
    test_no_reading_while_selected_is_not_a_send();
    test_reading_lost_clears_once();
    test_stale_boundary_exact();
    test_keepalive_boundary_exact();
    test_resend_min_gap_boundary_exact();
    printf("room_feed: all tests passed\n");
    return 0;
}
