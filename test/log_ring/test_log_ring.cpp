// Host-side tests for the log line ring (drop-oldest, fixed capacity).
// Compiled and run by run.sh — no hardware or ESP-IDF needed.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "log_ring.h"

static int g_checks = 0;
#define CHECK(c) do { assert(c); g_checks++; } while (0)

static void push_str(LogRing &r, const char *s) { r.push(s, strlen(s)); }

// ── Empty ring ───────────────────────────────────────────────────────────────
static void test_empty(void) {
    LogRing r;
    char out[64];
    CHECK(r.count() == 0);
    CHECK(r.pop(out, sizeof(out)) == 0);
    CHECK(r.dropped() == 0);
}

// ── Single push/pop roundtrip preserves content and length ──────────────────
static void test_roundtrip(void) {
    LogRing r;
    char out[64];
    push_str(r, "hello");
    CHECK(r.count() == 1);
    CHECK(r.pop(out, sizeof(out)) == 5);
    CHECK(strcmp(out, "hello") == 0);
    CHECK(r.count() == 0);
    CHECK(r.pop(out, sizeof(out)) == 0);
}

// ── FIFO order ───────────────────────────────────────────────────────────────
static void test_fifo(void) {
    LogRing r;
    char out[64];
    push_str(r, "one");
    push_str(r, "two");
    push_str(r, "three");
    CHECK(r.pop(out, sizeof(out)) == 3 && strcmp(out, "one") == 0);
    CHECK(r.pop(out, sizeof(out)) == 3 && strcmp(out, "two") == 0);
    CHECK(r.pop(out, sizeof(out)) == 5 && strcmp(out, "three") == 0);
}

// ── Push truncates at MAX_LINE-1 and still NUL-terminates ───────────────────
static void test_push_truncation(void) {
    LogRing r;
    char big[LogRing::MAX_LINE + 50];
    memset(big, 'x', sizeof(big));
    r.push(big, sizeof(big));
    char out[LogRing::MAX_LINE + 50];
    size_t len = r.pop(out, sizeof(out));
    CHECK(len == LogRing::MAX_LINE - 1);
    CHECK(out[len] == '\0');
    CHECK(strlen(out) == LogRing::MAX_LINE - 1);
}

// ── Pop into a smaller buffer truncates safely ───────────────────────────────
static void test_pop_truncation(void) {
    LogRing r;
    char out[4];
    push_str(r, "truncate-me");
    CHECK(r.pop(out, sizeof(out)) == 3);
    CHECK(strcmp(out, "tru") == 0);
    // cap == 0 pops nothing and does not consume
    push_str(r, "kept");
    CHECK(r.pop(out, 0) == 0);
    CHECK(r.count() == 1);
}

// ── Overflow evicts the OLDEST line and counts drops ────────────────────────
static void test_overflow_drops_oldest(void) {
    LogRing r;
    char name[16], out[64];
    for (size_t i = 0; i < LogRing::CAPACITY + 3; i++) {
        snprintf(name, sizeof(name), "line%02zu", i);
        push_str(r, name);
    }
    CHECK(r.count() == LogRing::CAPACITY);
    CHECK(r.dropped() == 3);
    // Oldest surviving line is line03 (00..02 were evicted)
    CHECK(r.pop(out, sizeof(out)) > 0 && strcmp(out, "line03") == 0);
    // Newest line survived
    while (r.count() > 1) r.pop(out, sizeof(out));
    snprintf(name, sizeof(name), "line%02zu", LogRing::CAPACITY + 2);
    CHECK(r.pop(out, sizeof(out)) > 0 && strcmp(out, name) == 0);
}

// ── Wraparound: interleaved push/pop cycles keep order and content ──────────
static void test_wraparound(void) {
    LogRing r;
    char name[16], out[64];
    // Pop after every 2nd push: net growth stays below CAPACITY (no drops)
    // while tail/head wrap the array several times.
    size_t popped = 0, pushed = 0;
    for (size_t i = 0; i < LogRing::CAPACITY * 2 - 4; i++) {
        snprintf(name, sizeof(name), "w%03zu", i);
        push_str(r, name);
        pushed++;
        if (i % 2 == 1) {
            CHECK(r.pop(out, sizeof(out)) > 0);
            snprintf(name, sizeof(name), "w%03zu", popped);
            CHECK(strcmp(out, name) == 0);
            popped++;
        }
    }
    CHECK(r.dropped() == 0);
    CHECK(r.count() == pushed - popped);
}

// ── Drop counter is cumulative across separate overflows ────────────────────
static void test_dropped_cumulative(void) {
    LogRing r;
    char out[64];
    for (size_t i = 0; i < LogRing::CAPACITY + 2; i++) push_str(r, "a");
    CHECK(r.dropped() == 2);
    while (r.pop(out, sizeof(out)) > 0) {}
    for (size_t i = 0; i < LogRing::CAPACITY + 5; i++) push_str(r, "b");
    CHECK(r.dropped() == 7);
}

int main(void) {
    test_empty();
    test_roundtrip();
    test_fifo();
    test_push_truncation();
    test_pop_truncation();
    test_overflow_drops_oldest();
    test_wraparound();
    test_dropped_cumulative();
    printf("log_ring: all %d checks passed\n", g_checks);
    return 0;
}
