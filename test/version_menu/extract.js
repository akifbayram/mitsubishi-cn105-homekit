// Pulls the version-menu block out of web/index.html together with the
// ver-compare block it depends on (normVer/parseVer/cmpVer), and returns the
// menu's two functions. Same approach as ../version_compare/extract.js: the
// firmware ships one gzipped HTML blob, so the markers are what make the
// inline script testable. If either pair disappears this throws.
//
// The evaluated text is a fixed slice of a tracked file in this repo, read at
// test time — never network, argv or user input. vm.runInThisContext (not a
// new context, so assert.deepStrictEqual sees this realm's Array.prototype),
// inside one IIFE so both blocks share a scope and stay off the global object.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML = path.join(__dirname, '..', '..', 'web', 'index.html');

function slice(src, name) {
  const START = '// ── ' + name + ':start';
  const END = '// ── ' + name + ':end';
  const a = src.indexOf(START);
  const b = src.indexOf(END);
  if (a < 0 || b < 0) throw new Error(name + ' markers missing from web/index.html');
  if (b < a) throw new Error(name + ' markers out of order in web/index.html');
  return src.slice(a + START.length, b);
}

function extract() {
  const src = fs.readFileSync(HTML, 'utf8');
  const cmp = slice(src, 'ver-compare');
  const menu = slice(src, 'ver-menu');
  const api = vm.runInThisContext(
    '(function(){' + cmp + '\n' + menu +
    '\nreturn {readVersions:readVersions,menuModel:menuModel};})',
    { filename: 'web/index.html#ver-menu' }
  )();
  for (const name of ['readVersions', 'menuModel']) {
    if (typeof api[name] !== 'function')
      throw new Error(`ver-menu block does not define ${name}()`);
  }
  return api;
}

module.exports = extract();
