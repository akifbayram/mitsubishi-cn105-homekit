// Pulls the parts of web/index.html this suite exercises out as a module: the
// C↔F setpoint table, the slider reconfiguration that runs on every unit
// change, the °C/°F button handler, and the state-frame merge. The firmware
// ships one gzipped HTML blob, so none of this can live in its own file; the
// markers are what make it testable anyway. If a marker ever disappears this
// throws, which is the point.
//
// The evaluated text is a fixed slice of a tracked file in this repo, read at
// test time — it is never network, argv or user input. It still goes through
// vm rather than eval/new Function so it cannot reach into this module's
// scope, and the wrapper keeps its declarations off the global object.
//
// runInThisContext, not runInNewContext: a fresh context is a fresh realm with
// its own Array.prototype, and assert.deepStrictEqual compares prototypes — so
// cross-realm arrays fail as "same structure but not reference-equal" even when
// every value matches.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML = path.join(__dirname, '..', '..', 'web', 'index.html');
const src = fs.readFileSync(HTML, 'utf8');

function block(name) {
  const start = `// ── ${name}:start`;
  const end = `// ── ${name}:end`;
  const a = src.indexOf(start);
  const b = src.indexOf(end);
  if (a < 0 || b < 0) throw new Error(`${name} markers missing from web/index.html`);
  if (b < a) throw new Error(`${name} markers out of order in web/index.html`);
  return src.slice(a + start.length, b);
}

// The shipped <input type="range"> attributes, so the suite starts the sliders
// exactly where a freshly loaded page does instead of at invented numbers.
function sliderAttrs(id) {
  const tag = new RegExp(`<input[^>]*\\bid="${id}"[^>]*>`).exec(src);
  if (!tag) throw new Error(`no <input id="${id}"> in web/index.html`);
  const attr = (n) => {
    const m = new RegExp(`\\b${n}="([^"]*)"`).exec(tag[0]);
    if (!m) throw new Error(`<input id="${id}"> has no ${n}`);
    return m[1];
  };
  return { min: attr('min'), max: attr('max'), step: attr('step'), value: attr('value') };
}

const DEFAULTS = {
  target: sliderAttrs('tempSlider'),
  heat: sliderAttrs('heatSlider'),
  cool: sliderAttrs('coolSlider')
};

// A range input keeps min/max/step/value as STRINGS, and configureSliders()
// leans on that with a loose `min==61`. Coerce on set so a tightening to `===`
// fails here instead of in a browser. Clamping and step snapping are not
// modelled: setSliderAttrs() always writes min/max/step before value, and every
// value it writes sits inside the range it just set.
function makeInput(a) {
  const el = {};
  ['min', 'max', 'step', 'value'].forEach(function (p) {
    let v = String(a[p]);
    Object.defineProperty(el, p, {
      get: function () { return v; },
      set: function (x) { v = String(x); },
      enumerable: true
    });
  });
  return el;
}

// One wrapper per instance so each case gets its own sliders and unit.
const factory = vm.runInThisContext(
  '(function(makeInput,defaults){return function(unit){\n' +
  '  var tempUnit=unit;\n' +
  '  var tempSlider=makeInput(defaults.target);\n' +
  '  var heatSlider=makeInput(defaults.heat);\n' +
  '  var coolSlider=makeInput(defaults.cool);\n' +
  '  var state={},lastCmdTime=0,sent=[],window={};\n' +
  '  function paintTempSlider(){}\n' +      // canvas fill, nothing to assert on
  '  function setActive(){}\n' +
  '  function renderDual(){}\n' +
  '  function send(m){sent.push(m);return true}\n' +
  block('temp-unit') + '\n' +
  block('slider-units') + '\n' +
  block('unit-tap') + '\n' +
  '  return {target:tempSlider,heat:heatSlider,cool:coolSlider,\n' +
  '    unit:function(){return tempUnit},\n' +
  '    cmdArmed:function(){return lastCmdTime!==0},\n' +
  '    sent:sent,\n' +
  '    configureSliders:configureSliders,\n' +
  '    cToFTable:cToFTable,fToCTable:fToCTable,\n' +
  '    tap:function(u){window.setTempUnit({getAttribute:function(){return u}})}};\n' +
  '};})',
  { filename: 'web/index.html#temp-unit' }
)(makeInput, DEFAULTS);

const mergeState = vm.runInThisContext(
  '(function(){' + block('state-merge') + '\nreturn mergeState;})',
  { filename: 'web/index.html#state-merge' }
)();

// The Room Sensor writes, with their optimistic repaint stubbed out: what
// matters here is what reaches the wire when lastState is short a field.
const roomFactory = vm.runInThisContext(
  '(function(){return function(st){\n' +
  '  var lastState=st,sent=[],toasts=[];\n' +
  '  function renderRoomCard(){}\n' +
  '  function showToast(t){toasts.push(t)}\n' +
  '  function send(m){sent.push(m);return true}\n' +
  '  var window={};\n' +
  block('room-writes') + '\n' +
  '  return {sent:sent,toasts:toasts,state:function(){return lastState},\n' +
  '    setRoomMode:window.setRoomMode,selectRoomSingle:window.selectRoomSingle,\n' +
  '    toggleRoomMember:window.toggleRoomMember};\n' +
  '};})',
  { filename: 'web/index.html#room-writes' }
)();

const probe = factory('C');
for (const name of ['configureSliders', 'cToFTable', 'fToCTable', 'tap']) {
  if (typeof probe[name] !== 'function')
    throw new Error(`extracted blocks do not define ${name}()`);
}
const roomProbe = roomFactory({});
for (const name of ['setRoomMode', 'selectRoomSingle', 'toggleRoomMember']) {
  if (typeof roomProbe[name] !== 'function')
    throw new Error(`room-writes block does not define ${name}()`);
}
if (typeof mergeState !== 'function')
  throw new Error('state-merge block does not define mergeState()');

module.exports = { newUi: factory, newRoom: roomFactory, mergeState, DEFAULTS };
