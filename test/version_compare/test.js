const assert = require('assert');
const { parseVer, cmpVer, normVer } = require('./extract.js');

// [a, b, expected] — expected is cmpVer(a, b).
// Duplicated verbatim in serin-labs.github.io/tests/version-compare.test.js.
const VECTORS = [
  ['0.2.5', '0.2.6', -1],
  ['0.2.6', '0.2.5', 1],
  ['0.2.5', '0.2.5', 0],
  ['v0.2.5', '0.2.5', 0],
  ['0.2.5', '0.2.5.0', 0],
  ['2026.7.1', '2026.7.2', -1],
  // A prerelease sorts below the release that carries the same core.
  ['0.2.6-beta.1', '0.2.6', -1],
  ['0.2.6', '0.2.6-beta.1', 1],
  ['0.2.5', '0.2.6-beta.1', -1],
  // Numeric identifiers compare numerically, not lexically.
  ['0.2.6-beta.2', '0.2.6-beta.10', -1],
  ['0.2.6-beta.1', '0.2.6-beta.2', -1],
  // Alphanumeric identifiers compare lexically; numeric sorts before them.
  ['0.2.6-beta.1', '0.2.6-rc.1', -1],
  ['0.2.6-1', '0.2.6-beta', -1],
  // Fewer identifiers sorts lower.
  ['0.2.6-beta', '0.2.6-beta.1', -1],
  // A git-describe tail never changes the ordering, only the dev flag.
  ['0.2.6-beta.1-3-gabc1234', '0.2.6-beta.1', 0],
  ['0.2.5-7-gabc1234', '0.2.5', 0]
];

VECTORS.forEach(([a, b, want]) => {
  assert.strictEqual(cmpVer(a, b), want, `cmpVer(${a}, ${b})`);
  // `want === 0 ? 0 : -want`, never a bare `-want`: assert.strictEqual uses
  // Object.is, and Object.is(0, -0) is false.
  assert.strictEqual(cmpVer(b, a), want === 0 ? 0 : -want,
    `cmpVer(${b}, ${a}) (antisymmetry)`);
});

// dev is what keeps a build between tags from ever claiming "up to date".
assert.strictEqual(parseVer('v0.2.5').dev, false);
assert.strictEqual(parseVer('0.2.6-beta.1').dev, false);
assert.strictEqual(parseVer('v0.2.5-7-gabc1234').dev, true);
assert.strictEqual(parseVer('v0.2.5-7-gabc1234-dirty').dev, true);
assert.strictEqual(parseVer('0.2.6-beta.1-dirty').dev, true);
assert.strictEqual(parseVer('0.2.6-beta.1-3-gabc1234').dev, true);

// The parse itself, so a refactor cannot quietly change what pre means.
assert.deepStrictEqual(parseVer('0.2.6-beta.1').core, [0, 2, 6]);
assert.deepStrictEqual(parseVer('0.2.6-beta.1').pre, ['beta', 1]);
assert.strictEqual(parseVer('0.2.6').pre, null);
assert.deepStrictEqual(parseVer('0.2.6-beta.1-3-gabc1234').pre, ['beta', 1]);

// The fallback PROJECT_VER when git describe cannot run (scripts/project_ver.py).
// It must sort below every real release so any found build stays installable.
assert.strictEqual(cmpVer('0.0.0-dev', '0.2.5'), -1);

assert.strictEqual(normVer('v0.2.5'), '0.2.5');
assert.strictEqual(normVer('0.2.5'), '0.2.5');

console.log('PASS: version comparison (' + VECTORS.length + ' vectors)');
