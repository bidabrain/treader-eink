// webtoon 面板脚本：无构建、无第三方库。/ webtoon panel script: no build step, no third-party libraries.
const $ = (s) => document.querySelector(s);
const $$ = (s) => document.querySelectorAll(s);

async function api(path, opts = {}) {
  const init = { method: opts.method || 'GET', headers: {} };
  if (opts.body !== undefined) { init.headers['Content-Type'] = 'application/json'; init.body = JSON.stringify(opts.body); }
  const r = await fetch(path, init);
  let data = null;
  try { data = await r.json(); } catch (_) {}
  if (r.status === 401) { showGate(); throw new Error('请先登录面板'); }
  if (!r.ok) throw new Error((data && (data.detail || data.error)) || ('HTTP ' + r.status));
  return data;
}
const when = (t) => t ? new Date(t * 1000).toLocaleString() : '—';
const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---- 进入 / Gate ----
let setupMode = false;
async function boot() {
  const s = await api('/api/panel/state');
  $('#ver').textContent = ' v' + s.version;
  if (!s.signed_in) return showGate(s.setup);
  $('#gate').hidden = true; $('#app').hidden = false;
  show('home');
  loadSources().catch((e) => console.warn(e));
}
function showGate(setup) {
  setupMode = !!setup;
  $('#app').hidden = true; $('#gate').hidden = false;
  $('#gateHint').textContent = setup ? '第一次使用，请设置面板密码（至少 8 位）。' : '输入面板密码。';
  $('#gatePassword').autocomplete = setup ? 'new-password' : 'current-password';
}
$('#gateForm').onsubmit = async (e) => {
  e.preventDefault();
  try {
    await api(setupMode ? '/api/panel/setup' : '/api/panel/login', { method: 'POST', body: { password: $('#gatePassword').value } });
    $('#gatePassword').value = ''; $('#gateMsg').textContent = ''; boot();
  } catch (err) { $('#gateMsg').textContent = err.message; }
};
$('#logout').onclick = async () => { await api('/api/panel/logout', { method: 'POST' }); location.reload(); };

// ---- 页签 / Tabs ----
const loaders = {};
function show(tab) {
  $$('header nav a').forEach((a) => a.classList.toggle('on', a.dataset.tab === tab));
  $$('main section').forEach((s) => { s.hidden = s.dataset.panel !== tab; });
  if (loaders[tab]) loaders[tab]().catch((e) => console.warn(e));
}
$$('header nav a').forEach((a) => { a.onclick = () => show(a.dataset.tab); });

// ---- 概览 / Home ----
loaders.home = async () => {
  const s = await api('/api/panel/status');
  const items = [['缓存', s.cache_mb + ' MB'], ['排队中', s.queue], ['外网', s.tunnel.status]];
  $('#stats').innerHTML = items.map(([k, v]) => `<div class="stat"><span class="muted">${k}</span><b>${esc(v)}</b></div>`).join('');
  $('#recent').innerHTML = s.recent.length ? s.recent.map((r) =>
    `<li><span>${esc(r.title || r.id)} · 第 ${r.no} 话 <span class="muted">${esc(r.lang)}</span></span><span class="muted">${esc(r.note)} · ${r.sec} 秒 · ${when(r.at)}</span></li>`).join('')
    : '<li class="muted">还没有转过</li>';
};

