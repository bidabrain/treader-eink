// 番茄下载器面板脚本：无构建、无第三方库。/ Tomato downloader panel script: no build step, no third-party libraries.
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
const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---- 进入 / Gate ----
let setupMode = false;
async function boot() {
  const s = await api('/hub/api/state');
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
    await api(setupMode ? '/hub/api/setup' : '/hub/api/login', { method: 'POST', body: { password: $('#gatePassword').value } });
    $('#gatePassword').value = ''; $('#gateMsg').textContent = ''; boot();
  } catch (err) { $('#gateMsg').textContent = err.message; }
};
$('#logout').onclick = async () => { await api('/hub/api/logout', { method: 'POST' }); location.reload(); };

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
  const s = await api('/hub/api/status');
  const d = s.downloader;
  const items = [
    ['下载器', d.ok ? '在线' : '离线'], ['下载器版本', d.version || '—'], ['书库', d.books ?? '—'],
    ['进行中的下载', d.jobs ?? '—'], ['外网', s.tunnel.status],
  ];
  $('#stats').innerHTML = items.map(([k, v]) => `<div class="stat"><span class="muted">${k}</span><b>${esc(v)}</b></div>`).join('');
  $('#upState').innerHTML = d.ok ? `书保存在下载器容器的 <code>${esc(d.save_dir)}</code>（宿主机 <code>data/downloader/books</code>）。`
    : `<span class="err">连不上番茄下载器${d.error ? '（' + esc(d.error) + '）' : ''}。刚启动的话等几秒再刷新；一直这样就看 docker compose logs downloader。</span>`;
};

// ---- 设备与外网 / Device and tunnel ----
loaders.device = async () => {
  const d = await api('/hub/api/device');
  $('#devUrl').textContent = location.origin;
  $('#devPw').textContent = d.password;
  $$('input[name=tmode]').forEach((r) => { r.checked = r.value === d.tunnel_mode; });
  showTunnel(d.tunnel);
};
function showTunnel(t) {
  $('#tState').innerHTML = `${esc(t.status)}${t.url ? ' · 外网地址 <code>' + esc(t.url) + '</code>' : ''}${t.error ? '<br><span class="err">' + esc(t.error) + '</span>' : ''}`;
  $('#devTunnelUrl').innerHTML = t.url ? ` / <code>${esc(t.url)}</code>（外网）` : '';
}
async function setDevicePassword(pw) {
  try {
    const r = await api('/hub/api/device/password', { method: 'POST', body: { password: pw } });
    $('#devPw').textContent = r.password; $('#pwCustom').value = '';
    $('#pwMsg').textContent = '已更换。设备和 App 上要改成新的密码。';
  } catch (e) { $('#pwMsg').textContent = e.message; }
}
$('#pwRandom').onclick = () => { if (confirm('换成一个随机密码？设备和 App 上要重新填。')) setDevicePassword(''); };
$('#pwSet').onsubmit = (e) => { e.preventDefault(); const v = $('#pwCustom').value.trim(); if (v) setDevicePassword(v); };
$('#tApply').onclick = async () => {
  const mode = ($('input[name=tmode]:checked') || {}).value || 'off';
  try { showTunnel(await api('/hub/api/tunnel', { method: 'POST', body: { mode, token: $('#tToken').value } })); $('#tToken').value = '';
    setTimeout(loaders.device, 6000); } catch (e) { $('#tState').textContent = e.message; }
};

// ---- 设置 / Settings ----
$('#pwForm').onsubmit = async (e) => {
  e.preventDefault();
  try { await api('/hub/api/password', { method: 'POST', body: { password: $('#pwNew').value } }); $('#pwFormMsg').textContent = '已修改，请重新登录'; setTimeout(() => location.reload(), 1200); }
  catch (err) { $('#pwFormMsg').textContent = err.message; }
};

// ---- 日志 / Logs ----
loaders.logs = async () => { $('#logBox').textContent = (await api('/hub/api/logs')).join('\n') || '暂无'; };

boot().catch((e) => { document.body.textContent = '面板加载失败：' + e.message; });
