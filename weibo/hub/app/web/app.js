// weibo 面板脚本：无构建、无第三方库。/ weibo panel script: no build step, no third-party libraries.
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
function show(tab) {
  if (current === 'captcha' && tab !== 'captcha') capClose();
  current = tab;
  $$('header nav a').forEach((a) => a.classList.toggle('on', a.dataset.tab === tab));
  $$('main section').forEach((s) => { s.hidden = s.dataset.panel !== tab; });
  if (loaders[tab]) loaders[tab]().catch((e) => console.warn(e));
}
$$('header nav a').forEach((a) => { a.onclick = () => show(a.dataset.tab); });

// ---- 概览 / Home ----
loaders.home = async () => {
  const s = await api('/api/panel/status');
  const items = [
    ['身份', s.fetch.visitor ? '访客' : 'Cookie'], ['关注', s.users], ['关注微博', s.follow_posts], ['热门微博', s.hot_posts],
    ['缓存', s.cache_mb + ' MB'], ['今日长文', s.long_today + ' / ' + s.long_cap], ['外网', s.tunnel.status],
  ];
  $('#stats').innerHTML = items.map(([k, v]) => `<div class="stat"><span class="muted">${k}</span><b>${esc(v)}</b></div>`).join('');
  const f = s.fetch;
  $('#fetchState').innerHTML = (s.captcha ? '<span class="err">微博要求验证，到“验证”页处理。</span><br>' : '')
    + (f.paused ? `<span class="err">已暂停：${esc(f.paused)}</span>`
    : f.running ? `正在拉取：${esc(f.current || '…')}（${f.done}/${f.total}），本轮新增关注 ${f.new_follow} 条`
    : `上一轮：${when(f.last_round)}；下一轮：${when(f.next_round)}`)
    + (f.last_error ? `<br><span class="err">${esc(f.last_error)}</span>` : '');
};
$('#runNow').onclick = async () => { await api('/api/panel/fetch/run', { method: 'POST' }); setTimeout(loaders.home, 800); };

// ---- 关注 / Follows ----
loaders.users = async () => {
  const list = await api('/api/panel/users');
  $('#userCount').textContent = list.length ? `（${list.length}）` : '';
  $('#userList').innerHTML = list.length ? list.map((u) => `<li><span>${u.avatar_url ? `<img class="avatar" referrerpolicy="no-referrer" src="${esc(u.avatar_url)}" alt="">` : ''}<b>${esc(u.screen_name || u.id)}</b>${u.special ? ' ★' : ''}
    <span class="muted">${esc(u.id)} · 上次：${when(u.last_fetched_at)}${u.last_error ? ' · <span class="err">' + esc(u.last_error) + '</span>' : ''}</span></span>
    <span><button class="ghost" data-sp="${u.id}" data-on="${u.special ? 1 : 0}">${u.special ? '取消特别' : '特别关注'}</button>
    <button class="ghost" data-del="${u.id}">移除</button></span></li>`).join('') : '<li class="muted">还没有，先添加一个。</li>';
  $$('[data-del]').forEach((b) => { b.onclick = async () => { if (!confirm('移除这个人？')) return; await api('/api/panel/users/' + b.dataset.del, { method: 'DELETE' }); loaders.users(); }; });
  $$('[data-sp]').forEach((b) => { b.onclick = async () => { await api(`/api/panel/users/${b.dataset.sp}/special`, { method: 'POST', body: { special: b.dataset.on !== '1' } }); loaders.users(); }; });
};
$('#addForm').onsubmit = async (e) => {
  e.preventDefault();
  $('#addMsg').textContent = '查询中…';
  try { await api('/api/panel/users', { method: 'POST', body: { link: $('#addLink').value } }); $('#addLink').value = ''; $('#addMsg').textContent = ''; loaders.users(); }
  catch (err) { $('#addMsg').textContent = err.message; }
};
$('#searchForm').onsubmit = async (e) => {
  e.preventDefault();
  $('#searchList').innerHTML = '<li class="muted">搜索中…</li>';
  try {
    const list = await api('/api/panel/search?q=' + encodeURIComponent($('#searchQ').value));
    $('#searchList').innerHTML = list.length ? list.map((u, i) => `<li><span><b>${esc(u.screen_name)}</b> <span class="muted">${esc(u.followers_count)} 粉丝 · ${esc((u.description || '').slice(0, 40))}</span></span>
      <button class="ghost" data-pick="${i}" ${u.followed ? 'disabled' : ''}>${u.followed ? '已关注' : '关注'}</button></li>`).join('') : '<li class="muted">没搜到。</li>';
    $$('[data-pick]').forEach((b) => { b.onclick = async () => {
      b.disabled = true;
      try { await api('/api/panel/users/pick', { method: 'POST', body: { id: list[+b.dataset.pick].id } }); b.textContent = '已关注'; loaders.users(); }
      catch (err) { b.disabled = false; alert(err.message); }
    }; });
  } catch (err) { $('#searchList').innerHTML = `<li class="err">${esc(err.message)}</li>`; }
};

