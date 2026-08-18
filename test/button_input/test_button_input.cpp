// Host tests for the pure button mechanics (button_input.h).
#include "button_input.h"
#include <cassert>
#include <cstdio>

// Drive the input for `ms` milliseconds at a 10 ms poll (the real main-loop
// cadence), returning the last non-NONE event seen.
static ButtonOut drive(ButtonInput& b, bool pressed, uint32_t& t, uint32_t ms) {
    ButtonOut last = {BTN_EV_NONE, 0, 0, false};
    for (uint32_t i = 0; i < ms; i += 10) {
        ButtonOut o = b.update(pressed, t);
        if (o.ev != BTN_EV_NONE) last = o;
        t += 10;
    }
    return last;
}

int main() {
    // A clean single click settles as clicks == 1
    { ButtonInput b; uint32_t t = 1000;
      drive(b, true, t, 100);
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_CLICK); assert(o.clicks == 1); }

    // Three clicks inside the burst gap settle as clicks == 3
    { ButtonInput b; uint32_t t = 1000;
      for (int i = 0; i < 3; i++) { drive(b, true, t, 100); drive(b, false, t, 150); }
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_CLICK); assert(o.clicks == 3); }

    // Two clicks then a pause settle as clicks == 2, not 3
    { ButtonInput b; uint32_t t = 1000;
      for (int i = 0; i < 2; i++) { drive(b, true, t, 100); drive(b, false, t, 150); }
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_CLICK); assert(o.clicks == 2); }

    // Contact bounce during a single press still yields exactly one click
    { ButtonInput b; uint32_t t = 1000;
      b.update(true,  t); t += 5;
      b.update(false, t); t += 5;
      b.update(true,  t); t += 5;
      b.update(false, t); t += 5;
      b.update(true,  t); t += 5;
      drive(b, true, t, 100);
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_CLICK); assert(o.clicks == 1); }

    // A long press yields RELEASE carrying its duration, never a click
    { ButtonInput b; uint32_t t = 1000;
      drive(b, true, t, 3000);
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_RELEASE);
      assert(o.heldMs >= 2900 && o.heldMs <= 3010); }

    // heldMs tracks the live press and reads 0 once released
    { ButtonInput b; uint32_t t = 1000;
      drive(b, true, t, 1000);
      ButtonOut mid = b.update(true, t);
      assert(mid.pressed); assert(mid.heldMs >= 950);
      drive(b, false, t, 600);
      ButtonOut up = b.update(false, t);
      assert(!up.pressed); assert(up.heldMs == 0); }

    // A starved poll (main loop stalled) abandons the burst instead of
    // misreading the gap as more clicks
    { ButtonInput b; uint32_t t = 1000;
      drive(b, true, t, 100); drive(b, false, t, 100);
      t += 5000;                                  // stall
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_NONE); }

    // Multi-click burst straddling rollover catches premature settle (signed-diff comparison)
    { ButtonInput b; const uint32_t S = 0xFFFFFFFFu - 400; uint32_t t = S;
      drive(b, true, t, 100);
      drive(b, false, t, 150);
      drive(b, true, t, 100);
      drive(b, false, t, 150);
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_CLICK); assert(o.clicks == 2); }

    // Hold duration spanning rollover (unsigned-subtraction delta)
    { ButtonInput b; const uint32_t S = 0xFFFFFFFFu - 50; uint32_t t = S;
      drive(b, true, t, 3000);
      ButtonOut mid = b.update(true, t);
      assert(mid.pressed); assert(mid.heldMs >= 2900);
      ButtonOut o = drive(b, false, t, 600);
      assert(o.ev == BTN_EV_RELEASE);
      assert(o.heldMs >= 2900 && o.heldMs <= 3010); }

    // Debounce must not complete early when _rawSince + BTN_DEBOUNCE_MS wraps.
    // 0xFFFFFFF0 + 25 wraps to 9, so a naive `nowMs >= _rawSince + BTN_DEBOUNCE_MS`
    // sees a huge nowMs against a tiny sum and confirms the press after 10 ms.
    { ButtonInput b;
      uint32_t t = 0xFFFFFFF0u;
      b.update(true, t);              // raw goes high; _rawSince = t
      ButtonOut early = b.update(true, t + 10);
      assert(!early.pressed);         // only 10 ms elapsed — debounce not due
      ButtonOut late = b.update(true, t + 30);
      assert(late.pressed); }         // 30 ms elapsed — debounce complete

    printf("button_input: all tests passed\n");
    return 0;
}
