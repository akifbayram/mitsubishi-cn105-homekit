// Execute the shipped export/import and credential handlers at their browser
// boundaries. Only the DOM, file download and transport are replaced.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const root = path.join(__dirname, '../..');
const index = fs.readFileSync(path.join(root, 'web/index.html'), 'utf8');
const recovery = fs.readFileSync(path.join(root, 'web/recovery.html'), 'utf8');

function handler(source, signature, ending = '  };') {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  const end = source.indexOf('\n' + ending, start);
  assert.notEqual(end, -1, signature + ' end');
  return source.slice(start, end + ending.length + 1);
}

const original = {
  deviceName: 'Bedroom', tempUnit: 'C', pollInterval: 2000, cn105Baud: 2400,
  logLevel: 3, vaneConfig: 2, modeMask: 31, heatThresh: 20, coolThresh: 24,
  roomSource: 0, roomMode: 1, roomSingle: 4, roomMembers: 20,
  roomOffs: [0, 6, -2, 0, 8, 0], bleTimeout: 600, bleEnabled: true,
  bleAddr: 'AA:BB:CC:DD:EE:01',
  bleSensors: [
    { i: 0, addr: 'AA:BB:CC:DD:EE:01', name: 'Bedroom', temp: 23.5, age: 1000 },
    { i: 2, addr: 'AA:BB:CC:DD:EE:02', name: 'Office \\"', temp: 22.5, age: 1000 }
  ],
  betaChannel: true, nightLat: 42, nightLon: -71,
  nightDuskOff: 10, nightDawnOff: -10, nightCeil: 30
};

function browser(state = original, failAt = -1) {
  const sent = [], toasts = [];
  let exported;
  const context = {
    window: {}, lastState: structuredClone(state), RUNNING_FW_VER: 'test',
    ensureConnected() { return true; },
    send(message) {
      if (sent.length === failAt) return false;
      sent.push(JSON.parse(JSON.stringify(message)));
      return true;
    },
    showToast(message, error) { toasts.push({ message, error }); },
    Blob: class { constructor(parts) { exported = JSON.parse(parts[0]); } },
    URL: { createObjectURL() { return 'blob:test'; }, revokeObjectURL() {} },
    document: {
      createElement() { return { click() {}, remove() {} }; },
      body: { appendChild() {} }
    },
    setTimeout() {}
  };
  // The import's first message is the checked config: run the real range
  // validation and submit path with only their UI/timer boundaries stubbed.
  Object.assign(context, { RANGE_NAMES: ['Heat', 'Cool', 'Auto'], rangeRequestId: 0, rangePending: null,
    rangeEditVersion: 0, renderSaveState() {}, rangeStatus() {},
    saveFailed(message) { toasts.push({ message, error: true }); } });
  vm.createContext(context);
  vm.runInContext(handler(index, '  function exportRangeSettings(st,out){', '  }') + '\n' +
    handler(index, '  function validRangeSettings(st){', '  }') + '\n' +
    handler(index, '  function copyRangeSettings(from,to){', '  }') + '\n' +
    handler(index, '  function submitCheckedConfig(cfg,success){', '  }') + '\n' +
    handler(index, '  window.exportSettings=function(){') + '\n' +
    handler(index, '  window.importSettings=function(input){'), context);
  return {
    sent, toasts,
    export() { context.window.exportSettings(); return exported; },
    async import(file) {
      context.window.importSettings({ files: [{ text: async () => JSON.stringify(file) }], value: 'file' });
      await new Promise(resolve => setImmediate(resolve));
    }
  };
}

test('backup includes current room model, named sensor identities, offsets, night settings and channel', () => {
  const backup = browser().export();
  assert.equal(backup.v, 1);
  assert.equal(backup.settings.roomSource, 0); // cached old importers still have their legacy field
  for (const key of ['roomMode', 'roomSingle', 'roomMembers', 'roomOffs', 'betaChannel',
    'nightLat', 'nightLon', 'nightDuskOff', 'nightDawnOff', 'nightCeil']) {
    assert.deepEqual(backup.settings[key], original[key], key);
  }
  assert.deepEqual(backup.settings.bleSensors, [
    { i: 0, addr: 'AA:BB:CC:DD:EE:01', name: 'Bedroom' },
    { i: 2, addr: 'AA:BB:CC:DD:EE:02', name: 'Office \\"' }
  ]);
  assert.deepEqual(backup.settings.ble, { enabled: true, addr: original.bleAddr, timeout: 600 });
});

