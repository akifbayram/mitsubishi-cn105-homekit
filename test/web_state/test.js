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
// Device card and mode-page nav bar: is each Save button resting?
const saveIdle = (ui) => ['settingsSaveBtn', 'mpSave'].map((id) => ui.els[id] && ui.els[id].disabled);

// Units change labels while every slider position stays in canonical Celsius.
same(DEFAULTS.target.min, '16', 'the page starts in Celsius');
{
  const ui = newUi('C');
  const before = snap(ui);
  ui.tap('C');
  same(snap(ui), before, 'active unit tap changes nothing');
  same(ui.sent, [], 'active unit tap sends nothing');
  ui.tap('F');
  same(snap(ui), before, 'F labels do not move canonical positions');
  assert.strictEqual(ui.label(22),71,'native table labels remain compatible');
  ui.configureSliders();
  ui.configureSliders();
  ui.tap('F');
  same(snap(ui),before,'repeated updates preserve all positions');
  same(ui.sent,[{cmd:'config',tempUnit:'F'}],'only changed unit is persisted');
}
{
  const ui = newUi('C');
  const ranges={ftabId:0,heat:{min:10,max:31,step:0.5},cool:{min:18,max:30,step:1},auto:{min:10,max:31,step:0.5}};
  ui.setState({mode:'heat',setpointRanges:ranges});
  for (const c of [10,30.5,31]) {
    ui.target.value=c;ui.heat.value=c;ui.cool.value=c;
    for(let i=0;i<12;i++) {ui.tap('F');ui.configureSliders();ui.tap('C');}
    same([ui.target.selectedC,ui.heat.selectedC,ui.cool.selectedC],[c,c,c],'extended endpoint survives repeated switches');
  }
  assert.strictEqual(ui.label(10),50,'extended profiles use linear conversion');
  same([ui.target.min,ui.target.max,ui.target.step],['10','31','0.5'],'custom min does not rely on 61 F');
  ui.setState({mode:'dry',setpointRanges:ranges});
  same([ui.target.min,ui.target.max,ui.target.step],['18','30','1'],'Dry target uses Cool/Dry range');
  same([ui.heat.min,ui.cool.max],['10','31'],'band controls retain Auto range');
  ranges.heat={min:22.5,max:23,step:0.5};
  ui.setState({mode:'heat',setpointRanges:ranges});
  ui.tap('F');
  ui.target.value=22.5;ui.configureSliders();
  assert.strictEqual(ui.target.selectedC,22.5);
  ui.target.value=23;ui.configureSliders();
  assert.strictEqual(ui.target.selectedC,23);
  ranges.ftabId=1;
  assert.strictEqual(ui.label(10),50,'physical readback outside a native profile must not be clamped');
  ranges.ftabId=0;
  same([ui.label(22.5),ui.label(23)],[73,73],'shared F labels keep distinct canonical endpoints');
}
{
  const {newRanges}=require('./extract.js');
  const climate=newRanges();
  climate.ack({ok:false,code:'outside_range',msg:'Temperature is outside the selected range'});
  same(climate.toasts,['Temperature is outside the selected range'],'ordinary climate rejection remains visible');
  const ui=newRanges();
  ui.submit({cmd:'config',rangeHeatSource:'manual',rangeHeatMin:10,rangeHeatMax:31},'Saved');
  same(ui.toasts,[],'sending does not claim persistence');
  same(ui.els.settingsSaveStatus.textContent,'Saving…','progress shows under the Device Save');
  same(saveIdle(ui),[true,true],'every Save rests while a save is in flight');
  ui.submit({cmd:'config',rangeHeatSource:'manual',rangeHeatMin:11,rangeHeatMax:31},'Latest saved');
  ui.ack({requestId:1,ok:true});
  assert.strictEqual(ui.dirty(),true,'late ack cannot approve newer submission');
  same(ui.toasts,[],'out of order ack shows no success');
  ui.editAgain();
  ui.ack({requestId:2,ok:true});
  assert.strictEqual(ui.dirty(),true,'matching ack cannot approve edits made after sending');
  same(ui.toasts,[],'newer unsaved edit remains visible');
  same(saveIdle(ui),[false,false],'unsaved edits light every Save up');
  ui.submit({cmd:'config'},'Latest saved');
  ui.ack({requestId:3,ok:false,code:'storage_failed',msg:'Could not save settings'});
  assert.strictEqual(ui.dirty(),true,'storage failure preserves edits');
  same(saveIdle(ui),[false,false],'a failed save leaves Save lit');
  ui.submit({cmd:'config'},'Latest saved');
  ui.ack({requestId:4,ok:true});
  assert.strictEqual(ui.dirty(),false,'matching ack confirms the current edit');
  same(saveIdle(ui),[true,true],'nothing left to save, so Save rests');
  assert.strictEqual(ui.toasts[ui.toasts.length-1],'Latest saved');
}
// The mode page has no status line, so a save that fails before any reply must toast.
{
  const {newRanges}=require('./extract.js');
  const offline=newRanges();
  offline.offline();
  offline.submit({cmd:'config'},'Settings saved');
  same(offline.toasts,['Not connected — not saved'],'an unsendable save says so wherever Save was pressed');
  const silent=newRanges();
  silent.submit({cmd:'config'},'Settings saved');
  silent.fireTimers();
  same(silent.toasts,['Save not confirmed. Reconnect and try again.'],'an unconfirmed save says so too');
  same(saveIdle(silent),[false,false],'and Save lights up again to retry');
}

