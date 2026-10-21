const assert = require('assert');
const { readVersions, menuModel } = require('./extract.js');

const URL = 'https://h/x/firmware/homekit/versions.json';
const sha = c => c.repeat(64);
const build = (ver, board, c) => ({
  path: 'archive/' + ver + '/' + board + '/firmware.bin', sha256: sha(c)
});
// The real index shape: a row's builds is an object keyed by board name, and
// the row carries a release_url that Entry must not pick up.
const rel = (ver, builds, date) => ({
  version: ver, date: date || '2026-01-01',
  release_url: 'https://github.com/o/r/releases/tag/v' + ver, builds
});

// Scrambled on purpose: readVersions owns the ordering.
const INDEX = {
  name: 'Mitsubishi HomeKit Controller',
  versions: [
    rel('0.2.7', { nanoc6: build('0.2.7', 'nanoc6', 'a') }),
    rel('0.2.9-beta.1', { nanoc6: build('0.2.9-beta.1', 'nanoc6', 'b') }),
    rel('0.2', { nanoc6: build('0.2', 'nanoc6', 'c') }),
    rel('0.2.8', { 'm5atoms3-lite': build('0.2.8', 'm5atoms3-lite', 'e'),
                   nanoc6: build('0.2.8', 'nanoc6', 'd') }),
    rel('0.2.6-beta.4', { nanoc6: build('0.2.6-beta.4', 'nanoc6', 'f') }),
    rel('0.2.5', { 'm5atoms3-lite': build('0.2.5', 'm5atoms3-lite', '1') }),   // no nanoc6 build
    rel('0.2.4', { nanoc6: { path: 'archive/0.2.4/nanoc6/firmware.bin', sha256: 'zz' } })
  ]
};

const list = readVersions(INDEX, URL, 'nanoc6');
const v = rows => rows.map(r => r.entry.version);
assert.deepStrictEqual(list.map(e => e.version), ['0.2.9-beta.1', '0.2.8', '0.2.7', '0.2.6-beta.4', '0.2']);
assert.strictEqual(list[1].binUrl, 'https://h/x/firmware/homekit/archive/0.2.8/nanoc6/firmware.bin');
[null, {}, {versions: 'x'}].forEach(bad => assert.deepStrictEqual(readVersions(bad, URL, 'nanoc6'), []));

// An Entry is exactly {version, date, binUrl, sha256}: no notes, no release_url.
assert.deepStrictEqual(list[1], {
  version: '0.2.8',
  date: '2026-01-01',
  binUrl: 'https://h/x/firmware/homekit/archive/0.2.8/nanoc6/firmware.bin',
  sha256: sha('d')
});
assert.strictEqual(readVersions(INDEX, URL, 'm5atoms3-lite')[1].version, '0.2.5');
assert.deepStrictEqual(readVersions(INDEX, URL, 'esp32'), []);

// Defensive skips: bad version, missing path or sha256, junk rows, builds that
// is an array (the manifests' shape, not the index's), null, or a string.
assert.deepStrictEqual(readVersions({versions: [
  rel('latest', { nanoc6: build('latest', 'nanoc6', 'a') }),
  rel('1.0.0', { nanoc6: { sha256: sha('a') } }),
  rel('1.0.1', { nanoc6: { path: 'archive/1.0.1/nanoc6/firmware.bin' } }),
  rel('1.0.2', { nanoc6: null }),
  rel('1.0.3', 'nope'),
  rel('1.0.4', null),
  rel('1.0.5', [{ board: 'nanoc6', path: 'archive/1.0.5/nanoc6/firmware.bin', sha256: sha('a') }]),
  rel('1.0.6', { 'm5atoms3-lite': build('1.0.6', 'm5atoms3-lite', 'a') }),   // other board only
  null, 7, {},
  rel('1.0.9', { nanoc6: build('1.0.9', 'nanoc6', 'a') })
]}, URL, 'nanoc6').map(e => e.version), ['1.0.9']);
assert.deepStrictEqual(readVersions({versions: [
  rel('1.0.0', { nanoc6: build('1.0.0', 'nanoc6', 'a') })
]}, URL, 'm5atoms3-lite'), []);

// Board lookup is by own property: a board named like an Object.prototype member
// never matches, even when the prototype is polluted with a path and a sha256.
['constructor', '__proto__', 'toString', 'hasOwnProperty'].forEach(b =>
  assert.deepStrictEqual(readVersions(INDEX, URL, b), []));
Object.defineProperty(Object.prototype, 'path', { value: 'evil', configurable: true });
Object.defineProperty(Object.prototype, 'sha256', { value: sha('9'), configurable: true });
try {
  assert.deepStrictEqual(readVersions(INDEX, URL, '__proto__'), []);
  assert.deepStrictEqual(readVersions(INDEX, URL, 'constructor'), []);
} finally {
  delete Object.prototype.path;
  delete Object.prototype.sha256;
}

let m = menuModel(list, '0.2.6-beta.4', false);
assert.strictEqual(m.offer.version, '0.2.8');
assert.deepStrictEqual(v(m.pinned), ['0.2.8', '0.2.6-beta.4']);
assert.deepStrictEqual(m.pinned.map(r => r.tags), [['latest'], ['beta', 'installed']]);
assert.deepStrictEqual(v(m.others), ['0.2.9-beta.1', '0.2.7', '0.2']);

m = menuModel(list, 'v0.2.8', false);
assert.strictEqual(m.offer, null);
assert.deepStrictEqual(m.pinned.map(r => r.tags), [['latest', 'installed']]);

m = menuModel(list, '0.2.8', true);
assert.strictEqual(m.offer.version, '0.2.9-beta.1');
assert.deepStrictEqual(v(m.pinned), ['0.2.9-beta.1', '0.2.8']);

m = menuModel(list, '0.2.9-beta.1', false);                 // leaving a beta: stable is pinned, nothing offered
assert.strictEqual(m.offer, null);
assert.deepStrictEqual(v(m.pinned), ['0.2.8', '0.2.9-beta.1']);

m = menuModel(list, '0.2.8-3-gabc1234', false);              // dev build: no orange at equal, no installed tag
assert.strictEqual(m.offer, null);
assert.ok(m.pinned.concat(m.others).every(r => !r.installed));
assert.strictEqual(menuModel(list, '0.2.7-5-gabc1234-dirty', false).offer.version, '0.2.8');

m = menuModel(list, '0.3.0', false);                         // running a version the index doesn't know
assert.strictEqual(m.offer, null);
assert.deepStrictEqual(v(m.pinned), ['0.2.8']);
assert.deepStrictEqual(menuModel([], '0.2.8', true), {pinned: [], others: [], offer: null});

console.log('PASS: version menu model');
