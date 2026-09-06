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

// ── Serin Link selected but silent ─────────────────────────────────────────
// A dial that stops sending sensor frames drops hasLinkSensor, so the selected
// source used to vanish from the list, the hero fell back to the internal
// row's NAME ("Heat Pump Sensor · no reading"), no banner fired (the single-
// mode branch was BLE-only and stale-only), and the header pill sat in neutral
// gray. The card must keep the row visible with the cause, mark the built-in
// row as the sensor actually in use, and raise the state to caution tier.
{
  // The screenshot scenario: Link selected, dial offline 12 min, two healthy
  // BLE sensors (one with a low battery, which must NOT win the banner slot).
  const IDLE_LINK = {
    roomMode: 0, roomSingle: 1, roomStatus: 2, room: 20,
    hasLinkSensor: false, linkTemp: null, linkHumidity: null,
    linkActive: false, linkStale: false, linkStaleMs: 0,
    remoteBonded: true, remoteLive: false, remoteLastSeen: 720,
    bleEnabled: true,
    bleSensors: [
      { i: 0, name: 'Nightstand', type: 'Govee', temp: 24.3, age: 240000, active: true, stale: false, batt: 17, rssi: -80 },
      { i: 1, name: 'Wall', type: 'Govee', temp: 23.8, age: 180000, active: true, stale: false, batt: 100, rssi: -75 }
    ]
  };
  const { newRoomRender } = require('./extract.js');
  const ui = newRoomRender();

  // The selected source stays in the list, with the cause on the sub-line.
  const model = ui.roomSensors(IDLE_LINK);
  const link = model.find(m => m.bit === 1);
  assert.ok(link, 'Serin Link row renders while selected, even with no sensor frame yet');
  assert.strictEqual(link.sub, 'Dial offline · last seen 12m ago',
    'silent-dial row says why: offline, with last-seen age');
  assert.ok(!link.active && !link.stale, 'a never-reported link is neither active nor stale');
  checks += 3;

  const liveLink = ui.roomSensors(Object.assign({}, IDLE_LINK, { remoteLive: true }))
    .find(m => m.bit === 1);
  assert.strictEqual(liveLink.sub, 'Connected · no reading yet',
    'a live dial with no reading says so instead of claiming offline');
  const noDial = ui.roomSensors(Object.assign({}, IDLE_LINK, { remoteBonded: false, remoteLive: false }))
    .find(m => m.bit === 1);
  assert.strictEqual(noDial.sub, 'No dial paired', 'selected with no bonded dial names the real gap');
  checks += 2;

  // Unselected and never reported stays hidden (a sensor-less dial is not a row).
  assert.ok(!ui.roomSensors(Object.assign({}, IDLE_LINK, { roomSingle: 2 })).find(m => m.bit === 1),
    'an unselected, never-reported link stays out of the list');
  // …but an Average-mode membership keeps it visible the same way.
  assert.ok(ui.roomSensors(Object.assign({}, IDLE_LINK, { roomMode: 1, roomMembers: 0b0010 })).find(m => m.bit === 1),
    'an average-mode member renders even before its first frame');
  checks += 2;

  // Hero foot names the silent source, not the fallback sensor.
  ui.renderRoomHero(IDLE_LINK, model);
  assert.strictEqual(ui.els.heroFoot.textContent,
    'Serin Link · no reading · using the built-in sensor',
    'hero foot names the source that is silent');
  checks++;

  // The banner slot goes to the source failure, not the battery warning.
  ui.renderRoomBanner(IDLE_LINK, model);
  assert.strictEqual(ui.els.bleBanner.className, 'ble-banner caution',
    'selected-but-silent raises a caution banner');
  assert.ok(/Serin Link/.test(ui.els.bleBanner.textContent) &&
            /dial is offline/i.test(ui.els.bleBanner.textContent) &&
            /last seen 12m ago/.test(ui.els.bleBanner.textContent) &&
            /built-in/.test(ui.els.bleBanner.textContent),
    'banner states the source, the cause, and the fallback: ' + ui.els.bleBanner.textContent);
  checks += 2;

  // Header pill leaves neutral gray for caution orange.
  ui.renderRoomSummary(IDLE_LINK, model);
  assert.strictEqual(ui.els.roomDot.className, 'status-dot warning', 'summary dot is warning-tier');
  assert.strictEqual(ui.els.roomStatusLabel.textContent, 'Serin Link · Not reporting',
    'summary pill says the source is not reporting');
  checks += 2;

  // The list shows reality: built-in row carries the highlight and "In use",
  // the configured-but-silent row sits idle with a muted "Selected" badge.
  const ih = ui.roomRowHtml(IDLE_LINK, model.find(m => m.bit === 0));
  assert.ok(/class="src-row sel"/.test(ih), 'built-in row takes the selection highlight during fallback');
  assert.ok(ih.indexOf('status-dot active') >= 0, 'built-in row dot goes green during fallback');
  assert.ok(ih.indexOf('In use') >= 0, 'built-in row is labeled In use during fallback');
  const lh = ui.roomRowHtml(IDLE_LINK, link);
  assert.ok(lh.indexOf('status-dot idle') >= 0, 'silent link row keeps an idle dot');
  assert.ok(lh.indexOf('Selected · no reading') >= 0, 'silent link row keeps a muted Selected badge');
  assert.ok(!/class="src-row sel"/.test(lh), 'silent link row gives up the selection highlight');
  assert.ok(lh.indexOf('aria-checked="true"') >= 0, 'tap semantics still follow the configured selection');
  checks += 7;

  // Stale Link now gets the single-mode warn banner (it was BLE-only).
  const staleSt = Object.assign({}, IDLE_LINK, {
    roomStatus: 1, hasLinkSensor: true, linkTemp: 24.2,
    linkActive: false, linkStale: true, linkStaleMs: 900000
  });
  const staleModel = ui.roomSensors(staleSt);
  ui.renderRoomBanner(staleSt, staleModel);
  assert.strictEqual(ui.els.bleBanner.className, 'ble-banner warn', 'stale link raises the warn banner');
  assert.ok(/Serin Link/.test(ui.els.bleBanner.textContent), 'stale banner names the link source');
  checks += 2;

  // Healthy selection regression: a live selected BLE row keeps Active + sel,
  // and the built-in row carries no fallback markers.
  const okSt = Object.assign({}, IDLE_LINK, { roomSingle: 2, roomStatus: 0 });
  const okModel = ui.roomSensors(okSt);
  const okRow = ui.roomRowHtml(okSt, okModel.find(m => m.bit === 2));
  assert.ok(/class="src-row sel"/.test(okRow) && okRow.indexOf('>Active<') >= 0,
    'a healthy selected row keeps the highlight and Active badge');
  const okInternal = ui.roomRowHtml(okSt, okModel.find(m => m.bit === 0));
  assert.ok(okInternal.indexOf('In use') < 0 && !/class="src-row sel"/.test(okInternal),
    'the built-in row carries no fallback markers while a remote is live');
  checks += 2;
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