test('same-device round trip preserves Average and restores names before dependent room writes', async () => {
  const ui = browser();
  await ui.import(ui.export());
  const sourceIndex = ui.sent.findIndex(message => message.roomMode === 1);
  assert.ok(sourceIndex >= 0, 'Average must be restored explicitly');
  const source = ui.sent[sourceIndex];
  assert.equal(source.roomSingle, 4);
  assert.equal(source.roomMembers, 20);
  assert.ok(ui.sent.every(message => !('roomSource' in message)), 'modern model takes precedence');
  assert.ok(ui.sent.every(message => !('bleAddr' in message)), 'legacy slot-zero write must not overwrite modern slots');
  for (const sensor of original.bleSensors) {
    const identityIndex = ui.sent.findIndex(message => message.bleAddMac === sensor.addr && message.bleAddName === sensor.name);
    assert.ok(identityIndex >= 0 && identityIndex < sourceIndex, 'identity and name precede selection');
    assert.ok(identityIndex < ui.sent.findIndex(message => 'roomOffIdx' in message),
      'adding a new identity can reset offsets, so identities also precede calibration');
  }
  assert.deepEqual(ui.sent.filter(message => 'roomOffIdx' in message).map(message => [message.roomOffIdx, message.roomOffVal]),
    [[0, 0], [1, 6], [2, -2], [3, 0], [4, 8], [5, 0]]);
  assert.ok(ui.sent.some(message => message.betaChannel === true));
  // Thresholds travel with the ranges in the checked config, which goes first.
  assert.equal(ui.sent[0].heatThresh, 20);
  assert.equal(ui.sent[0].coolThresh, 24);
  assert.equal(typeof ui.sent[0].requestId, 'number');
  const night = ui.sent.find(message => 'nightLat' in message);
  assert.deepEqual(night, { cmd: 'set',
    nightLat: 42, nightLon: -71, nightDuskOff: 10, nightDawnOff: -10, nightCeil: 30 });
});

test('restore remaps sparse saved sensor selections and offsets onto a full destination by MAC', async () => {
  const file = browser().export();
  const destination = { ...original, bleSensors: [
    { i: 0, addr: 'AA:BB:CC:DD:EE:10', name: 'Unrelated 0' },
    { i: 1, addr: 'AA:BB:CC:DD:EE:11', name: 'Unrelated 1' },
    { i: 2, addr: 'AA:BB:CC:DD:EE:12', name: 'Unrelated 2' },
    { i: 3, addr: 'AA:BB:CC:DD:EE:02', name: 'Old name' }
  ] };
  const ui = browser(destination);
  await ui.import(file);
  assert.deepEqual(ui.sent.filter(m => 'bleDelIdx' in m).map(m => m.bleDelIdx), [0, 1, 2]);
  const firstAdd = ui.sent.findIndex(m => 'bleAddMac' in m);
  assert.ok(ui.sent.slice(0, firstAdd).filter(m => 'bleDelIdx' in m).length === 3);
  const source = ui.sent.find(m => 'roomMode' in m);
  assert.equal(source.roomMode, 1);
  assert.equal(source.roomSingle, 5);
  assert.equal(source.roomMembers, 36);
  assert.equal(ui.sent.find(m => m.roomOffIdx === 2).roomOffVal, -2);
  assert.equal(ui.sent.find(m => m.roomOffIdx === 5).roomOffVal, 8);
});

test('restoring a sparse list to an empty device maps both selected sensor and average members', async () => {
  const ui = browser({ bleSensors: [] });
  await ui.import(browser().export());
  assert.ok(ui.sent.every(m => !('bleDelIdx' in m)));
  const source = ui.sent.find(m => 'roomMode' in m);
  assert.equal(source.roomSingle, 3);
  assert.equal(source.roomMembers, 12);
  assert.equal(ui.sent.find(m => m.roomOffIdx === 3).roomOffVal, 8);
});

test('four sensors restore through legacy list commands even before a full destination state arrives', async () => {
  const sensors = Array.from({ length: 4 }, (_, i) => ({ i, addr: `AA:BB:CC:DD:EE:0${i}`, name: `Room ${i}` }));
  const ui = browser({});
  await ui.import(browser({ ...original, bleSensors: sensors, roomSingle: 5, roomMembers: 60 }).export());
  assert.deepEqual(ui.sent.filter(m => 'bleDelIdx' in m).map(m => m.bleDelIdx), [0, 1, 2, 3]);
  assert.deepEqual(ui.sent.filter(m => 'bleAddMac' in m).map(m => m.bleAddMac), sensors.map(s => s.addr));
  const source = ui.sent.find(m => 'roomMode' in m);
  assert.equal(source.roomSingle, 5);
  assert.equal(source.roomMembers, 60);
});

