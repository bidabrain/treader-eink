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
let seg = 'rank', key = '';
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
  chips($('#segs'), SEGS, seg, (k) => { seg = k; key = ''; loadStore(); });
  $('#storeMsg').textContent = '加载中…';
  try {
    const r = await api(`/api/panel/store?lang=${lang()}&seg=${seg}&key=${encodeURIComponent(key)}`);
    key = r.key;
    chips($('#keys'), r.keys.map((k) => [k.key, k.name]), key, (k) => { key = k; loadStore(); });
    $('#storeMsg').textContent = `${r.items.length} 部`;
    covers(r.items);
  } catch (e) { $('#storeMsg').textContent = e.message; }
}
loaders.store = loadStore;
$('#lang').onchange = () => { key = ''; loadStore(); };
$('#qGo').onclick = async () => {
  const q = $('#q').value.trim();
  if (!q) return loadStore();
  $('#storeMsg').textContent = '搜索中…'; $('#keys').innerHTML = '';
  try {
    const r = await api(`/api/panel/search?lang=${lang()}&q=${encodeURIComponent(q)}`);
    $('#storeMsg').textContent = `“${r.q}”：${r.items.length} 部`;
    covers(r.items);
  } catch (e) { $('#storeMsg').textContent = e.message; }
};
async function openTitle(id) {
  $('#detail').hidden = false; $('#dTitle').textContent = '加载中…'; $('#dEps').innerHTML = '';
  try {
    const t = await api(`/api/panel/title/${id}?lang=${lang()}`);
    $('#dTitle').textContent = t.title;
    $('#dMeta').textContent = `${t.author} · ${t.genre} · 免费 ${t.episodes.length} 话${t.done ? ' · 已完结' : ''}`;
    $('#dSummary').textContent = t.summary;
    $('#dEps').innerHTML = t.episodes.slice().reverse().slice(0, 30).map((e) =>
      `<li><span>#${e.no} ${esc(e.title)}</span><span><span class="muted" id="ep${e.no}"></span> <button class="ghost" data-no="${e.no}">试转</button></span></li>`).join('');
    $$('#dEps button').forEach((b) => { b.onclick = () => tryEpisode(id, b.dataset.no); });
    $('#detail').scrollIntoView({ behavior: 'smooth' });
  } catch (e) { $('#dTitle').textContent = e.message; }
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
