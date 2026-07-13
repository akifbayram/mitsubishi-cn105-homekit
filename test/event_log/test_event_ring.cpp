// Host-side tests for the persisted device-event ring (overwrite-oldest).
// Compiled and run by run.sh — no hardware or ESP-IDF needed.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "event_ring.h"

static int g_checks = 0;
#define CHECK(c) do { assert(c); g_checks++; } while (0)

static EventEntry ev(uint32_t bootN, uint8_t type, uint8_t code) {
    EventEntry e{};
    e.bootN = bootN;
    e.uptimeS = bootN * 10;
    e.type = type;
    e.code = code;
    return e;
}

// ── Fresh blob is valid and empty ────────────────────────────────────────────
static void test_reset(void) {
    EventBlob b;
    memset(&b, 0xAB, sizeof(b));      // garbage in
    eventblob_reset(b);
    CHECK(eventblob_valid(b));
    CHECK(b.count == 0);
    CHECK(eventblob_newest(b, 0) == nullptr);
}

// ── Append below capacity keeps all, newest-first iteration ─────────────────
static void test_append_order(void) {
    EventBlob b;
    eventblob_reset(b);
    for (uint32_t i = 0; i < 5; i++) eventblob_append(b, ev(i, 1, (uint8_t)i));
    CHECK(b.count == 5);
    for (size_t i = 0; i < 5; i++) {
        const EventEntry *e = eventblob_newest(b, i);
        CHECK(e != nullptr);
        CHECK(e->bootN == 4 - i);     // i=0 → newest (bootN 4)
    }
    CHECK(eventblob_newest(b, 5) == nullptr);
}

// ── Overflow overwrites the OLDEST entries ──────────────────────────────────
static void test_overflow(void) {
    EventBlob b;
    eventblob_reset(b);
    const uint32_t total = (uint32_t)EVENTLOG_CAP + 7;
    for (uint32_t i = 0; i < total; i++) eventblob_append(b, ev(i, 2, 0));
    CHECK(b.count == EVENTLOG_CAP);
    // Newest is the last appended; oldest surviving is total - CAP.
    CHECK(eventblob_newest(b, 0)->bootN == total - 1);
    CHECK(eventblob_newest(b, EVENTLOG_CAP - 1)->bootN == total - EVENTLOG_CAP);
    // Order stays strictly descending with no gaps.
    for (size_t i = 0; i + 1 < EVENTLOG_CAP; i++) {
        CHECK(eventblob_newest(b, i)->bootN == eventblob_newest(b, i + 1)->bootN + 1);
    }
}

// ── Loaded-blob validation rejects corrupt headers ───────────────────────────
static void test_validation(void) {
    EventBlob b;
    eventblob_reset(b);
    eventblob_append(b, ev(1, 1, 0));
    CHECK(eventblob_valid(b));

    EventBlob bad = b; bad.ver = EVENTBLOB_VERSION + 1;
    CHECK(!eventblob_valid(bad));
    bad = b; bad.count = EVENTLOG_CAP + 1;
    CHECK(!eventblob_valid(bad));
    bad = b; bad.head = EVENTLOG_CAP;
    CHECK(!eventblob_valid(bad));
}

// ── Fields survive the ring intact (persist-format sanity) ──────────────────
static void test_field_integrity(void) {
    EventBlob b;
    eventblob_reset(b);
    EventEntry e{};
    e.bootN = 0xDEADBEEF;
    e.uptimeS = 12345;
    e.epoch = 1780000000;
    e.type = 9;
    e.code = 0x80;
    eventblob_append(b, e);
    const EventEntry *r = eventblob_newest(b, 0);
    CHECK(r->bootN == 0xDEADBEEF && r->uptimeS == 12345 &&
          r->epoch == 1780000000 && r->type == 9 && r->code == 0x80);
}

int main(void) {
    test_reset();
    test_append_order();
    test_overflow();
    test_validation();
    test_field_integrity();
    printf("event_ring: all %d checks passed\n", g_checks);
    return 0;
}
