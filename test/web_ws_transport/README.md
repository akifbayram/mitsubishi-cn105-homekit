# WebSocket backpressure regression

Run `bash test/web_ws_transport/run.sh` from the repository root. The harness
compiles the production transport and extracts the actual `WebUI::broadcastWs`
and `sendWsText` methods. Its HTTPD task deliberately blocks a socket send while
the protocol caller keeps publishing; it also checks saturated discovery,
connection reuse, failed-client retirement, direct replies, short writes,
allocation/admission failures (including lost successful submissions), delayed
duplicate callbacks, and teardown with a callback in flight.

Before this fix the same boundary harness failed with:
`WebUI::broadcastWs blocked protocol caller behind socket send`.

The transport has at most eight pending messages and one HTTPD delivery in
flight, with an aggregate payload allocation cap of 8192 bytes (including the
in-flight payload; allocator metadata is additional). Frames may contain up to
4111 text bytes plus their terminator. State and discovery snapshots coalesce;
logs are best effort. Discovery may evict pending periodic state to preserve
one-shot scan completion. The main loop retries rejected scan snapshots and
serializes new scan requests with that pending state. Already admitted frames
keep HTTPD delivery order; a later pending discovery snapshot replaces an older
unsent one. Picking a result still dismisses later scan updates in the existing
browser handler. Publication takes its mutex with zero wait and does
not touch HTTPD sessions or sockets. State capture remains on its original
caller, outside the scheduler task.

The scheduling task uses a 3072-byte stack. It retains IDF's default nonblocking
HTTPD admission and retries missing progress every 500 ms; dropped UDP wakeups
cannot strand data. Delayed duplicate callbacks harmlessly drain the next
message or do nothing. Callback backlog is bounded by the SDK control mailbox;
callbacks allocate no payload or ticket memory. Enabling the global blocking
HTTPD option would also affect HomeKit's separate server and can deadlock its
multi-session close path, so this transport does not change that option. Socket transmission stays on HTTPD so SDK PONG/CLOSE frames and
immediate command replies cannot interleave. Only HTTPD registers/retires
clients; queued destinations include a connection generation. WebSocket socket
sends have a 250 ms timeout per call (at most two calls per text frame). A
failed or partial send retires the client, and shutdown lets HTTPD close it.
The production server/transport live until reboot; this change does not add a
runtime HTTPD stop API. Startup failure releases partial resources before any
WebSocket handlers are registered. Host teardown joins the scheduling task,
drains and joins fake HTTPD, then releases resources. The HTTPD global user context is borrowed, not heap owned.

These are host boundary tests, not a Wi-Fi or ESP-NOW hardware soak. They prove
that a blocked web socket no longer blocks its protocol caller; they do not
attribute any historical watchdog reset to this path. Hardware checks should
cover a normal browser, several clients, a client that stops reading, and
reconnects while Link control and state traffic continue.
