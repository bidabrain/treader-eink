// rednote 面板脚本：无构建、无第三方库。/ rednote panel script: no build step, no third-party libraries.
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
  const items = [
    ['小红书服务', s.mcp_up ? '在线' : '离线'], ['关注博主', s.creators], ['笔记', s.notes],
    ['缓存', s.cache_mb + ' MB'], ['今日请求', s.requests_today + ' / ' + s.daily_cap],
    ['今日推荐请求', s.rec_enabled ? s.rec_today + ' / ' + s.rec_daily_cap : '已关闭'], ['外网', s.tunnel.status],
  ];
  $('#stats').innerHTML = items.map(([k, v]) => `<div class="stat"><span class="muted">${k}</span><b>${esc(v)}</b></div>`).join('');
  const f = s.fetch;
  $('#fetchState').innerHTML = f.paused ? `<span class="err">已暂停：${esc(f.paused)}</span>（处理好后点“立即拉一轮”继续）`
    : f.running ? `正在拉取：${esc(f.current || '…')}，本轮新增 ${f.new_notes} 篇`
    : `上一轮：${when(f.last_round)}；下一轮：${when(f.next_round)}`;
};
$('#runNow').onclick = async () => { await api('/api/panel/fetch/run', { method: 'POST' }); setTimeout(loaders.home, 800); };

// ---- 小红书账号 / Account ----
$('#xhsCheck').onclick = async () => {
  $('#xhsState').textContent = '查询中…（要开浏览器，约 5 秒）';
  try {
    const s = await api('/api/panel/xhs/status');
    $('#xhsState').textContent = s.is_logged_in ? `已登录：${s.username || ''}` : '未登录';
  } catch (e) { $('#xhsState').textContent = '查询失败：' + e.message; }
};
$('#xhsQr').onclick = async () => {
  $('#xhsQr').disabled = true; $('#qrBox').hidden = true;
  $('#xhsState').textContent = '正在打开登录页…如果超过半分钟还没出二维码，打开下面的“浏览器桌面”点一下页面上的“登录”。';
  try {
    const q = await api('/api/panel/xhs/qrcode', { method: 'POST' });
    if (q.is_logged_in) { $('#xhsState').textContent = '已经是登录状态'; return; }
    $('#qrImg').src = q.img; $('#qrBox').hidden = false;
    $('#qrHint').textContent = `用小红书 App 扫码，${q.timeout || '几分钟'}内有效。扫完稍等几秒再点“查询登录状态”（查询会刷新登录页，别在扫码前点）。`;
    $('#xhsState').textContent = '等待扫码';
  } catch (e) { $('#xhsState').textContent = '获取二维码失败：' + e.message; }
  finally { $('#xhsQr').disabled = false; }
};

// ---- 博主 / Creators ----
loaders.creators = async () => {
  const list = await api('/api/panel/creators');
  $('#creatorList').innerHTML = list.length ? list.map((c) => `<li><span><b>${esc(c.nickname || c.user_id)}</b>
    <span class="muted">上次：${when(c.last_fetch)}${c.last_error ? ' · <span class="err">' + esc(c.last_error) + '</span>' : ''}</span></span>
    <button class="ghost" data-del="${c.user_id}">移除</button></li>`).join('') : '<li class="muted">还没有，先添加一个。</li>';
  $$('[data-del]').forEach((b) => { b.onclick = async () => { await api('/api/panel/creators/' + b.dataset.del, { method: 'DELETE' }); loaders.creators(); }; });
};
$('#addForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/creators', { method: 'POST', body: { link: $('#addLink').value } }); $('#addLink').value = ''; $('#addMsg').textContent = ''; loaders.creators(); }
  catch (err) { $('#addMsg').textContent = err.message; }
};
$('#discover').onclick = async () => {
  $('#discoverList').innerHTML = '<li class="muted">读取中…</li>';
  try {
    const list = await api('/api/panel/discover');
    $('#discoverList').innerHTML = list.map((u, i) => `<li><span><b>${esc(u.nickname)}</b> <span class="muted">${esc(u.sample)}${u.type === 'video' ? '（视频）' : ''}</span></span>
      <button class="ghost" data-pick="${i}">关注</button></li>`).join('');
    $$('[data-pick]').forEach((b) => { b.onclick = async () => { const u = list[+b.dataset.pick];
      await api('/api/panel/creators/pick', { method: 'POST', body: u }); b.textContent = '已添加'; b.disabled = true; loaders.creators(); }; });
  } catch (e) { $('#discoverList').innerHTML = `<li class="err">${esc(e.message)}</li>`; }
};

// ---- 内容 / Notes ----
loaders.notes = async () => {
  const list = await api('/api/panel/notes');
  $('#noteGrid').innerHTML = list.length ? list.map((n) => `<figure><img loading="lazy" src="/api/panel/img/${n.id}/thumb.jpg" alt="">
    <figcaption><b>${esc(n.title || '（无标题）')}</b><br><span class="muted">${n.source === 'rec' ? '推荐 · ' : ''}${esc(n.author)} · ${n.images} 页</span></figcaption></figure>`).join('')
    : '<p class="muted">还没有内容。</p>';
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
loaders.settings = async () => {
  const cfg = await api('/api/panel/settings');
  fill($('#setForm'), cfg);
  fill($('#davForm'), cfg);
  $('#davForm').webdav_pass.value = '';
  $('#davForm').webdav_pass.placeholder = cfg.has_webdav_pass ? '已保存，留空表示不改' : 'WebDAV 密码';
};
$('#davForm').onsubmit = async (e) => {
  e.preventDefault();
  const f = e.target;
  try {
    await api('/api/panel/settings', { method: 'POST', body: { webdav_url: f.webdav_url.value, webdav_user: f.webdav_user.value, webdav_pass: f.webdav_pass.value } });
    $('#davMsg').textContent = '已保存'; loaders.settings();
  } catch (err) { $('#davMsg').textContent = err.message; }
};
$('#davBackup').onclick = async () => {
  $('#davMsg').textContent = '备份中…';
  try { const r = await api('/api/panel/webdav/backup', { method: 'POST' }); $('#davMsg').textContent = `已备份 ${r.creators} 位博主`; }
  catch (err) { $('#davMsg').textContent = err.message; }
};
$('#davRestore').onclick = async () => {
  if (!confirm('用备份里的名单替换现在关注的博主？')) return;
  $('#davMsg').textContent = '恢复中…';
  try { const r = await api('/api/panel/webdav/restore', { method: 'POST' }); $('#davMsg').textContent = `已恢复 ${r.creators} 位博主`; }
  catch (err) { $('#davMsg').textContent = err.message; }
};
$('#setForm').onsubmit = async (e) => {
  e.preventDefault();
  const body = {};
  for (const el of e.target.elements) if (el.name) body[el.name] = el.type === 'checkbox' ? el.checked : el.value;
  try { await api('/api/panel/settings', { method: 'POST', body }); $('#setMsg').textContent = '已保存'; }
  catch (err) { $('#setMsg').textContent = err.message; }
};
$('#pwForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/api/panel/password', { method: 'POST', body: { password: $('#pwNew').value } }); $('#pwMsg').textContent = '已修改，请重新登录'; setTimeout(() => location.reload(), 1200); }
  catch (err) { $('#pwMsg').textContent = err.message; }
};

// ---- 日志 / Logs ----
loaders.logs = async () => { $('#logBox').textContent = (await api('/api/panel/logs')).join('\n') || '暂无'; };

boot().catch((e) => { document.body.textContent = '面板加载失败：' + e.message; });
