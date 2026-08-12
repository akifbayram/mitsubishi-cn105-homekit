// Pulls the version-comparison block out of web/index.html and returns it as a
// module. The firmware ships one gzipped HTML blob, so the comparator cannot
// live in its own file; the markers are what make it testable anyway. If the
// markers ever disappear this throws, which is the point.
//
// The evaluated text is a fixed slice of a tracked file in this repo, read at
// test time — it is never network, argv or user input. It still goes through
// vm rather than eval/new Function so it cannot reach into this module's
// scope, and the IIFE wrapper keeps its declarations off the global object.
//
// runInThisContext, not runInNewContext: a fresh context is a fresh realm with
// its own Array.prototype, and assert.deepStrictEqual compares prototypes — so
// cross-realm arrays fail as "same structure but not reference-equal" even when
// every value matches.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML = path.join(__dirname, '..', '..', 'web', 'index.html');
const START = '// ── ver-compare:start';
const END = '// ── ver-compare:end';

function extract() {
  const src = fs.readFileSync(HTML, 'utf8');
  const a = src.indexOf(START);
  const b = src.indexOf(END);
  if (a < 0 || b < 0) throw new Error('ver-compare markers missing from web/index.html');
  if (b < a) throw new Error('ver-compare markers out of order in web/index.html');
  const block = src.slice(a + START.length, b);
  const api = vm.runInThisContext(
    '(function(){' + block + '\nreturn {normVer:normVer,parseVer:parseVer,cmpVer:cmpVer};})',
    { filename: 'web/index.html#ver-compare' }
  )();
  for (const name of ['normVer', 'parseVer', 'cmpVer']) {
    if (typeof api[name] !== 'function')
      throw new Error(`ver-compare block does not define ${name}()`);
  }
  return api;
}

module.exports = extract();