test('old version-one files retain legacy fields and restore BLE identity before selection', async () => {
  const ui = browser({ bleSensors: [] });
  await ui.import({ app: 'cn105-homekit-settings', v: 1, settings: {
    deviceName: ' Old device ', roomSource: 1, roomTimeout: 900, heatThresh: 19,
    ble: { enabled: true, addr: 'AA:BB:CC:DD:EE:01', timeout: 600 }
  } });
  const identityIndex = ui.sent.findIndex(m => m.bleAddr === 'AA:BB:CC:DD:EE:01');
  const sourceIndex = ui.sent.findIndex(m => m.roomSource === 1);
  assert.ok(identityIndex >= 0 && identityIndex <= sourceIndex);
  assert.ok(ui.sent.some(m => m.deviceName === 'Old device' && m.bleEnabled === true && m.bleTimeout === 600));
  assert.ok(ui.sent.every(m => !('roomMode' in m) && !('bleDelIdx' in m)));
  assert.equal(ui.sent[0].heatThresh, 19);
  assert.ok(ui.sent.every(m => m.cmd !== 'set'), 'a legacy threshold rides in the checked config');
});

test('disabled night location and empty sensor list restore explicitly without guessing absent old fields', async () => {
  const state = { ...original, nightLat: null, nightLon: null, bleSensors: [], bleAddr: '',
    roomMode: 0, roomSingle: 0, roomMembers: 0, betaChannel: false };
  const ui = browser();
  await ui.import(browser(state).export());
  assert.ok(ui.sent.some(m => m.nightLocClear === 1));
  assert.ok(ui.sent.some(m => m.betaChannel === false));
  assert.deepEqual(ui.sent.filter(m => 'bleDelIdx' in m).map(m => m.bleDelIdx), [0, 2]);
});

test('invalid modern sensor lists and failed sends never report a successful restore', async () => {
  for (const sensors of [
    Array.from({ length: 5 }, (_, i) => ({ i, addr: `AA:BB:CC:DD:EE:0${i}`, name: 'Too many' })),
    [{ i: 0, addr: 'bad', name: 'Invalid' }],
    [{ i: 0, addr: original.bleAddr }, { i: 1, addr: original.bleAddr }]
  ]) {
    const file = browser().export();
    file.settings.bleSensors = sensors;
    const ui = browser();
    await ui.import(file);
    assert.deepEqual(ui.sent, []);
    assert.ok(ui.toasts.some(t => t.error));
  }
  for (const failureAt of [0, 3]) {
    const ui = browser(original, failureAt);
    await ui.import(browser().export());
    assert.equal(ui.sent.length, failureAt, 'stop replay immediately when the transport fails');
    assert.ok(ui.sent.every(m => !('roomMode' in m)), 'do not select an incompletely restored model');
    assert.ok(ui.toasts.some(t => t.error));
    assert.ok(!ui.toasts.some(t => !t.error));
  }
});

test('main Wi-Fi save and button validation preserve leading, trailing and all-whitespace SSIDs', () => {
  for (const ssid of ['', ' Leading', 'Trailing ', ' Both ', '   ', '\t']) {
    const sent = [];
    const elements = { cfgSSID: { value: ssid }, cfgPass: { value: '' }, wifiSaveBtn: {} };
    const context = { window: {}, $: id => elements[id], send: m => { sent.push(m); return true; },
      showToast() {}, wifiPendingSsid: null };
    vm.createContext(context);
    vm.runInContext(handler(index, '  window.saveWifi=function(){') + '\n' +
      handler(index, '  function checkWifiFields(){', '  }'), context);
    context.checkWifiFields();
    assert.equal(elements.wifiSaveBtn.disabled, ssid.length === 0);
    context.window.saveWifi();
    assert.equal(sent.length, ssid.length === 0 ? 0 : 1);
    if (ssid) assert.equal(sent[0].ssid, ssid);
  }
});

test('recovery join preserves SSIDs and requires a password only for the exact secured scan row', () => {
  for (const [ssid, selected, secure, password, disabled] of [
    ['', '', false, '', true], [' Leading ', '', false, '', false],
    ['   ', '', false, '', false], ['\t', '', false, '', false],
    [' Secured ', ' Secured ', true, '', true],
    [' Secured ', ' Secured ', true, 'password', false],
    ['Secured', ' Secured ', true, '', false],
    ['Open', 'Open', false, '', false]
  ]) {
    let submitted;
    const context = { joinBtn: {}, ssidInput: { value: ssid }, passInput: { value: password },
      selSSID: selected, selSecure: secure, setPill() {}, setHdr() {},
      pickerBlock: { style: {} }, window: { scrollTo() {} },
      fetch(url, opts) { submitted = JSON.parse(opts.body); return new Promise(() => {}); } };
    vm.createContext(context);
    vm.runInContext(handler(recovery, '  joinBtn.onclick=function(){') + '\n' +
      handler(recovery, '  function checkFields(){', '  }'), context);
    context.checkFields();
    assert.equal(Boolean(context.joinBtn.disabled), disabled);
    if (!disabled) {
      context.joinBtn.onclick();
      assert.equal(submitted.ssid, ssid);
      assert.equal(submitted.password, password);
    }
  }
});