// The Modes page offers Heat Pump Limits or Custom Limits. A mode still on the compatibility
// range reads as Custom 16–30.5 but stays compatibility until a bound changes:
// a real manual 16–30.5 is off-grid on whole-degree units.
const limits=(v)=>({custom:v.custom,min:v.min,max:v.max});
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const ranges={heat:{min:16,max:30.5,step:0.5,selection:'compatibility',manualMin:0,manualMax:0,reference:{min:10,max:28,source:'fresh'}},
    cool:{min:18,max:29,step:0.5,selection:'automatic',manualMin:0,manualMax:0,reference:{min:18,max:29,source:'fresh'}},
    auto:{min:16,max:30.5,step:0.5,selection:'compatibility',manualMin:0,manualMax:0,reference:null}};
  ui.render({setpointRanges:ranges});
  same(ui.addEdits({cmd:'config'}),{cmd:'config'},'unrelated saves leave every mode untouched');
  same(limits(ui.view('Heat')),{custom:true,min:16,max:30.5},'compatibility reads as Custom 16–30.5');
  ui.pick('Heat',false);
  same(limits(ui.view('Heat')),{custom:false,min:10,max:28},'Heat Pump Limits previews what the unit reports');
  same(ui.addEdits({cmd:'config'}),{cmd:'config',rangeHeatSource:'automatic'},'Heat Pump Limits replaces compatibility');
  ui.pick('Heat',true);
  same(ui.addEdits({cmd:'config'}),{cmd:'config'},'Custom again restores compatibility untouched');
  ui.nudge('Heat','min',1);
  ui.render({setpointRanges:ranges});
  same(ui.addEdits({cmd:'config'}),{cmd:'config',rangeHeatSource:'manual',rangeHeatMin:16.5,rangeHeatMax:30.5},'a bound change makes it a real custom range that survives refresh');
  same(ui.next('Heat','min',-1),16,'the low bound steps back down');
  const out={};ui.export({setpointRanges:ranges},out);
  same(out,{rangeHeatSource:'compatibility',rangeCoolSource:'automatic',rangeAutoSource:'compatibility'},'backup contains selections without detection cache');
  const previous={setpointRanges:ranges};
  assert.strictEqual(mergeState(previous,{trunc:true,power:true}).setpointRanges,ranges,'partial pushes retain complete preferences');
}

// Custom returns to the saved custom range, otherwise starts from the unit's.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const ranges={heat:{min:18,max:26,step:0.5,selection:'manual',manualMin:18,manualMax:26,reference:{min:10,max:28,source:'fresh'}},
    cool:{min:19,max:30,step:0.5,selection:'automatic',manualMin:0,manualMax:0,reference:{min:19,max:30,source:'fresh'}},
    auto:{min:19,max:28,step:0.5,selection:'automatic',manualMin:0,manualMax:0,reference:{min:19,max:28,source:'fresh'}}};
  ui.render({setpointRanges:ranges});
  ui.pick('Heat',false);ui.pick('Heat',true);
  same(ui.addEdits({cmd:'config'}),{cmd:'config'},'Heat Pump Limits then Custom restores the saved custom range');
  ui.pick('Cool',true);
  same(ui.addEdits({cmd:'config'}),{cmd:'config',rangeCoolSource:'manual',rangeCoolMin:19,rangeCoolMax:30},'a first custom range starts from the heat pump limits');
  ui.nudge('Heat','min',-1);ui.nudge('Heat','min',-1);
  same(limits(ui.view('Heat')),{custom:true,min:17,max:26},'each press moves one half degree');
}

