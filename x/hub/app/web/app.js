// X 面板脚本：无构建、无第三方库。/ X panel script: no build step, no third-party libraries.
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
let current = 'home';
let timer = null;
function show(tab) {
  current = tab;
  $$('header nav a').forEach((a) => a.classList.toggle('on', a.dataset.tab === tab));
  $$('main section').forEach((s) => { s.hidden = s.dataset.panel !== tab; });
  clearInterval(timer); timer = null;
  if (tab !== 'xlogin') closeVnc();
  if (loaders[tab]) loaders[tab]().catch((e) => console.warn(e));
  // 概览和登录页每 3 秒刷新状态。/ Home and sign-in refresh their status every 3 seconds.
  if (tab === 'home' || tab === 'xlogin') timer = setInterval(() => loaders[tab]().catch(() => {}), 3000);
}
$$('header nav a').forEach((a) => { a.onclick = () => show(a.dataset.tab); });

const ACCOUNT = { ok: '<span class="ok">已登录 X</span>', need_login: '<span class="err">没有登录 X 或登录已失效</span>',
  locked: '<span class="err">X 要求验证账号</span>', unknown: '浏览器启动中…' };

// ---- 概览 / Home ----
loaders.home = async () => {
  const s = await api('/api/panel/status');
  $('#acct').innerHTML = ACCOUNT[s.account] + (s.account !== 'ok' && s.account !== 'unknown' ? '：到“登录 X”页处理。' : '')
    + (s.sleeping ? ' · <span class="muted">现在是睡眠时段，自动拉取暂停</span>' : '')
    + (s.running ? ` · 正在拉取${esc(s.feeds[s.running].name)}…` : '');
  $('#pauseLine').innerHTML = s.paused ? `<span class="err">自动拉取已停：${esc(s.paused)}</span> <button id="resume" class="ghost">恢复自动拉取</button>` : '';
  if ($('#resume')) $('#resume').onclick = async () => { await api('/api/panel/resume', { method: 'POST' }); loaders.home(); };
  $('#feeds').innerHTML = Object.entries(s.feeds).map(([k, f]) => `<div class="card"><h2>${esc(f.name)}</h2>
    <p>${!f.on ? '<span class="muted">总开关已关，不拉取</span>' : f.enabled ? `下次自动：${when(f.next_at)}` : '<span class="muted">自动拉取已关</span>'}<br>
    上次：${when(f.last_at)}${f.last_at ? `，${f.last_total} 条，新 ${f.last_new}，去广告 ${f.last_ads}` : ''}<br>
    已存：${f.count} 条${f.last_error ? `<br><span class="err">${esc(f.last_error)}</span>` : ''}</p>
    <button data-fetch="${k}" ${s.running === k || !f.on ? 'disabled' : ''}>立即拉取</button> <span class="muted" data-msg="${k}"></span></div>`).join('');
  $$('[data-fetch]').forEach((b) => { b.onclick = async () => {
    const m = $(`[data-msg="${b.dataset.fetch}"]`);
    try { const r = await api('/api/panel/fetch/' + b.dataset.fetch, { method: 'POST' }); m.textContent = r.message; } catch (e) { m.textContent = e.message; }
  }; });
  const items = [['推文', s.posts], ['缓存', s.cache_mb + ' MB'], ['外网', s.tunnel.status]];
  $('#stats').innerHTML = items.map(([k, v]) => `<div class="stat"><span class="muted">${k}</span><b>${esc(v)}</b></div>`).join('');
};

// ---- 登录 X / X sign-in ----
const VNC_URL = '/novnc/vnc.html?autoconnect=1&resize=scale&reconnect=1&reconnect_delay=1500&show_dot=1&path=api/panel/vnc/ws';
function closeVnc() { const f = $('#vncFrame'); if (!f.hidden) { f.src = 'about:blank'; f.hidden = true; } $('#loginPop').hidden = true; }
loaders.xlogin = async () => {
  const s = await api('/api/panel/status');
  const l = s.login;
  $('#loginState').innerHTML = ACCOUNT[s.account] + (l.message ? ` · ${esc(l.message)}` : '');
  $('#loginOpen').disabled = l.active; $('#loginStop').disabled = !l.active;
  const f = $('#vncFrame');
  const ready = l.active && /在下面/.test(l.message);
  if (ready && f.hidden) { f.src = VNC_URL; f.hidden = false; $('#loginPop').href = VNC_URL; $('#loginPop').hidden = false; }
  if (!l.active) closeVnc();
};
$('#loginOpen').onclick = async () => { await api('/api/panel/x/login', { method: 'POST' }); loaders.xlogin(); };
$('#loginStop').onclick = async () => {
  closeVnc(); $('#loginState').textContent = '正在保存登录…';
  await api('/api/panel/x/login/stop', { method: 'POST' }); loaders.xlogin();
};
$('#cookieForm').onsubmit = async (e) => {
  e.preventDefault();
  $('#cookieMsg').textContent = '导入中…';
  try { const r = await api('/api/panel/x/cookies', { method: 'POST', body: { text: e.target.text.value } });
    e.target.text.value = ''; $('#cookieMsg').textContent = r.ok ? '已导入，已登录' : '导入了，但没有登录态'; }
  catch (err) { $('#cookieMsg').textContent = err.message; }
};
$('#signout').onclick = async () => {
  if (!confirm('清除服务器浏览器里的 X 登录？之后要重新登录才能拉取。')) return;
  await api('/api/panel/x/signout', { method: 'POST' }); loaders.xlogin();
};

