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

// Low heat widens only the target slider, down to 50 °F / 10 °C, and the
// 50-60 °F rows round-trip like the rest of the table.
{
  const ui = newUi('C');
  ui.setTargetMin(10);
  ui.configureSliders();
  same([ui.target.min, ui.heat.min, ui.cool.min], ['10', '16', '16'], 'low heat lowers the target floor alone (°C)');
  ui.target.value = '13';
  ui.tap('F');
  same([ui.target.min, ui.target.value, ui.heat.min], ['50', '55', '61'], 'low heat floor and a 13 °C target in °F');
  ui.tap('C');
  same(ui.target.value, '13', '55 °F comes back as 13 °C');
  ui.setTargetMin(16);
  ui.configureSliders();
  same(ui.target.min, '16', 'leaving low heat restores the floor');
  for (let f = 50; f <= 60; f++) {
    const c = ui.fToCTable(f);
    assert.ok(c >= 10 && c < 16, `${f} °F is a low heat setpoint`);
    same(ui.cToFTable(c), f, `${f} °F → ${c} °C → ${f} °F`);
  }
  same(ui.fToCTable(40), 10, 'below the table clamps to 50 °F');
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

// ── Heat pump link down ────────────────────────────────────────────────────
// Every remote feed and every room-temperature poll rides the CN105 link. With
// it down, s.room is the LAST poll and nothing reaches the pump, yet the card
// printed that stale number under "Heat pump sees", badged the selection
// Active, and the Climate panel named the sensor in green. A bench unit not yet
// wired to the indoor unit read as "my sensor is ignored". Sensor readings,
// their ages and their own liveness dots are still true and must stay.
{
  const { newRoomRender } = require('./extract.js');
  const ui = newRoomRender();
  const SENSORS = [
    { i: 0, name: 'Living Room', type: 'Govee', temp: 22.4, age: 12000, active: true, stale: false, batt: 80, rssi: -70 },
    { i: 1, name: 'Bedroom', type: 'Govee', temp: 21.9, age: 40000, active: true, stale: false, batt: 90, rssi: -72 }
  ];
  const LIVE = {
    connected: true, room: 22.5, roomMode: 0, roomSingle: 2, roomStatus: 0,
    bleEnabled: true, bleSensors: SENSORS, hasLinkSensor: false
  };
  const OFF = Object.assign({}, LIVE, { connected: false, room: 20.5 });

  // The pump's reading is withheld only on an explicit false: a frame that
  // predates the field must keep rendering as before.
  assert.strictEqual(ui.pumpRoom(OFF), null, 'no pump reading while the link is down');
  assert.strictEqual(ui.pumpRoom(LIVE), 22.5, 'pump reading passes through while connected');
  assert.strictEqual(ui.pumpRoom({ room: 20 }), 20, 'a frame without connected is not offline');
  checks += 3;

  // Hero: no number, one neutral line naming what will be sent.
  let model = ui.roomSensors(OFF);
  ui.renderRoomHero(OFF, model);
  assert.strictEqual(ui.els.heroVal.textContent, '--', 'hero blanks while offline');
  assert.strictEqual(ui.els.heroFoot.textContent,
    'Not connected to the heat pump · Living Room will be sent when it reconnects',
    'hero foot explains the link, not the sensor');
  assert.strictEqual(ui.els.heroFoot.className, 'hero-foot', 'offline foot is neutral, not warn');
  checks += 3;

  ui.renderRoomHero(LIVE, ui.roomSensors(LIVE));
  assert.strictEqual(ui.els.heroVal.textContent, '22.5', 'connected hero still shows the echoed value');
  assert.strictEqual(ui.els.heroFoot.textContent, 'Living Room · updated 12s ago', 'connected foot unchanged');
  checks += 2;

  // Rows: the selection stays highlighted but is not "Active"; sensor dots
  // keep meaning "this sensor is reporting", so a live selected sensor does
  // not go grey next to a live unselected one.
  const selRow = ui.roomRowHtml(OFF, model.find(m => m.bit === 2));
  assert.ok(/class="src-row sel"/.test(selRow), 'selection highlight survives the outage');
  assert.ok(selRow.indexOf('src-badge mut">Selected<') >= 0 && selRow.indexOf('>Active<') < 0,
    'Active becomes a muted Selected while offline');
  assert.ok(selRow.indexOf('status-dot active') >= 0, 'a live selected sensor keeps its live dot');
  assert.ok(ui.roomRowHtml(OFF, model.find(m => m.bit === 3)).indexOf('status-dot active') >= 0,
    'an unselected live sensor keeps its live dot too');
  const intRow = model.find(m => m.bit === 0);
  assert.strictEqual(intRow.temp, null, 'built-in row shows no reading while offline');
  checks += 5;

  // Fallback is a claim about what the pump reads; offline it cannot be made.
  const staleOff = Object.assign({}, OFF, { roomStatus: 1 });
  const staleModel = ui.roomSensors(staleOff);
  const fbInt = ui.roomRowHtml(staleOff, staleModel.find(m => m.bit === 0));
  assert.ok(fbInt.indexOf('In use') < 0 && !/class="src-row sel"/.test(fbInt) &&
            fbInt.indexOf('status-dot idle') >= 0,
    'built-in row is not "In use" while offline');
  const fbSel = ui.roomRowHtml(staleOff, staleModel.find(m => m.bit === 2));
  assert.ok(/class="src-row sel"/.test(fbSel) && fbSel.indexOf('Selected · stale') >= 0,
    'a stale selection keeps its highlight and stale badge while offline');
  // Not overridden by a live feed here, so only the link rule blanks it.
  assert.strictEqual(staleModel.find(m => m.bit === 0).temp, null,
    'built-in row shows no reading during an offline fallback');
  assert.ok(fbInt.indexOf('src-temp') < 0, 'built-in row renders no temperature during an offline fallback');
  checks += 4;

  // Banner: the sensor trouble stays, the claim about the pump goes.
  ui.renderRoomBanner(Object.assign({}, staleOff, { bleReverted: true }), staleModel);
  assert.ok(/No update from Living Room/.test(ui.els.bleBanner.textContent) &&
            !/built-in/.test(ui.els.bleBanner.textContent),
    'offline stale banner drops "reverted to the built-in sensor": ' + ui.els.bleBanner.textContent);
  ui.renderRoomBanner(staleOff, staleModel);
  assert.ok(/No update from/.test(ui.els.bleBanner.textContent), 'connected-style stale banner still fires offline');
  checks += 2;

  const silentOff = Object.assign({}, OFF, {
    roomSingle: 1, roomStatus: 2, remoteBonded: true, remoteLive: true
  });
  const silentModel = ui.roomSensors(silentOff);
  ui.renderRoomBanner(silentOff, silentModel);
  assert.strictEqual(ui.els.bleBanner.textContent, 'Serin Link is connected but hasn’t sent a reading yet.',
    'offline no-reading banner drops "Using the heat pump\'s built-in sensor"');
  ui.renderRoomHero(silentOff, silentModel);
  assert.strictEqual(ui.els.heroFoot.textContent, 'Not connected to the heat pump',
    'a selection with no reading promises nothing on reconnect');
  checks += 2;

  // Built-in selected: nothing is sent, so the foot and labels say only that.
  const intOff = Object.assign({}, OFF, { roomSingle: 0 });
  const intModel = ui.roomSensors(intOff);
  const intSel = ui.roomRowHtml(intOff, intModel.find(m => m.bit === 0));
  assert.ok(intSel.indexOf('status-dot idle') >= 0 && intSel.indexOf('src-badge mut">Selected<') >= 0,
    'selected built-in row is idle and Selected while offline');
  assert.strictEqual(intModel.find(m => m.bit === 0).temp, null,
    'selected built-in row shows no reading while offline');
  assert.ok(intSel.indexOf('src-temp') < 0, 'selected built-in row renders no temperature while offline');
  checks += 2;
  ui.renderRoomHero(intOff, intModel);
  assert.strictEqual(ui.els.heroFoot.textContent, 'Not connected to the heat pump',
    'built-in selection: no "will be sent" promise');
  ui.renderRoomSummary(intOff, intModel);
  assert.strictEqual(ui.els.roomStatusLabel.textContent, 'Heat pump offline',
    'built-in selection header does not repeat "heat pump"');
  checks += 3;

  // Section header: neutral, names the selection and the link.
  ui.renderRoomSummary(OFF, model);
  assert.strictEqual(ui.els.roomDot.className, 'status-dot idle', 'header dot idle while offline');
  assert.strictEqual(ui.els.roomStatusLabel.textContent, 'Living Room · Heat pump offline',
    'header names the selection and the outage');
  assert.strictEqual(ui.els.roomStatusLabel.style.color, 'var(--text3)', 'header label is neutral grey');
  checks += 3;

  // Average: no number, chips stay (they are sensor values), foot and header
  // follow the same rule.
  const avgOff = Object.assign({}, OFF, {
    roomMode: 1, roomMembers: 0b1100, contrib: 0b1100, avgFeeding: true, effTemp: 22.15
  });
  const avgModel = ui.roomSensors(avgOff);
  ui.renderRoomHero(avgOff, avgModel);
  assert.strictEqual(ui.els.heroVal.textContent, '--', 'average hero blanks while offline');
  assert.ok(/Living Room/.test(ui.els.heroChips.innerHTML) && /Bedroom/.test(ui.els.heroChips.innerHTML),
    'average chips keep the sensor values');
  assert.strictEqual(ui.els.heroFoot.textContent,
    'Not connected to the heat pump · the average will be sent when it reconnects',
    'average foot explains the link');
  ui.renderRoomSummary(avgOff, avgModel);
  assert.strictEqual(ui.els.roomStatusLabel.textContent, 'Average · Heat pump offline',
    'average header names the outage');
  checks += 4;

  // Average with nothing reporting: no fallback chip or banner clause naming
  // the pump's sensor, and the built-in row shows no reading.
  const fbAvgOff = Object.assign({}, avgOff, {
    contrib: 0, avgFeeding: false, avgFallback: true, exclStale: 0b1100
  });
  const fbAvgModel = ui.roomSensors(fbAvgOff);
  assert.strictEqual(fbAvgModel.find(m => m.bit === 0).temp, null,
    'average built-in row shows no reading while offline');
  ui.renderRoomHero(fbAvgOff, fbAvgModel);
  assert.ok(!/fallback/.test(ui.els.heroChips.innerHTML), 'no "Built-in sensor · fallback" chip while offline');
  assert.strictEqual(ui.els.heroFoot.textContent, 'Not connected to the heat pump',
    'average with no live member promises nothing on reconnect');
  ui.renderRoomBanner(fbAvgOff, fbAvgModel);
  assert.strictEqual(ui.els.bleBanner.textContent, 'No remote readings.',
    'average fallback banner drops the built-in clause while offline');
  ui.renderRoomSummary(fbAvgOff, fbAvgModel);
  assert.strictEqual(ui.els.roomStatusLabel.textContent, 'Heat pump offline',
    'average fallback header names only the outage');
  checks += 5;

  // Connected fallback keeps its existing copy.
  const fbAvgOn = Object.assign({}, fbAvgOff, { connected: true });
  ui.renderRoomBanner(fbAvgOn, ui.roomSensors(fbAvgOn));
  assert.ok(/Using the heat pump's built-in sensor/.test(ui.els.bleBanner.textContent),
    'connected average fallback banner is unchanged');
  ui.renderRoomHero(fbAvgOn, ui.roomSensors(fbAvgOn));
  assert.ok(/Built-in sensor · fallback/.test(ui.els.heroChips.innerHTML),
    'connected average fallback chip is unchanged');
  checks += 2;

  // Climate panel source line under the Room reading.
  assert.deepStrictEqual(ui.tempSourceView(LIVE), { label: 'Living Room', live: true, stale: false },
    'connected source line is unchanged');
  assert.deepStrictEqual(ui.tempSourceView(OFF),
    { label: 'Living Room · heat pump offline', live: false, stale: false },
    'offline source line drops the green and names the outage');
  assert.deepStrictEqual(ui.tempSourceView(intOff),
    { label: 'Heat pump offline', live: false, stale: false },
    'built-in selection source line does not repeat "heat pump"');
  assert.deepStrictEqual(ui.tempSourceView(avgOff),
    { label: 'Average · 2 sensors · heat pump offline', live: false, stale: false },
    'average source line names the outage');
  checks += 4;
}

// The Climate panel lines live in onState(), outside the extracted block, so
// pin that they go through the helpers above.
{
  const fs = require('fs');
  const path = require('path');
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');
  assert.ok(/roomTemp\.textContent\s*=\s*fmtReadingTemp\(\s*pumpRoom\(\s*s\s*\)\s*\)/.test(src),
    'Climate Room reading must go through pumpRoom()');
  assert.ok(/\$\('roomDeg'\)\.hidden\s*=\s*\(\s*pumpRoom\(\s*s\s*\)\s*==\s*null\s*\)/.test(src),
    'Climate degree sign must hide with the pumpRoom() reading');
  assert.ok(/tempSourceView\(\s*s\s*\)/.test(src.slice(src.indexOf('function onState('))),
    'Climate source line must go through tempSourceView()');
  assert.ok(/pumpOffline\(\s*s\s*\)\s*\?\s*'\.'\s*:\s*" — using the heat pump's internal sensor\."/.test(src),
    'Link card stale banner must drop the internal-sensor clause while offline');
  checks += 4;
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

// The firmware refuses restart / hkReset / wifiForget frames without their
// typed confirm word (main/web_ws.cpp). The page must send exactly those words,
// and must not render a missing setup code (sent only while unpaired) as text.
{
  const fs = require('fs');
  const path = require('path');
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');
  [['restart', 'RESTART'], ['hkReset', 'RESET'], ['wifiForget', 'FORGET'], ['factoryReset', 'ERASE']]
    .forEach(function (p) {
      assert.ok(src.includes("send({cmd:'" + p[0] + "',confirm:'" + p[1] + "'})"),
        p[0] + ' must send confirm ' + p[1]);
      checks++;
    });
  assert.ok(src.includes("hkSetupCodeEl.textContent=s.hkSetupCode||'--'"),
    'absent setup code must render as a placeholder');
  checks++;
}

// Forget-all removes every bond and restarts, so an accidental confirmation
// must not send it. Exercise the shipped typed prompt and click handlers;
// single-dial forget still uses the plain browser confirmation.
{
  const fs = require('fs');
  const path = require('path');
  const vm = require('vm');
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');
  const typed = src.slice(src.indexOf('  function typedConfirm('), src.indexOf('  window.factoryReset='));
  const handlers = src.slice(src.indexOf("  document.getElementById('forgetRemoteBtn').addEventListener"),
    src.indexOf("  document.getElementById('pairRemoteBtn').addEventListener"));
  const dialCmd = src.match(/function forgetDialCmd\(mac\)\{[^\n]*\}/)[0];
  const create = vm.runInThisContext('(function(answer,count){\n' +
    'var sent=[],prompts=[],confirms=[],callbacks={},remoteDialCount=count;\n' +
    'function prompt(msg){prompts.push(msg);return answer}\n' +
    'function confirm(msg){confirms.push(msg);return true}\n' +
    'function showToast(){}\n' +
    'function send(msg){sent.push(msg)}\n' +
    'var document={getElementById:function(id){return {addEventListener:function(type,cb){callbacks[id]=cb}}}};\n' +
    typed + dialCmd + '\n' + handlers + '\n' +
    'return {sent:sent,prompts:prompts,confirms:confirms,callbacks:callbacks};})',
    { filename: 'web/index.html#forget-commands' });

  for (const count of [0, 3]) {
    for (const answer of [null, '', 'RESET', 'FORGETTING']) {
      const ui = create(answer, count);
      ui.callbacks.forgetRemoteBtn();
      same(ui.sent, [], 'forget-all sends nothing for cancelled or incorrect typed input');
      assert.strictEqual(ui.prompts.length, 1, 'forget-all asks for a typed word');
      assert.ok(ui.prompts[0].includes('Type FORGET to confirm:'), 'forget-all prompt asks for FORGET');
      assert.ok(/restart/.test(ui.prompts[0]), 'forget-all prompt explains the restart');
      same(ui.confirms, [], 'forget-all uses the typed prompt');
      checks += 3;
    }
    for (const answer of ['FORGET', ' forget ']) {
      const ui = create(answer, count);
      ui.callbacks.forgetRemoteBtn();
      same(ui.sent, [{ cmd: 'forgetRemote', confirm: 'FORGET' }],
        'forget-all sends the exact confirm word after typed confirmation');
    }
  }

  const one = create(null, 3);
  one.callbacks.remoteDialList({ target: { closest: function () {
    return { getAttribute: function () { return 'AA:BB:CC:DD:EE:FF'; } };
  } } });
  same(one.sent, [{ cmd: 'forgetDial', mac: 'AA:BB:CC:DD:EE:FF' }], 'single-dial forget sends only its MAC');
  same(one.prompts, [], 'single-dial forget needs no typed word');
  assert.strictEqual(one.confirms.length, 1, 'single-dial forget keeps plain browser confirm');
  checks++;

  // Pin the server guard before the destructive call, consistent with the
  // existing firmware source checks in this suite.
  const ws = fs.readFileSync(path.join(__dirname, '..', '..', 'main', 'web_ws.cpp'), 'utf8');
  const begin = ws.indexOf('} else if (strcmp(cmd, "forgetRemote") == 0) {');
  const branch = ws.slice(begin, ws.indexOf('} else if (strcmp(cmd, "forgetDial")', begin));
  assert.ok(/if\s*\(!webConfirmed\(msg, "FORGET"\)\)\s*\{[\s\S]*?sendWsText\([\s\S]*?return;\s*\}/.test(branch),
    'forgetRemote must reject frames without exact FORGET confirmation');
  assert.ok(branch.indexOf('webConfirmed') < branch.indexOf('espnow_forget_and_restart()'),
    'forgetRemote must check confirmation before removing bonds and restarting');
  checks += 2;
}

// Once paired the firmware sends no setup code or URI, so the page must not
// offer a Show Setup Code control, must say why, and must clear a QR drawn
// before the pairing.
{
  const fs = require('fs');
  const path = require('path');
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');
  assert.ok(src.includes("$('hkShowSetupBtn').style.display=hkPairedNow?'none':''"),
    'Show Setup Code button must be hidden while paired');
  assert.ok(src.includes('id="hkSetupHidden"') &&
    src.includes("$('hkSetupHidden').style.display=hkPairedNow?'':'none'") &&
    src.includes('Setup code hidden after pairing'),
    'paired state must explain the hidden setup code');
  assert.ok(/else if\(!s\.hkSetupURI&&lastQRURI\)\{\s*lastQRURI='';\s*qrCanvas\.getContext\('2d'\)\.clearRect/.test(src),
    'an empty setup URI must clear the drawn QR and lastQRURI');
  checks += 3;

  // Firmware side, pinned on source text: the set handler only acts on a
  // target that is a finite number, and the Origin check runs before the 101.
  const ws = fs.readFileSync(path.join(__dirname, '..', '..', 'main', 'web_ws.cpp'), 'utf8');
  assert.ok(/jsonGetFloat\(msg, "target", &floatVal\)\s*&&\s*std::isfinite\(floatVal\)/.test(ws),
    'set target must be gated on jsonGetFloat and isfinite');
  const srv = fs.readFileSync(path.join(__dirname, '..', '..', 'main', 'web_server.cpp'), 'utf8');
  assert.ok(/\.ws_pre_handshake_cb\s*=\s*wsPreHandshake/.test(srv),
    '/ws must register the Origin check as ws_pre_handshake_cb');
  checks += 2;
}

console.log('PASS: web state handling (' + checks + ' checks)');