// Steps follow the unit's grid. Compatibility always reports 0.5, so it borrows
// the grid from a mode that reports one.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const ranges={heat:{min:16,max:30.5,step:0.5,selection:'compatibility',manualMin:0,manualMax:0,reference:null},
    cool:{min:16,max:30,step:1,selection:'automatic',manualMin:0,manualMax:0,reference:null},
    auto:{min:20,max:25,step:1,selection:'manual',manualMin:20,manualMax:25,reference:null}};
  ui.render({setpointRanges:ranges});
  ui.nudge('Heat','max',-1);
  same(ui.addEdits({cmd:'config'}),{cmd:'config',rangeHeatSource:'manual',rangeHeatMin:16,rangeHeatMax:30},'whole-degree units snap 30.5 to 30 in one press');
  same(ui.next('Heat','min',-1),null,'whole-degree custom ranges stop at 16');
  ui.nudge('Auto','min',1);ui.nudge('Auto','min',1);ui.nudge('Auto','min',1);
  same(limits(ui.view('Auto')),{custom:true,min:23,max:25},'Auto low climbs to 2 degrees under high');
  same(ui.next('Auto','min',1),null,'Auto keeps its 2 degree gap');
}

// Fahrenheit: a press always changes the displayed whole degree.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const ranges={heat:{min:17.5,max:26,step:0.5,selection:'manual',manualMin:17.5,manualMax:26,reference:null},
    cool:{min:16,max:30.5,step:0.5,selection:'compatibility',manualMin:0,manualMax:0,reference:null},
    auto:{min:16,max:30.5,step:0.5,selection:'compatibility',manualMin:0,manualMax:0,reference:null}};
  ui.render({setpointRanges:ranges});
  ui.unit('F');
  same(ui.next('Heat','min',1),18.5,'64 °F at 17.5 and 18 °C, so the next press lands on 65 °F');
  same(ui.next('Heat','min',-1),17,'down from 64 °F lands on 63 °F');
}

// One mode mask behind the list, with mode_mask_sanitize()'s rules mirrored.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  ui.applyCaps(31);
  ui.setCap('heat',false);
  same(ui.caps(),2|4|8,'turning Heat off turns Auto off too');
  same(ui.toasts.length,1,'and says so');
  ui.setCap('auto',true);
  same(ui.caps(),2|4|8,'Auto cannot turn on without Heat and Cool');
  ui.setCap('cool',false);ui.setCap('dry',false);ui.setCap('fan',false);
  same(ui.caps(),8,'the last real mode stays on');
  ui.setCap('heat',true);ui.setCap('cool',true);ui.setCap('auto',true);
  same(ui.caps(),1|2|8|16,'Auto turns back on once Heat and Cool are on');
}

// Home app and Link get one range: the union over the enabled modes.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const ranges={heat:{min:10,max:28,step:0.5,selection:'automatic',manualMin:0,manualMax:0,reference:{min:10,max:28,source:'fresh'}},
    cool:{min:16,max:30.5,step:0.5,selection:'manual',manualMin:16,manualMax:30.5,reference:{min:19,max:30,source:'fresh'}},
    auto:{min:19,max:28,step:0.5,selection:'automatic',manualMin:0,manualMax:0,reference:{min:19,max:28,source:'fresh'}}};
  ui.render({setpointRanges:ranges});
  ui.applyCaps(31);
  same(ui.advertised(),{min:10,max:30.5},'spans every enabled mode');
  ui.setCap('heat',false);
  same(ui.advertised(),{min:16,max:30.5},'a disabled mode drops out');
  ui.setCap('cool',false);
  same(ui.advertised(),{min:16,max:30.5},'Dry keeps the shared Cool range in use');
}

// Whole-policy rollback preserves the exact profile/band used by real export.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const low={setpointRanges:{auto:{selection:'manual',manualMin:10,manualMax:18}},heatThresh:12,coolThresh:16,autoUnit:false};
  const retained=mergeState(low,{trunc:true,power:true});
  const lowBackup=ui.exportFile(retained).settings;
  same([retained.autoUnit,lowBackup.rangeAutoMin,lowBackup.rangeAutoMax,lowBackup.heatThresh,lowBackup.coolThresh],
    [false,10,18,12,16],'omitted atomic policy retains profile, both band endpoints and ownership for export');

  const high={setpointRanges:{auto:{selection:'manual',manualMin:22,manualMax:30}},heatThresh:24,coolThresh:28,autoUnit:true};
  const replaced=mergeState(retained,{trunc:true,...high});
  const highBackup=ui.exportFile(replaced).settings;
  same([replaced.autoUnit,highBackup.rangeAutoMin,highBackup.rangeAutoMax,highBackup.heatThresh,highBackup.coolThresh],
    [true,22,30,24,28],'complete newer policy replaces all export inputs together');
  same([low.heatThresh,low.coolThresh,low.autoUnit],[12,16,false],'merging does not mutate the prior complete policy');
}

