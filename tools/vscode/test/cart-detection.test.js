'use strict';

/* Unit tests for the pure cart-detection helpers in extension.js.
 *
 * extension.js does `require('vscode')` at module load, which only resolves
 * inside a running VS Code instance.  We stub that module via a Module._load
 * shim before requiring the extension so the helpers can run under plain node:
 *
 *   node --test tools/vscode/test/
 *
 * Regression coverage for issue #55: the launch path must come from the
 * manifest `id` (`<id>.blyt`), not the project directory basename — the two
 * differ for any cart whose folder name ≠ its declared id. */

const Module = require('node:module');
const test   = require('node:test');
const assert = require('node:assert');
const fs     = require('node:fs');
const os     = require('node:os');
const path   = require('node:path');

/* Stub `require('vscode')` with the minimal surface the helpers touch.
 * detectAnyCart reads vscode.window.activeTextEditor; everything else under
 * test is vscode-free. */
const origLoad = Module._load;
Module._load = function (request, ...rest) {
    if (request === 'vscode') return { window: { activeTextEditor: undefined } };
    return origLoad.call(this, request, ...rest);
};

const { _test } = require('../extension.js');
const { cartId, detectAnyCart } = _test;

/* Build a throwaway cart project directory.  `dirName` is the folder basename,
 * `info` (when provided) is written verbatim as blyt.info.yaml. */
function makeProject(dirName, info) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'blyt-vscode-'));
    const projectDir = path.join(root, dirName);
    fs.mkdirSync(projectDir, { recursive: true });
    if (info !== undefined) {
        fs.writeFileSync(path.join(projectDir, 'blyt.info.yaml'), info);
    }
    return projectDir;
}

test('cartId reads `id` from blyt.info.yaml when dir name differs', () => {
    const dir = makeProject('blyt-dap-retest', 'id: dap_retest\ntitle: T\n');
    assert.strictEqual(cartId(dir), 'dap_retest');
});

test('cartId falls back to basename when manifest is missing', () => {
    const dir = makeProject('hello', undefined);
    assert.strictEqual(cartId(dir), 'hello');
});

test('cartId falls back to basename on unparseable manifest', () => {
    const dir = makeProject('weird', ': : not yaml :');
    assert.strictEqual(cartId(dir), 'weird');
});

test('cartId falls back to basename when `id` is absent', () => {
    const dir = makeProject('noid', 'title: Only A Title\n');
    assert.strictEqual(cartId(dir), 'noid');
});

test('detectAnyCart builds <id>.blyt from the manifest id (issue #55)', () => {
    const dir = makeProject('blyt-dap-retest', 'id: dap_retest\ntitle: T\n');
    const found = detectAnyCart({ uri: { fsPath: dir } });
    assert.ok(found, 'expected a cart to be detected');
    assert.strictEqual(found.projectDir, dir);
    assert.strictEqual(found.cart, path.join(dir, 'build', 'dap_retest.blyt'));
});

test('detectAnyCart falls back to <basename>.blyt without a manifest id', () => {
    const dir = makeProject('hello', 'title: T\n');
    const found = detectAnyCart({ uri: { fsPath: dir } });
    assert.ok(found);
    assert.strictEqual(found.cart, path.join(dir, 'build', 'hello.blyt'));
});

test('detectAnyCart returns null outside any cart project', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'blyt-nocart-'));
    assert.strictEqual(detectAnyCart({ uri: { fsPath: root } }), null);
});
