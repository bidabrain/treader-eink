// 验证器导入页解析函数的测试：从 main/apps/auth_form.html 取出 parsers 段运行。
// Tests for the authenticator import page parsers, extracted from main/apps/auth_form.html.
//
// 运行 / Run:  docker run --rm -v "$PWD:/w" -w /w node:22 node tools/auth_form_test.mjs
// （本机有 Node 18+ 时也可以直接 node tools/auth_form_test.mjs。/ Plain `node` 18+ works too.)

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const html = readFileSync(fileURLToPath(new URL('../main/apps/auth_form.html', import.meta.url)), 'utf8');
const b = html.indexOf('// ---- parsers begin ----');
const e = html.indexOf('// ---- parsers end ----');
assert.ok(b > 0 && e > b, 'parser markers not found');
const P = new Function(html.slice(b, e) + `
  return { b32encode, b32norm, base32Valid, utf8Len, parseOtpauth, parseMigration, parseLines, missingBatches, accountKey };`)();

let n = 0;
function t(name, fn) { fn(); n++; console.log('ok', name); }

/* ---- 测试用 protobuf 写入器 / Tiny protobuf writer for fixtures ---- */

function varint(v) {
  const out = [];
  let x = BigInt.asUintN(64, BigInt(v));
  do { let c = Number(x & 0x7fn); x >>= 7n; if (x) c |= 0x80; out.push(c); } while (x);
  return out;
}
const tagv = (f, w) => varint(f * 8 + w);
const fVar = (f, v) => [...tagv(f, 0), ...varint(v)];
const fBytes = (f, bytes) => [...tagv(f, 2), ...varint(bytes.length), ...bytes];
const fStr = (f, s) => fBytes(f, [...new TextEncoder().encode(s)]);
const fFixed32 = (f) => [...tagv(f, 5), 1, 2, 3, 4];
const fFixed64 = (f) => [...tagv(f, 1), 1, 2, 3, 4, 5, 6, 7, 8];

function otp({ secret, name, issuer, algo, digits, type, counter }) {
  const o = [...fBytes(1, [...secret]), ...fStr(2, name)];
  if (issuer != null) o.push(...fStr(3, issuer));
  if (algo != null) o.push(...fVar(4, algo));
  if (digits != null) o.push(...fVar(5, digits));
  if (type != null) o.push(...fVar(6, type));
  if (counter != null) o.push(...fVar(7, counter));
  return o;
}
function migrationUri(otps, extra = []) {
  const payload = Uint8Array.from([...otps.flatMap((o) => fBytes(1, o)), ...extra]);
  const b64 = Buffer.from(payload).toString('base64');
  return { b64, uri: 'otpauth-migration://offline?data=' + encodeURIComponent(b64) };
}
const ascii = (s) => Buffer.from(s, 'latin1');

/* ---- base32 ---- */