// ---- 内容 / Posts ----
loaders.posts = async () => {
  const list = await api('/api/panel/posts');
  $('#postGrid').innerHTML = list.length ? list.map((p) => `<figure>${p.images
    ? `<img loading="lazy" src="/api/panel/img/${p.id}/thumb.jpg" alt="">` : `<div class="txt">${esc(p.text.slice(0, 120))}</div>`}
    <figcaption>${p.in_hot ? '<span class="tag">热门</span>' : ''}${p.in_follow ? '<span class="tag">关注</span>' : ''}<b>${esc(p.user_name)}</b>
    <span class="muted">${p.images} 页</span><br><span class="muted">${esc(p.text.slice(0, 60))}</span></figcaption></figure>`).join('')
    : '<p class="muted">还没有内容。</p>';
};

// ---- 验证 / Captcha ----
let ws = null;
function capClose() { if (ws) { ws.close(); ws = null; } }
loaders.captcha = async () => {
  capClose();
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/api/panel/captcha/ws');
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === 'frame') { $('#capImg').src = 'data:image/jpeg;base64,' + m.data; $('#capBox').hidden = false; $('#capState').textContent = '在下面的画面里操作。'; }
    else if (m.type === 'state' && !m.pending) { $('#capBox').hidden = true; $('#capState').textContent = '现在不需要验证。'; }
  };
  ws.onclose = () => { if (current === 'captcha') $('#capState').textContent = '连接断开，切回这一页重连。'; };
};
const send = (m) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(m)); };
function pos(e) {
  const img = $('#capImg'), r = img.getBoundingClientRect();
  const scale = img.naturalWidth / r.width || 1;
  return { x: (e.clientX - r.left) * scale, y: (e.clientY - r.top) * scale };
}
let down = null;
$('#capImg').addEventListener('pointerdown', (e) => { e.preventDefault(); down = pos(e); e.target.setPointerCapture(e.pointerId); send({ type: 'down', ...down }); });
$('#capImg').addEventListener('pointermove', (e) => { if (down) send({ type: 'move', ...pos(e) }); });
$('#capImg').addEventListener('pointerup', (e) => { if (!down) return; const p = pos(e); send({ type: 'up', ...p }); down = null; });
$('#capDone').onclick = () => send({ type: 'resolve' });
$('#capReload').onclick = () => send({ type: 'reload' });
$('#capCancel').onclick = () => send({ type: 'cancel' });

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
  $('#setForm').hot_channel.innerHTML = Object.entries(cfg.hot_channels).map(([k, v]) => `<option value="${esc(k)}">${esc(v)}</option>`).join('');
  fill($('#setForm'), cfg); fill($('#davForm'), cfg);
  $('#setForm').cookie.value = ''; $('#setForm').clear_cookie.checked = false; $('#davForm').webdav_pass.value = '';
  $('#cookieState').textContent = cfg.has_cookie ? '（已保存一份 Cookie）' : '（现在没有 Cookie）';
};
$('#setForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/settings', { method: 'POST', body: collect(e.target) }); $('#setMsg').textContent = '已保存'; loaders.settings(); }
  catch (err) { $('#setMsg').textContent = err.message; }
};
$('#davForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/settings', { method: 'POST', body: collect(e.target) }); $('#davMsg').textContent = '已保存'; loaders.settings(); }
  catch (err) { $('#davMsg').textContent = err.message; }
};
$('#davBackup').onclick = async () => {
  $('#davMsg').textContent = '备份中…';
  try { const r = await api('/api/panel/webdav/backup', { method: 'POST' }); $('#davMsg').textContent = `已备份 ${r.users} 人`; }
  catch (err) { $('#davMsg').textContent = err.message; }
};
$('#davRestore').onclick = async () => {
  if (!confirm('用备份里的名单替换现在的关注？')) return;
  $('#davMsg').textContent = '恢复中…';
  try { const r = await api('/api/panel/webdav/restore', { method: 'POST' }); $('#davMsg').textContent = `已恢复 ${r.users} 人${r.cookie ? '，含 Cookie' : ''}`; }
  catch (err) { $('#davMsg').textContent = err.message; }
};
$('#pwForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/password', { method: 'POST', body: { password: $('#pwNew').value } }); $('#pwMsg').textContent = '已修改，请重新登录'; setTimeout(() => location.reload(), 1200); }
  catch (err) { $('#pwMsg').textContent = err.message; }
};

// ---- 日志 / Logs ----
loaders.logs = async () => { $('#logBox').textContent = (await api('/api/panel/logs')).join('\n') || '暂无'; };

boot().catch((e) => { document.body.textContent = '面板加载失败：' + e.message; });