// ---- 发现页 / Store ----
const LANGS = { 'zh-hant': '繁體中文', en: 'English', es: 'Español', fr: 'Français', de: 'Deutsch', th: 'ภาษาไทย', id: 'Indonesia' };
const SEGS = [['rank', '排行'], ['genre', '类型'], ['day', '星期'], ['done', '完结']];
const MSEGS = [['pop', '人气'], ['latest', '最新']];
let seg = 'rank', key = '', srcs = [];
const src = () => $('#src').value || 'webtoon';
const isWt = () => src() === 'webtoon';
async function loadSources() {
  const r = await api('/api/panel/mihon');
  srcs = r.items.filter((s) => !s.hidden);
  $('#mihonTab').hidden = !r.mihon;
  $('#src').hidden = srcs.length < 2;
  const cur = src();
  $('#src').innerHTML = srcs.map((s) => `<option value="${esc(s.key)}">${esc(s.name)}${s.lang ? ' · ' + esc(s.lang) : ''}</option>`).join('');
  if (srcs.some((s) => s.key === cur)) $('#src').value = cur;
  return r;
}
$('#lang').innerHTML = Object.entries(LANGS).map(([k, v]) => `<option value="${k}">${v}</option>`).join('');
const lang = () => $('#lang').value;
function chips(el, list, cur, pick) {
  el.innerHTML = list.map(([k, v]) => `<a class="chip${k === cur ? ' on' : ''}" data-k="${esc(k)}">${esc(v)}</a>`).join('');
  el.querySelectorAll('a').forEach((a) => { a.onclick = () => pick(a.dataset.k); });
}
function covers(items) {
  $('#covers').innerHTML = items.map((c) => `<figure data-id="${esc(c.id)}"><img loading="lazy" src="/api/panel/cover/${esc(c.id)}.jpg?lang=${lang()}" alt="">
    <figcaption>${esc(c.title)}${c.canvas ? ' <span class="muted">Canvas</span>' : ''}<br><span class="muted">${esc(c.genre || c.author)}</span></figcaption></figure>`).join('');
  $$('#covers figure').forEach((f) => { f.onclick = () => openTitle(f.dataset.id); });
}
async function loadStore() {
  $('#lang').hidden = !isWt();
  const segs = isWt() ? SEGS : MSEGS.filter(([k]) => k === 'pop' || (srcs.find((s) => s.key === src()) || {}).latest);
  if (!segs.some(([k]) => k === seg)) seg = segs[0][0];
  chips($('#segs'), segs, seg, (k) => { seg = k; key = ''; loadStore(); });
  $('#storeMsg').textContent = '加载中…';
  try {
    const r = await api(`/api/panel/store?lang=${lang()}&seg=${seg}&key=${encodeURIComponent(key)}&src=${encodeURIComponent(src())}`);
    key = r.key;
    chips($('#keys'), r.keys.map((k) => [k.key, k.name]), key, (k) => { key = k; loadStore(); });
    $('#storeMsg').textContent = `${r.items.length} 部`;
    covers(r.items);
  } catch (e) { $('#storeMsg').textContent = e.message; }
}
loaders.store = loadStore;
$('#lang').onchange = () => { key = ''; loadStore(); };
$('#src').onchange = () => { key = ''; $('#detail').hidden = true; loadStore(); };
$('#qGo').onclick = async () => {
  const q = $('#q').value.trim();
  if (!q) return loadStore();
  $('#storeMsg').textContent = '搜索中…'; $('#keys').innerHTML = '';
  try {
    const r = await api(`/api/panel/search?lang=${lang()}&q=${encodeURIComponent(q)}&src=${encodeURIComponent(src())}`);
    $('#storeMsg').textContent = `“${r.q}”：${r.items.length} 部`;
    covers(r.items);
  } catch (e) { $('#storeMsg').textContent = e.message; }
};
async function openTitle(id) {
  $('#detail').hidden = false; $('#dTitle').textContent = '加载中…'; $('#dEps').innerHTML = '';
  try {
    const t = await api(`/api/panel/title/${id}?lang=${lang()}`);
    $('#dTitle').textContent = t.title;
    $('#dMeta').textContent = `${t.src ? t.src + ' · ' : ''}${t.author} · ${t.genre} · ${t.src ? '' : '免费 '}${t.episodes.length} 话${t.done ? ' · 已完结' : ''}${t.error ? ' · ' + t.error : ''}`;
    $('#dSummary').textContent = t.summary;
    $('#dEps').innerHTML = t.episodes.slice().reverse().slice(0, 30).map((e) =>
      `<li><span>#${e.no} ${esc(e.title)}</span><span><span class="muted" id="ep${e.no}"></span> <button class="ghost" data-no="${e.no}">试转</button></span></li>`).join('');
    $$('#dEps button').forEach((b) => { b.onclick = () => tryEpisode(id, b.dataset.no); });
    // 插件作品可以重新编号。/ Extension titles can be renumbered.
    $('#dRenum').hidden = !/^m\d/.test(id);
    $('#renumBtn').onclick = () => renumber(id);
    $('#detail').scrollIntoView({ behavior: 'smooth' });
  } catch (e) { $('#dTitle').textContent = e.message; }
}
async function renumber(id) {
  if (!confirm('按章节号重新给这部作品编话号？服务器上已经转好的这部的话会删掉。设备和 App 上已订阅的：先取消订阅并删掉已下载的话，再重新订阅。')) return;
  $('#renumMsg').textContent = '重新编号中…';
  try {
    const r = await api(`/api/panel/mihon/renumber/${id}`, { method: 'POST' });
    const eps = r.title.episodes;
    const note = eps.length ? `已重新编号：第 1 话是“${eps[0].title}”，第 ${eps[eps.length - 1].no} 话是“${eps[eps.length - 1].title}”` : '已重新编号';
    await openTitle(id);
    $('#renumMsg').textContent = note;
  } catch (e) { $('#renumMsg').textContent = e.message; }
}
async function tryEpisode(id, no) {
  const out = $('#ep' + no);
  for (;;) {
    const s = await api(`/api/panel/episode/${id}/${no}?lang=${lang()}`).catch((e) => ({ state: 'error', error: e.message }));
    if (s.state === 'ready') { out.innerHTML = `<a href="/api/panel/episode/${id}/${no}.cbz?lang=${lang()}">下载 CBZ（${(s.size / 1048576).toFixed(1)} MB）</a>`; return; }
    if (s.state === 'error') { out.textContent = s.error; return; }
    out.textContent = s.total ? `转换中 ${s.done}/${s.total}` : '排队中…';
    await new Promise((r) => setTimeout(r, 2000));
  }
}

