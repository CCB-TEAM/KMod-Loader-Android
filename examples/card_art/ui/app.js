/* the card art pack manager: a page of the loader's UI.web. Lua owns the data and answers every action with a fresh state. */
var q = '', prev = { key: null, uri: null, err: false };
var S = { packs: [], t: {} }, view = { pack: null, page: 1 }, status = null, armed = null, PAGE = 10;
var body = document.getElementById('body');

function esc(s) { return String(s).replace(/[&<>"']/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; }); }
function T(k) { return S.t[k] || k; }
function fmt(k) { var a = arguments, i = 1; return T(k).replace(/%d/g, function () { return a[i++]; }); }
function arr(x) { return Array.isArray(x) ? x : []; }
function find(n) { var p = arr(S.packs); for (var i = 0; i < p.length; i++) if (p[i].name === n) return p[i]; return null; }
function has(s, k) { return String(s).toLowerCase().indexOf(k) >= 0; }
function entryHit(e) { var k = q.trim().toLowerCase(); return !k || has(e.card, k) || has(e.file, k) || (e.name && has(e.name, k)); }
function packHit(p) { var k = q.trim().toLowerCase(); return !k || has(p.name, k) || arr(p.entries).some(entryHit); }
function act(o) { armed = null; kmod.send(o); }

function render() {
  document.getElementById('title').textContent = T('mgr_title');
  document.getElementById('q').placeholder = T('mgr_search');
  var el = document.getElementById('status');
  el.textContent = status ? status.text : '';
  el.className = status ? (status.ok ? 'ok' : 'err') : '';
  var p = view.pack && find(view.pack);
  if (view.pack && !p) view.pack = null;
  body.innerHTML = p ? packView(p) : listView();
}

function listView() {
  var h = '<p class="intro">' + esc(fmt('mgr_intro', arr(S.packs).length)) + '</p>';
  h += '<div class="bar2"><button class="btn acc" data-a="import">' + esc(T('mgr_import_pack')) + '</button></div>';
  var ps = arr(S.packs).filter(packHit);
  if (!arr(S.packs).length) h += '<div class="empty">' + esc(T('mgr_none')) + '</div>';
  else if (!ps.length) h += '<div class="empty">' + esc(T('mgr_no_match')) + '</div>';
  ps.forEach(function (p) {
    h += '<div class="row' + (p.enabled ? '' : ' off') + '"><button class="sw' + (p.enabled ? ' on' : '') + '" data-a="toggle" data-p="' + esc(p.name) + '"></button>' +
      '<span class="name">' + esc(p.name) + '</span><span class="meta">' + esc(fmt('mgr_counts', p.images, p.cards)) + '</span>' +
      '<button class="btn" data-a="open" data-p="' + esc(p.name) + '">' + esc(T('mgr_open')) + '</button></div>';
  });
  return h;
}

function packView(p) {
  var h = '<div class="row' + (p.enabled ? '' : ' off') + '"><button class="sw' + (p.enabled ? ' on' : '') + '" data-a="toggle" data-p="' + esc(p.name) + '"></button>' +
    '<span class="name">' + esc(p.name) + '</span><span class="meta">' + esc(fmt('mgr_counts', p.images, p.cards)) + '</span></div>';
  h += '<div class="bar2"><input id="rn" value="' + esc(p.name) + '" maxlength="48" placeholder="' + esc(T('mgr_rename_to')) + '">' +
    '<button class="btn" data-a="rename" data-p="' + esc(p.name) + '">' + esc(T('mgr_rename')) + '</button>' +
    '<button class="btn" data-a="export" data-p="' + esc(p.name) + '">' + esc(T('mgr_export')) + '</button></div>';
  var es = [];
  arr(p.entries).forEach(function (e, n) { if (entryHit(e)) es.push({ card: e.card, name: e.name, file: e.file, n: n + 1 }); });
  var pages = Math.max(1, Math.ceil(es.length / PAGE));
  view.page = Math.min(Math.max(view.page, 1), pages);
  h += '<h3>' + esc(fmt('mgr_images', es.length, view.page, pages)) + '</h3>';
  for (var i = (view.page - 1) * PAGE; i < Math.min(es.length, view.page * PAGE); i++) {
    var n = es[i].n, key = 'r' + p.name + '|' + n, on = armed === key, pk = p.name + '|' + n, open = prev.key === pk;
    h += '<div class="row"><span class="name">' + esc(es[i].name || es[i].card) + (es[i].name ? '<span class="sub">' + esc(es[i].card) + '</span>' : '') + '</span><span class="meta">' + esc(es[i].file) + '</span>' +
      '<button class="btn" data-a="preview" data-p="' + esc(p.name) + '" data-i="' + n + '">' + esc(open ? T('mgr_hide') : T('mgr_preview')) + '</button>' +
      '<button class="btn danger' + (on ? ' armed' : '') + '" data-a="remove" data-p="' + esc(p.name) + '" data-i="' + n + '">' + esc(on ? T('mgr_again') : T('mgr_remove')) + '</button></div>';
    if (open) h += '<div class="prev">' + (prev.bad ? esc(T('mgr_bad_gif')) : prev.err ? esc(T('mgr_too_big')) : prev.uri ? '<img src="' + prev.uri + '">' + esc(T(prev.anim ? 'mgr_anim' : 'mgr_art_new')) : '…') + '</div>';
  }
  if (!es.length) h += '<div class="empty">' + esc(T('mgr_no_match')) + '</div>';
  if (pages > 1) h += '<div class="pager"><button class="btn" data-a="page" data-d="-1"' + (view.page <= 1 ? ' disabled' : '') + '>' + esc(T('mgr_prev')) + '</button>' +
    '<span>' + view.page + ' / ' + pages + '</span><button class="btn" data-a="page" data-d="1"' + (view.page >= pages ? ' disabled' : '') + '>' + esc(T('mgr_next')) + '</button></div>';
  var dk = 'd' + p.name;
  h += '<div class="bar2" style="margin-top:16px"><button class="btn" data-a="back">' + esc(T('mgr_back')) + '</button>' +
    '<button class="btn danger' + (armed === dk ? ' armed' : '') + '" data-a="delete" data-p="' + esc(p.name) + '">' + esc(armed === dk ? T('mgr_again') : T('mgr_delete_pack')) + '</button></div>';
  return h;
}

body.addEventListener('click', function (e) {
  var b = e.target.closest ? e.target.closest('[data-a]') : null;
  if (!b || b.disabled) return;
  var a = b.getAttribute('data-a'), p = b.getAttribute('data-p');
  if (a === 'open') { view.pack = p; view.page = 1; armed = null; status = null; render(); }
  else if (a === 'back') { view.pack = null; armed = null; render(); }
  else if (a === 'page') { view.page += parseInt(b.getAttribute('data-d'), 10); armed = null; render(); }
  else if (a === 'preview') {
    var pk = p + '|' + b.getAttribute('data-i');
    if (prev.key === pk) { prev = { key: null, uri: null, err: false }; render(); return; }
    prev = { key: pk, uri: null, err: false }; render();
    kmod.send({ cmd: 'preview', pack: p, i: parseInt(b.getAttribute('data-i'), 10) });
  }
  else if (a === 'toggle') act({ cmd: 'toggle', pack: p });
  else if (a === 'import') act({ cmd: 'import' });
  else if (a === 'export') act({ cmd: 'export', pack: p });
  else if (a === 'rename') act({ cmd: 'rename', pack: p, to: document.getElementById('rn').value });
  else if (a === 'remove' || a === 'delete') {
    var key = a === 'remove' ? 'r' + p + '|' + parseInt(b.getAttribute('data-i'), 10) : 'd' + p;
    if (armed !== key) { armed = key; render(); return; }
    armed = null;
    if (a === 'remove') act({ cmd: 'remove', pack: p, i: parseInt(b.getAttribute('data-i'), 10) });
    else act({ cmd: 'delete', pack: p });
  }
});

kmod.onmessage = function (m) {
  if (m.cmd === 'preview') {
    if (prev.key === m.pack + '|' + m.i) { prev.uri = m.uri || null; prev.err = !!m.err; prev.anim = !!m.anim; prev.bad = !!m.bad; render(); }
    return;
  }
  if (m.cmd === 'init') { S.t = m.t || {}; }
  if (m.cmd === 'init' || m.cmd === 'state') {
    prev = { key: null, uri: null, err: false };
    S.packs = m.packs; status = m.status ? { text: m.status, ok: m.ok !== false } : null;
    if (m.gone) view.pack = null;
    if (m.view) { view.pack = m.view; view.page = 1; }
    render();
  }
};
document.getElementById('q').addEventListener('input', function (e) { q = e.target.value; view.page = 1; render(); });
document.getElementById('x').onclick = function () { kmod.close(); };
kmod.dragMove(document.getElementById('bar'));
kmod.dragResize(document.getElementById('grip'));
render();