// ---- 内容 / Posts ----
loaders.posts = async () => {
  const list = await api('/api/panel/posts');
  $('#postGrid').innerHTML = list.length ? list.map((p) => `<figure>${p.images
    ? `<img loading="lazy" src="/api/panel/img/${p.id}/thumb.jpg" alt="">` : `<div class="txt">${esc(p.text.slice(0, 140))}</div>`}
    <figcaption>${p.in_foryou ? '<span class="tag">推荐</span>' : ''}${p.in_following ? '<span class="tag">关注</span>' : ''}${p.video ? '<span class="tag">视频</span>' : ''}<b>${esc(p.user_name)}</b>
    <span class="muted">@${esc(p.screen_name)}${p.rt_by ? ' · ' + esc(p.rt_by) + ' 转推' : ''}</span><br><span class="muted">${esc(p.text.slice(0, 80))}</span></figcaption></figure>`).join('')
    : '<p class="muted">还没有内容。</p>';
};

// ---- 设置 / Settings ----
function fill(form, cfg) {
  for (const el of form.elements) {
    if (!el.name || !(el.name in cfg)) continue;
    if (el.type === 'checkbox') el.checked = !!cfg[el.name]; else el.value = cfg[el.name];
  }
}
function collect(form) {
  const body = {};
  for (const el of form.elements) if (el.name) body[el.name] = el.type === 'checkbox' ? el.checked : el.value;
  return body;
}
loaders.settings = async () => {
  const cfg = await api('/api/panel/settings');
  $('#floor').textContent = cfg.floor_min;
  fill($('#setForm'), cfg);
};
$('#setForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/settings', { method: 'POST', body: collect(e.target) }); $('#setMsg').textContent = '已保存'; loaders.settings(); }
  catch (err) { $('#setMsg').textContent = err.message; }
};
$('#pwForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/password', { method: 'POST', body: { password: $('#pwNew').value } }); $('#pwMsg').textContent = '已修改，请重新登录'; setTimeout(() => location.reload(), 1200); }
  catch (err) { $('#pwMsg').textContent = err.message; }
};

// ---- 设备与外网 / Device and tunnel ----
loaders.device = async () => {
  const d = await api('/api/panel/device');
  $('#devUrl').textContent = location.origin;
  $('#devToken').textContent = d.device_token;
  const cfg = await api('/api/panel/settings');
  $$('input[name=tmode]').forEach((r) => { r.checked = r.value === cfg.tunnel_mode; });
  showTunnel(d.tunnel);
};
function showTunnel(t) {
  $('#tState').innerHTML = `${esc(t.status)}${t.url ? ' · 外网地址 <code>' + esc(t.url) + '</code>' : ''}${t.error ? '<br><span class="err">' + esc(t.error) + '</span>' : ''}`;
  $('#devTunnelUrl').innerHTML = t.url ? ` / <code>${esc(t.url)}</code>（外网）` : '';
}
$('#rotate').onclick = async () => { if (confirm('换令牌后设备要重新填，确定？')) { await api('/api/panel/device/rotate', { method: 'POST' }); loaders.device(); } };
$('#tApply').onclick = async () => {
  const mode = ($('input[name=tmode]:checked') || {}).value || 'off';
  try { showTunnel(await api('/api/panel/tunnel', { method: 'POST', body: { mode, token: $('#tToken').value } })); $('#tToken').value = '';
    setTimeout(loaders.device, 6000); } catch (e) { $('#tState').textContent = e.message; }
};

// ---- 日志 / Logs ----
loaders.logs = async () => { $('#logBox').textContent = (await api('/api/panel/logs')).join('\n') || '暂无'; };

boot().catch((e) => { document.body.textContent = '面板加载失败：' + e.message; });