// ---- 插件 / Extensions ----
let exts = [];
function showExts(r) {
  exts = r.extensions;
  $('#repos').innerHTML = r.stores.length ? r.stores.map((s) =>
    `<li><span>${esc(s.name || '')} <code>${esc(s.indexUrl)}</code></span><button class="ghost" data-url="${esc(s.indexUrl)}">删除</button></li>`).join('')
    : '<li class="muted">还没有添加仓库</li>';
  $$('#repos button').forEach((b) => { b.onclick = () => repo(b.dataset.url, true); });
  filterExts();
}
function filterExts() {
  const q = $('#extQ').value.trim().toLowerCase();
  const list = exts.filter((e) => !q || (e.name + ' ' + e.lang).toLowerCase().includes(q))
    .sort((a, b) => (b.isInstalled - a.isInstalled) || a.name.localeCompare(b.name)).slice(0, 200);
  $('#exts').innerHTML = list.length ? list.map((e) => {
    const acts = e.isInstalled ? (e.hasUpdate ? [['update', '更新']] : []).concat([['uninstall', '卸载']]) : [['install', '安装']];
    return `<li><span>${esc(e.name)} <span class="muted">${esc(e.lang)} · ${esc(e.versionName)}${e.contentWarning && e.contentWarning !== 'SAFE' ? ' · 成人' : ''}${e.isObsolete ? ' · 已失效' : ''}</span></span>
      <span>${acts.map(([a, n]) => `<button class="ghost" data-pkg="${esc(e.pkgName)}" data-a="${a}">${n}</button>`).join(' ')}</span></li>`;
  }).join('') : '<li class="muted">没有插件（先添加仓库）</li>';
  $$('#exts button').forEach((b) => { b.onclick = () => extDo(b, b.dataset.pkg, b.dataset.a); });
}
async function repo(url, remove) {
  $('#repoMsg').textContent = remove ? '删除中…' : '添加中…';
  try { showExts(await api('/api/panel/mihon/store', { method: 'POST', body: { url, remove } })); $('#repoMsg').textContent = ''; $('#repoUrl').value = ''; }
  catch (e) { $('#repoMsg').textContent = e.message; }
}
async function extDo(btn, pkg, action) {
  btn.disabled = true; btn.textContent = '处理中…';
  try { showExts(await api('/api/panel/mihon/extension', { method: 'POST', body: { pkg, action } })); loaders.mihonSources(); loadSources(); }
  catch (e) { btn.textContent = e.message; }
}
$('#repoAdd').onclick = () => { const u = $('#repoUrl').value.trim(); if (u) repo(u, false); };
$('#extQ').oninput = filterExts;
$('#extLoad').onclick = () => {
  $('#exts').innerHTML = '<li class="muted">加载中…</li>';
  api('/api/panel/mihon/extensions').then(showExts).catch((e) => { $('#exts').innerHTML = `<li class="err">${esc(e.message)}</li>`; });
};
let srcAll = [];
loaders.mihonSources = async () => {
  const r = await api('/api/panel/mihon');
  srcAll = r.items.filter((s) => s.key !== 'webtoon' && s.key !== 'ehentai');
  $('#mDedupe').checked = r.mihon_dedupe; $('#mSpread').value = r.mihon_spread;
  drawSrcs();
};
function drawSrcs() {
  $('#srcs').innerHTML = srcAll.length ? srcAll.map((s, i) =>
    `<li><label><input type="checkbox" data-i="${i}"${s.hidden ? '' : ' checked'}> ${esc(s.name)} <span class="muted">${esc(s.lang)}${s.nsfw ? ' · 成人' : ''}</span></label>
     <span><button class="ghost" data-up="${i}">↑</button> <button class="ghost" data-down="${i}">↓</button></span></li>`).join('')
    : '<li class="muted">还没有装插件</li>';
  $$('#srcs input').forEach((c) => { c.onchange = () => { srcAll[c.dataset.i].hidden = !c.checked; }; });
  const move = (i, d) => { const j = i + d; if (j < 0 || j >= srcAll.length) return; [srcAll[i], srcAll[j]] = [srcAll[j], srcAll[i]]; drawSrcs(); };
  $$('#srcs [data-up]').forEach((b) => { b.onclick = () => move(+b.dataset.up, -1); });
  $$('#srcs [data-down]').forEach((b) => { b.onclick = () => move(+b.dataset.down, 1); });
}
$('#mSave').onclick = async () => {
  try {
    await api('/api/panel/mihon/settings', { method: 'POST', body: {
      mihon_hidden: srcAll.filter((s) => s.hidden).map((s) => s.key), mihon_order: srcAll.map((s) => s.key),
      mihon_dedupe: $('#mDedupe').checked, mihon_spread: $('#mSpread').value } });
    $('#mMsg').textContent = '已保存'; loadSources();
  } catch (e) { $('#mMsg').textContent = e.message; }
};
loaders.mihon = async () => { await loaders.mihonSources(); $('#extLoad').onclick(); };