// Operational thresholds may be projected; backup keeps deliberate saved intent.
{
  const {newRanges}=require('./extract.js');
  const ui=newRanges();
  const live={heatThresh:24,coolThresh:26,savedHeatThresh:20,savedCoolThresh:25};
  const backup=ui.exportFile(live).settings;
  same([backup.heatThresh,backup.coolThresh],[20,25],'backup exports saved thresholds rather than discovery projection');
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
  assert.ok(/cJSON_IsNumber\(value\).*std::isfinite\(value->valuedouble\)/.test(ws),
    'set targets must be gated on JSON numeric type and finite values');
  const srv = fs.readFileSync(path.join(__dirname, '..', '..', 'main', 'web_server.cpp'), 'utf8');
  assert.ok(/\.ws_pre_handshake_cb\s*=\s*wsPreHandshake/.test(srv),
    '/ws must register the Origin check as ws_pre_handshake_cb');
  checks += 2;
}

// ── Add Sensor session ─────────────────────────────────────────────────────
// Sensors can take minutes to show up, so the scan stays open until the user
// picks one or stops it. While it runs, the button is Stop and a status line
// counts up from the firmware's elapsed time, which every browser shares.
{
  const { bleScanView } = require('./extract.js');
  const open = bleScanView(true, 75, false);
  assert.strictEqual(open.btn, 'Stop Scanning', 'an open session turns the button into Stop');
  assert.strictEqual(open.disabled, false, 'Stop must be clickable');
  assert.strictEqual(open.scanning, true);
  assert.ok(/^Searching\u2026 1:15\./.test(open.status), 'elapsed time reads m:ss: ' + open.status);
  assert.ok(/a minute or more/.test(open.status), 'status sets the expectation that sensors are slow');
  assert.ok(/ 0:05\./.test(bleScanView(true, 5, false).status), 'seconds are zero-padded');
  const idle = bleScanView(false, 0, false);
  assert.deepStrictEqual(idle, { btn: 'Add Sensor\u2026', disabled: false, scanning: false, status: '' },
    'no session: plain Add Sensor with no status line');
  const full = bleScanView(false, 0, true);
  assert.strictEqual(full.btn, 'Sensor list full');
  assert.strictEqual(full.disabled, true, 'a full list cannot start a scan');
  // A session that is already open still shows Stop even if the list filled,
  // so it can always be ended.
  assert.strictEqual(bleScanView(true, 0, true).disabled, false);
  checks += 10;
}

(async function(){
  const {newRanges}=require('./extract.js');
  async function importFile(ui,settings){
    ui.import({files:[{text:()=>Promise.resolve(JSON.stringify({app:'cn105-homekit-settings',settings}))}],value:'file'});
    await new Promise(resolve=>setImmediate(resolve));
  }
  const malformed=newRanges();
  await importFile(malformed,{rangeHeatSource:'manual',rangeHeatMin:10,heatThresh:20,coolThresh:25});
  same(malformed.sent,[],'malformed import sends no partial profile or band');
  const invalidBand=newRanges();
  await importFile(invalidBand,{rangeHeatSource:'manual',rangeHeatMin:10,rangeHeatMax:31,heatThresh:'bad'});
  same(invalidBand.sent,[],'invalid imported band sends nothing');
  const equalBounds=newRanges();
  await importFile(equalBounds,{rangeHeatSource:'manual',rangeHeatMin:22,rangeHeatMax:22,heatThresh:20});
  same(equalBounds.sent,[],'empty imported range sends no profile or band');
  const good=newRanges();
  await importFile(good,{rangeAutoSource:'manual',rangeAutoMin:10,rangeAutoMax:31,heatThresh:12,coolThresh:16});
  same(good.sent,[{cmd:'config',rangeAutoSource:'manual',rangeAutoMin:10,rangeAutoMax:31,heatThresh:12,coolThresh:16,requestId:1}],'range and band import is one checked transaction');
  same(good.toasts,[],'import waits for its acknowledgment');
  good.ack({requestId:99,ok:true});
  same(good.toasts,[],'unrelated ack cannot approve import');
  good.ack({requestId:1,ok:true});
  same(good.toasts,['Settings imported'],'matching ack confirms persisted import');
  const old=newRanges();
  await importFile(old,{deviceName:'Old backup',heatThresh:20,coolThresh:25});
  same(old.sent,[{cmd:'config',deviceName:'Old backup',heatThresh:20,coolThresh:25,requestId:1}],'old backup leaves range choices untouched');
  console.log('PASS: web state handling ('+checks+' checks plus range transactions)');
})().catch(function(e){console.error(e);process.exitCode=1});
