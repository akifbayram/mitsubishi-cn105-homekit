const assert = require('assert');
const { newUi, newRoom, mergeState, DEFAULTS } = require('./extract.js');

let checks = 0;
function snap(ui) {
  return {
    target: { min: ui.target.min, max: ui.target.max, step: ui.target.step, value: ui.target.value },
    heat: { min: ui.heat.min, max: ui.heat.max, step: ui.heat.step, value: ui.heat.value },
    cool: { min: ui.cool.min, max: ui.cool.max, step: ui.cool.step, value: ui.cool.value }
  };
}
function same(a, b, msg) { assert.deepStrictEqual(a, b, msg); checks++; }

// ── Unit switching ─────────────────────────────────────────────────────────
// Re-entering the F branch for sliders that already show F used to push every
// handle through the C→F table a second time: 72 °F reads as 72 °C, the table
// has no such row, and the nearest one is 30.5 °C = 88 °F. The slider then
// sends that back as a real setpoint. Both branches must be idempotent, and
// the °C/°F buttons must ignore a tap on the unit already in use.

// The shipped page starts in Celsius, so a tap on °C is the reachable case.
same(DEFAULTS.target.min, '16', 'tempSlider ships in Celsius');

{
  const ui = newUi('C');
  const before = snap(ui);
  ui.tap('C');
  same(snap(ui), before, 'tapping °C while already °C moves no slider');
  assert.deepStrictEqual(ui.sent, [], 'tapping the active unit sends no config');
  assert.strictEqual(ui.cmdArmed(), false, 'tapping the active unit does not re-arm the command lock');
  checks += 2;
  // The same re-entry through the push path (onState re-applies at 1 Hz).
  ui.configureSliders();
  ui.configureSliders();
  same(snap(ui), before, 'C branch is idempotent');
}

{
  const ui = newUi('C');
  ui.tap('F');
  const inF = snap(ui);
  same(inF, {
    target: { min: '61', max: '88', step: '1', value: '71' },   // 22.0 °C
    heat: { min: '61', max: '88', step: '1', value: '68' },     // 20.0 °C
    cool: { min: '61', max: '88', step: '1', value: '77' }      // 25.0 °C
  }, 'C→F converts all three sliders and re-ranges them');

  ui.tap('F');
  same(snap(ui), inF, 'tapping °F while already °F moves no slider');
  assert.deepStrictEqual(ui.sent, [{ cmd: 'config', tempUnit: 'F' }],
    'tapping °F twice sends one config, not two');
  checks++;

  ui.configureSliders();
  ui.configureSliders();
  same(snap(ui), inF, 'F branch is idempotent');

  ui.tap('C');
  same(snap(ui), {
    target: { min: '16', max: '30.5', step: '0.5', value: '22' },
    heat: { min: '16', max: '30.5', step: '0.5', value: '20' },
    cool: { min: '16', max: '30.5', step: '0.5', value: '25' }
  }, 'F→C restores the values the page started with');
}

// Every setpoint the table can represent survives C→F→C unchanged, on all
// three sliders — the target one is not a special case.
{
  const probe = newUi('C');
  for (let f = 61; f <= 88; f++) {
    const c = probe.fToCTable(f);
    const ui = newUi('C');
    ui.target.value = c; ui.heat.value = c; ui.cool.value = c;
    ui.tap('F');
    same([ui.target.value, ui.heat.value, ui.cool.value],
      [String(f), String(f), String(f)], `${c} °C → ${f} °F`);
    ui.tap('C');
    same([ui.target.value, ui.heat.value, ui.cool.value],
      [String(c), String(c), String(c)], `${c} °C round trip`);
  }
}