// ---- E-Hentai ----
let ehCatsOff = 0, ehCats = [];
function drawEhCats() {
  $('#ehCats').innerHTML = ehCats.map(([n, bit]) => `<a class="chip${ehCatsOff & bit ? '' : ' on'}" data-bit="${bit}">${esc(n)}</a>`).join('');
  $$('#ehCats a').forEach((a) => { a.onclick = () => { ehCatsOff ^= +a.dataset.bit; drawEhCats(); }; });
}
function showEh(r) {
  $('#ehOn').checked = r.eh_on; $('#ehSite').value = r.eh_site; $('#ehDirect').checked = r.eh_direct;
  $('#ehFilter').value = r.eh_filter; $('#ehJpn').checked = r.eh_jpn_title; $('#ehChunk').value = r.eh_chunk;
  $('#ehSpread').value = r.eh_spread;
  $('#ehSigned').textContent = r.signed_in ? `已登录${r.igneous ? '（能用 ExHentai）' : ''}，现在用 ${r.name}` : `没登录，现在用 ${r.name}`;
  ehCatsOff = r.eh_cats; ehCats = r.cats; drawEhCats();
}
loaders.eh = async () => showEh(await api('/api/panel/eh'));
async function ehPost(path, body, msgEl, note) {
  $(msgEl).textContent = '处理中…';
  try {
    const r = await api(path, { method: 'POST', body });
    showEh(r);
    $('#ehPass').value = $('#ehMember').value = $('#ehHash').value = $('#ehIgneous').value = '';
    $(msgEl).textContent = typeof note === 'function' ? note(r) : note; loadSources();
  } catch (e) { $(msgEl).textContent = e.message; }
}
$('#ehSave').onclick = () => ehPost('/api/panel/eh', {
  eh_on: $('#ehOn').checked, eh_site: $('#ehSite').value, eh_direct: $('#ehDirect').checked, eh_filter: $('#ehFilter').value,
  eh_cats: ehCatsOff, eh_jpn_title: $('#ehJpn').checked, eh_chunk: Number($('#ehChunk').value), eh_spread: $('#ehSpread').value,
}, '#ehMsg', '已保存');
$('#ehLogin').onclick = () => ehPost('/api/panel/eh/login', { user: $('#ehUser').value, password: $('#ehPass').value },
  '#ehLoginMsg', (r) => r.login.exhentai ? '已登录，ExHentai 也能用' : '已登录（这个账号还进不了 ExHentai）');
