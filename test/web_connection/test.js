// Execute the shipped browser connection callbacks with controllable delivery.
// Closing a socket does not cancel already queued browser events or timers.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const source = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');
const start = source.indexOf('  function connect(){');
const end = source.indexOf('  function wsReady()', start);
assert(start >= 0 && end > start, 'production connection functions missing');

function setup() {
  const sockets = [], timers = new Map(), cleared = new Set(), states = [], messages = [];
  let nextTimer = 1, visibility;
  const context = {
    ws: null, connectTimeout: null, reconnTimer: null, reconnDelay: 0,
    hbTimer: null, lastConnectedAt: 0, lastMsgTime: 0, fwWait: null,
    location: { hostname: 'unit.local', port: '' },
    Date: { now: () => 10000 },
    document: { hidden: false, addEventListener: (_, callback) => { visibility = callback; } },
    setTimeout(callback) { const id = nextTimer++; timers.set(id, callback); return id; },
    setInterval(callback) { const id = nextTimer++; timers.set(id, callback); return id; },
    clearTimeout(id) { cleared.add(id); },
    clearInterval(id) { cleared.add(id); },
    setConnState(state) { states.push(state); },
    onState(message) { messages.push(message); },
    fwBack() {},
    endRestarting() {},
    WebSocket: function () {
      this.readyState = 0;
      this.sent = [];
      this.close = () => { this.readyState = 2; };
      this.send = message => { assert.strictEqual(this.readyState, 1); this.sent.push(message); };
      sockets.push(this);
    }
  };
  vm.createContext(context);
  vm.runInContext(source.slice(start, end), context, { filename: 'web/index.html#connection' });
  return { context, sockets, timers, cleared, states, messages, visible: () => visibility() };
}

let failures = 0;
function test(name, run) {
  try { run(); console.log('PASS: ' + name); }
  catch (error) { ++failures; console.error('FAIL: ' + name + ': ' + error.message); }
}

test('delayed old close does not orphan the replacement or clear its timers', () => {
  const t = setup(), c = t.context;
  c.connect(); const old = c.ws;
  t.visible(); const replacement = c.ws, deadline = c.connectTimeout;
  old.onclose();
  assert.strictEqual(c.ws, replacement);
  assert.strictEqual(c.connectTimeout, deadline);
  replacement.readyState = 1; replacement.onopen();
  assert.strictEqual(c.ws, replacement);
  assert.deepStrictEqual(replacement.sent, ['{"cmd":"diag"}']);
  assert.strictEqual(c.reconnTimer, null);
});
test('delayed old error cannot close the current socket', () => {
  const t = setup(), c = t.context;
  c.connect(); const old = c.ws;
  c.connect(); const replacement = c.ws;
  replacement.readyState = 1; replacement.onopen();
  old.onerror();
  assert.strictEqual(replacement.readyState, 1);
});
test('delayed old open and message cannot mark connected or overwrite state', () => {
  const t = setup(), c = t.context;
  c.connect(); const old = c.ws;
  c.connect(); const deadline = c.connectTimeout;
  old.onopen(); old.onmessage({ data: '{"type":"state","target":10}' });
  assert.deepStrictEqual(t.states, []);
  assert.deepStrictEqual(t.messages, []);
  assert.strictEqual(c.connectTimeout, deadline);
});
test('queued old timeout cannot close a new connecting socket', () => {
  const t = setup(), c = t.context;
  c.connect(); const expired = t.timers.get(c.connectTimeout);
  c.connect(); const replacement = c.ws, deadline = c.connectTimeout;
  expired();
  assert.strictEqual(c.ws, replacement);
  assert.strictEqual(replacement.readyState, 0);
  assert.strictEqual(c.connectTimeout, deadline);
});
test('queued old heartbeat cannot close a new open socket', () => {
  const t = setup(), c = t.context;
  c.connect(); const old = c.ws;
  old.readyState = 1; old.onopen();
  const heartbeat = t.timers.get(c.hbTimer);
  old.readyState = 2; c.connect(); const replacement = c.ws;
  replacement.readyState = 1; replacement.onopen(); c.lastMsgTime = 0;
  heartbeat();
  assert.strictEqual(replacement.readyState, 1);
});
test('current close schedules reconnect and current timeout retires its socket', () => {
  const t = setup(), c = t.context;
  c.connect(); const old = c.ws;
  old.readyState = 1; old.onopen(); old.onclose();
  assert.strictEqual(c.ws, null);
  const reconnect = t.timers.get(c.reconnTimer); reconnect();
  const current = c.ws;
  assert.notStrictEqual(current, old);
  t.timers.get(c.connectTimeout)();
  assert.strictEqual(current.readyState, 2);
  assert.strictEqual(c.ws, null);
  assert(c.reconnTimer !== null);
});
test('current heartbeat closes a silent socket and accepts current messages', () => {
  const t = setup(), c = t.context;
  c.connect(); const current = c.ws;
  current.readyState = 1; current.onopen();
  current.onmessage({ data: '{"type":"state","target":22}' });
  assert.strictEqual(t.messages[0].target, 22);
  c.lastMsgTime = 0;
  t.timers.get(c.hbTimer)();
  assert.strictEqual(current.readyState, 2);
});
if (failures) process.exit(1);