// ── Partial state frames ───────────────────────────────────────────────────
// pushState() rolls back whole sections when a frame outgrows its buffer and
// flags the loss with "trunc". The fields those sections carried are then
// MISSING, not zero, and toggleRoomMember() XORs against one of them.
{
  const prev = { roomMembers: 5, bleSensors: [{ i: 0, name: 'Kitchen' }], remoteModel: 'dial' };
  const full = { power: true, roomMembers: 5 };
  assert.strictEqual(mergeState(prev, full), full,
    'a complete frame replaces wholesale — no field goes sticky');
  assert.strictEqual(mergeState(prev, { power: true }).remoteModel, undefined,
    'a complete frame that stops sending remoteModel clears it');
  checks += 2;

  const merged = mergeState(prev, { trunc: true, power: true });
  assert.strictEqual(merged.roomMembers, 5, 'a truncated frame keeps the last roomMembers');
  assert.deepStrictEqual(merged.bleSensors, prev.bleSensors, 'a truncated frame keeps the last sensor list');
  assert.strictEqual(merged.power, true, 'the truncated frame still supplies what it did carry');
  assert.strictEqual(prev.roomMembers, 5, 'merging does not mutate the previous frame');
  checks += 4;

  // Present-and-zero is a real value and must win — the case `|0` could not
  // tell apart from absent.
  assert.strictEqual(mergeState(prev, { trunc: true, roomMembers: 0 }).roomMembers, 0,
    'a truncated frame that does carry roomMembers:0 clears the members');
  // Nothing to merge with: the very first frame after a reconnect.
  const first = { trunc: true, power: true };
  assert.strictEqual(mergeState(null, first), first, 'the first frame is passed through');
  checks += 2;
}

// The writes themselves, for the frame the merge cannot repair.
{
  const r = newRoom({ roomMembers: 0b1101 });
  r.toggleRoomMember(2);
  assert.deepStrictEqual(r.sent, [{ cmd: 'config', roomMembers: 0b1001 }],
    'toggling a member flips one bit and leaves the rest');
  assert.strictEqual(r.state().roomMembers, 0b1001, 'the optimistic repaint carries the same value');
  checks += 2;

  const r2 = newRoom({ roomMembers: 0b1001 });
  r2.toggleRoomMember(1);
  assert.deepStrictEqual(r2.sent, [{ cmd: 'config', roomMembers: 0b1011 }], 'toggling a member on');
  checks++;

  // roomMembers absent: refuse rather than compute from 0 and clear the rest.
  const partial = newRoom({ trunc: true, power: true });
  partial.toggleRoomMember(2);
  assert.deepStrictEqual(partial.sent, [], 'a member toggle on a frame without roomMembers writes nothing');
  checks++;

  const none = newRoom(null);
  none.toggleRoomMember(2);
  assert.deepStrictEqual(none.sent, [], 'a member toggle before any frame writes nothing');
  checks++;

  // The other two writes send the value they were handed, so a missing field
  // costs at most a redundant write — never a wrong one.
  const m = newRoom({ trunc: true });
  m.setRoomMode(1);
  m.selectRoomSingle(3);
  assert.deepStrictEqual(m.sent, [{ cmd: 'config', roomMode: 1 }, { cmd: 'config', roomSingle: 3 }],
    'mode/source writes still send the tapped value on a partial frame');
  checks++;

  const noop = newRoom({ roomMode: 1, roomSingle: 3 });
  noop.setRoomMode(1);
  noop.selectRoomSingle(3);
  assert.deepStrictEqual(noop.sent, [], 'mode/source writes skip a no-op tap');
  checks++;
}

// The merge above is only reachable because onState() actually calls it, and
// that line lives outside the extracted block — deleting it would leave every
// test here green while restoring the bug. Pin it on the source text, the same
// way test/web_syntax asserts on the shipped HTML.
{
  const fs = require('fs');
  const path = require('path');
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');
  const onState = src.slice(src.indexOf('function onState('));
  const body = onState.slice(0, onState.indexOf('lastState=s'));
  assert.ok(/s\s*=\s*mergeState\s*\(\s*lastState\s*,\s*s\s*\)/.test(body),
    'onState() must merge the incoming frame before assigning lastState');
  checks++;
}

console.log('PASS: web state handling (' + checks + ' checks)');