$('#ehLogout').onclick = () => { if (confirm('退出 E-Hentai 登录？')) ehPost('/api/panel/eh', { eh_member_id: '', eh_pass_hash: '', eh_igneous: '' }, '#ehLoginMsg', '已退出'); };
$('#ehCookieSave').onclick = () => {
  const body = {};
  if ($('#ehMember').value.trim()) body.eh_member_id = $('#ehMember').value.trim();
  if ($('#ehHash').value.trim()) body.eh_pass_hash = $('#ehHash').value.trim();
  if ($('#ehIgneous').value.trim()) body.eh_igneous = $('#ehIgneous').value.trim();
  ehPost('/api/panel/eh', body, '#ehLoginMsg', '已保存 cookie');
};

// ---- 设备与外网 / Device and tunnel ----
loaders.device = async () => {
  const [cfg, st] = await Promise.all([api('/api/panel/settings'), api('/api/panel/status')]);
  $('#devUrl').textContent = location.origin;
  $('#devToken').textContent = cfg.device_token;
  $$('input[name=tmode]').forEach((r) => { r.checked = r.value === cfg.tunnel_mode; });
  showTunnel(st.tunnel);
};
function showTunnel(t) {
  $('#devTunnelUrl').innerHTML = t.url ? ` / <code>${esc(t.url)}</code>（外网）` : '';
  $('#tState').textContent = t.status + (t.error ? '：' + t.error : '');
}
$('#rotate').onclick = async () => { if (confirm('换令牌后设备要重新填，确定？')) { await api('/api/panel/device/rotate', { method: 'POST' }); loaders.device(); } };
$('#tApply').onclick = async () => {
  const mode = ($('input[name=tmode]:checked') || {}).value || 'off';
  try { showTunnel(await api('/api/panel/tunnel', { method: 'POST', body: { mode, token: $('#tToken').value } })); $('#tToken').value = '';
    setTimeout(loaders.device, 4000); setTimeout(loaders.device, 10000);
  } catch (e) { $('#tState').textContent = e.message; }
};

// ---- 设置 / Settings ----
loaders.settings = async () => {
  const cfg = await api('/api/panel/settings');
  for (const el of $('#setForm').elements) if (el.name && cfg[el.name] !== undefined) el.value = cfg[el.name];
};
$('#setSave').onclick = async () => {
  const body = {};
  for (const el of $('#setForm').elements) if (el.name) body[el.name] = Number(el.value);
  try { await api('/api/panel/settings', { method: 'POST', body }); $('#setMsg').textContent = '已保存'; }
  catch (e) { $('#setMsg').textContent = e.message; }
};
$('#cacheClear').onclick = async () => {
  if (!confirm('清空所有转好的话和封面？设备下次要时会重新转。')) return;
  await api('/api/panel/cache/clear', { method: 'POST' }); $('#setMsg').textContent = '已清空';
};
$('#pwForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/password', { method: 'POST', body: { password: $('#pw').value } }); $('#pw').value = ''; $('#pwMsg').textContent = '已修改'; }
  catch (err) { $('#pwMsg').textContent = err.message; }
};

boot();