t('base32 encode known vectors', () => {
  assert.equal(P.b32encode(ascii('Hello!')), 'JBSWY3DPEE');
  assert.equal(P.b32encode(ascii('12345678901234567890')), 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ');
  assert.equal(P.b32encode(ascii('f')), 'MY');
  assert.equal(P.b32encode(ascii('foobar')), 'MZXW6YTBOI');
  assert.equal(P.b32encode(new Uint8Array(0)), '');
});

t('base32Valid', () => {
  assert.ok(P.base32Valid('JBSWY3DPEE'));
  assert.ok(P.base32Valid('jbsw y3dp ee=='));
  assert.ok(P.base32Valid('GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'));
  assert.ok(!P.base32Valid(''));
  assert.ok(!P.base32Valid('A'));            // 0 字节。/ Zero bytes.
  assert.ok(!P.base32Valid('JBSWY3DPEE1'));  // '1' 不在字母表。/ Not in alphabet.
  assert.ok(!P.base32Valid('JBSW-Y3DP'));
  assert.ok(!P.base32Valid('JBSWY3DPE8'));
  assert.ok(P.base32Valid('A'.repeat(103)));  // 64 字节。/ 64 bytes.
  assert.ok(!P.base32Valid('A'.repeat(104))); // 65 字节。/ 65 bytes.
});

/* ---- otpauth:// ---- */

t('otpauth: RFC-style with issuer param', () => {
  const a = P.parseOtpauth('otpauth://totp/Example:alice@google.com?secret=JBSWY3DPEHPK3PXP&issuer=Example');
  assert.deepEqual({ ...a }, { issuer: 'Example', name: 'alice@google.com', secret: 'JBSWY3DPEHPK3PXP', algo: 'SHA1', digits: 6, period: 30, unsupported: '', notes: [] });
});

t('otpauth: %3A label, %20 and no issuer param', () => {
  const a = P.parseOtpauth('otpauth://totp/ACME%20Co%3Ajohn.doe%40email.com?secret=HXDMVJECJJWSRB3HWIZR4IFUGFTMXBOZ&algorithm=SHA256&digits=8&period=60');
  assert.equal(a.issuer, 'ACME Co');
  assert.equal(a.name, 'john.doe@email.com');
  assert.equal(a.algo, 'SHA256');
  assert.equal(a.digits, 8);
  assert.equal(a.period, 60);
  assert.equal(a.unsupported, '');
});

t('otpauth: issuer param wins over label prefix', () => {
  const a = P.parseOtpauth('otpauth://totp/Old%3A%20bob?issuer=New%20Inc&secret=JBSWY3DPEE');
  assert.equal(a.issuer, 'New Inc');
  assert.equal(a.name, 'bob');
});

t('otpauth: lowercase spaced secret normalised', () => {
  const a = P.parseOtpauth('otpauth://totp/bob?secret=jbsw%20y3dp%20ehpk%203pxp%3D%3D');
  assert.equal(a.secret, 'JBSWY3DPEHPK3PXP');
  assert.equal(a.issuer, '');
  assert.equal(a.name, 'bob');
  assert.equal(a.unsupported, '');
});

t('otpauth: uppercase scheme, SHA-512 spelling, Chinese label', () => {
  const a = P.parseOtpauth('OTPAUTH://TOTP/' + encodeURIComponent('银行:张三') + '?SECRET=JBSWY3DPEE&Algorithm=sha512');
  assert.equal(a.issuer, '银行');
  assert.equal(a.name, '张三');
  assert.equal(a.algo, 'SHA512');
});

t('otpauth: hotp rejected', () => {
  const a = P.parseOtpauth('otpauth://hotp/Foo:bar?secret=JBSWY3DPEE&counter=5');
  assert.equal(a.unsupported, '计数器型（HOTP）不支持');
});

t('otpauth: bad params rejected', () => {
  assert.match(P.parseOtpauth('otpauth://totp/x?secret=JBSWY3DPEE&digits=7').unsupported, /位/);
  assert.match(P.parseOtpauth('otpauth://totp/x?secret=JBSWY3DPEE&period=45').unsupported, /周期/);
  assert.match(P.parseOtpauth('otpauth://totp/x?secret=JBSWY3DPEE&algorithm=MD5').unsupported, /MD5/);
  assert.equal(P.parseOtpauth('otpauth://totp/x?secret=JBSW1').unsupported, '密钥无效');
  assert.equal(P.parseOtpauth('otpauth://totp/x').unsupported, '密钥无效');
  assert.throws(() => P.parseOtpauth('https://example.com'));
});

t('otpauth: name fallback and truncation', () => {
  assert.equal(P.parseOtpauth('otpauth://totp/?secret=JBSWY3DPEE&issuer=Only').name, 'Only');
  assert.equal(P.parseOtpauth('otpauth://totp/?secret=JBSWY3DPEE').name, '未命名');
  const long = '验'.repeat(30); // 90 字节。/ 90 bytes.
  const a = P.parseOtpauth('otpauth://totp/' + encodeURIComponent(long) + '?secret=JBSWY3DPEE');
  assert.equal(a.name, '验'.repeat(21));
  assert.ok(P.utf8Len(a.name) <= 64);
  assert.equal(a.notes.length, 1);
});

/* ---- otpauth-migration:// ---- */

t('migration: 3 accounts, SHA256/8, HOTP, batch fields, unknown fields', () => {
  const s1 = ascii('12345678901234567890');
  const s2 = Uint8Array.from([0xfb, 0xff, 0xbf, 0xfe, 0x3e, 0xfc, 0x00, 0x01, 0x02, 0x03]);
  const s3 = ascii('Hello!');
  const { b64, uri } = migrationUri([
    otp({ secret: s1, name: 'GitHub:octo', issuer: 'GitHub', algo: 1, digits: 1, type: 2 }),
    [...otp({ secret: s2, name: 'me@例子.com', issuer: '例子', algo: 2, digits: 2, type: 2 }), ...fFixed32(9), ...fFixed64(10), ...fStr(11, 'x')],
    otp({ secret: s3, name: 'counter', issuer: 'Old', algo: 0, digits: 0, type: 1, counter: 7 }),
  ], [...fVar(2, 1), ...fVar(3, 3), ...fVar(4, 1), ...fVar(5, -123456), ...fFixed32(15), ...fVar(6, 1)]);
  // 夹具确实带了需要 URL 编码的字符。/ Make sure the fixture exercises URL-encoded base64.
  assert.ok(/[+/]/.test(b64) && b64.endsWith('='), 'fixture lacks + / = : ' + b64);
  assert.ok(/%2B|%2F/.test(uri) && /%3D/.test(uri));

  const r = P.parseMigration(uri);
  assert.equal(r.batch_size, 3);
  assert.equal(r.batch_index, 1);
  assert.equal(r.version, 1);
  assert.equal(r.accounts.length, 3);
  const [a, b2, c] = r.accounts;
  assert.deepEqual([a.issuer, a.name, a.secret, a.algo, a.digits, a.period, a.unsupported], ['GitHub', 'octo', 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ', 'SHA1', 6, 30, '']);
  assert.deepEqual([b2.issuer, b2.name, b2.secret, b2.algo, b2.digits, b2.unsupported], ['例子', 'me@例子.com', P.b32encode(s2), 'SHA256', 8, '']);
  assert.equal(c.secret, 'JBSWY3DPEE');
  assert.equal(c.unsupported, '计数器型（HOTP）不支持');

  // 未编码、空格替代 '+'、base64url 都能读。/ Raw, '+'-as-space and base64url forms also parse.
  for (const v of [b64, b64.replace(/\+/g, ' '), b64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')]) {
    const r2 = P.parseMigration('otpauth-migration://offline?data=' + v);
    assert.equal(r2.accounts[1].secret, P.b32encode(s2));
  }
});

t('migration: MD5 and leading issuer stripped case-insensitively, empty issuer split', () => {
  const { uri } = migrationUri([
    otp({ secret: ascii('Hello!'), name: 'example: alice', issuer: 'Example' }),
    otp({ secret: ascii('Hello!'), name: 'x', issuer: 'Y', algo: 4 }),
    otp({ secret: ascii('Hello!'), name: 'Corp:bob' }),
  ]);
  const r = P.parseMigration(uri);
  assert.equal(r.batch_size, 1);
  assert.equal(r.accounts[0].name, 'alice');
  assert.equal(r.accounts[0].issuer, 'Example');
  assert.equal(r.accounts[1].unsupported, '算法 MD5 不支持');
  assert.deepEqual([r.accounts[2].issuer, r.accounts[2].name], ['Corp', 'bob']);
});

t('migration: errors', () => {
  assert.throws(() => P.parseMigration('otpauth-migration://offline?foo=1'), /没有数据/);
  assert.throws(() => P.parseMigration('otpauth-migration://offline?data=' + Buffer.from([0x0a, 0x50, 1]).toString('base64')), /不完整/);
  assert.throws(() => P.parseMigration('otpauth-migration://offline?data=' + Buffer.from([0x0b]).toString('base64')), /损坏/);
  assert.throws(() => P.parseMigration('otpauth-migration://offline?data=' + Buffer.from(fVar(2, 1)).toString('base64')), /没有账号/);
});

/* ---- 多行与分批 / Lines and batches ---- */

t('parseLines mixes both kinds and reports bad lines', () => {
  const m0 = migrationUri([otp({ secret: ascii('Hello!'), name: 'a', issuer: 'I' })], [...fVar(3, 3), ...fVar(4, 0), ...fVar(5, 77)]).uri;
  const m2 = migrationUri([otp({ secret: ascii('Hello!'), name: 'b', issuer: 'I' })], [...fVar(3, 3), ...fVar(4, 2), ...fVar(5, 77)]).uri;
  const text = [m0, '', '  otpauth://totp/X:y?secret=JBSWY3DPEE  ', 'hello', m2].join('\r\n');
  const r = P.parseLines(text);
  assert.equal(r.accounts.length, 3);
  assert.equal(r.errors.length, 1);
  assert.equal(r.errors[0].line, 4);
  assert.equal(r.batches.length, 2);
  assert.deepEqual(P.missingBatches(r.batches), [{ batch_id: 77, size: 3, missing: [2] }]);
  assert.deepEqual(P.missingBatches([{ batch_size: 1, batch_index: 0, batch_id: 5 }]), []);
});

t('dedupe key', () => {
  const a = P.parseOtpauth('otpauth://totp/I:n?secret=jbswy3dpee');
  const b = P.parseOtpauth('otpauth://totp/n?issuer=I&secret=JBSW%20Y3DP%20EE');
  assert.equal(P.accountKey(a), P.accountKey(b));
});

console.log(`\n${n} tests passed`);
