/* the square cropper: pan + zoom the picture under a fixed 1:1 frame; the result goes back to Lua as JPEG text in small acknowledged chunks */
var $ = function (id) { return document.getElementById(id); };
var T = {}, img = $('img'), stage = $('stage'), S = 300;
var rectMode = false, nw = 0, nh = 0, s = 1, s0 = 1, tx = 0, ty = 0, busy = false;
var OUT = 1024, CHUNK = 3000, MAXZ = 8;

function layout() {
  var w = $('wrap'), side = Math.max(120, Math.min(w.clientWidth, w.clientHeight) - 20);
  S = Math.floor(side); stage.style.width = stage.style.height = S + 'px';
  if (nw) fit(false);
}
function clampPos() {
  tx = Math.min(0, Math.max(S - nw * s, tx)); ty = Math.min(0, Math.max(S - nh * s, ty));
}
function apply() {
  clampPos();
  img.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + s + ')';
  $('zoom').value = Math.round(Math.log(s / s0) / Math.log(MAXZ) * 1000);
}
function fit(reset) {
  var old = s0; s0 = S / Math.min(nw, nh);
  if (reset) { s = s0; tx = (S - nw * s) / 2; ty = (S - nh * s) / 2; } else { s = Math.max(s0, s * s0 / old); }
  apply();
}
/* zoom around the point (px, py) of the frame */
function zoomAt(ns, px, py) {
  ns = Math.min(s0 * MAXZ, Math.max(s0, ns));
  tx = px - (px - tx) * ns / s; ty = py - (py - ty) * ns / s; s = ns; apply();
}
function k() { return stage.getBoundingClientRect().width / stage.offsetWidth || 1; }   /* client pixels per CSS pixel (the desktop page is zoomed) */

var ptrs = {}, last = null;
stage.addEventListener('pointerdown', function (e) {
  stage.setPointerCapture(e.pointerId); ptrs[e.pointerId] = { x: e.clientX, y: e.clientY }; last = null;
});
function up(e) { delete ptrs[e.pointerId]; last = null; }
stage.addEventListener('pointerup', up); stage.addEventListener('pointercancel', up);
stage.addEventListener('pointermove', function (e) {
  var p = ptrs[e.pointerId]; if (!p) return;
  var ids = Object.keys(ptrs), kk = k();
  if (ids.length === 1) {
    tx += (e.clientX - p.x) / kk; ty += (e.clientY - p.y) / kk; p.x = e.clientX; p.y = e.clientY; apply();
  } else if (ids.length >= 2) {
    p.x = e.clientX; p.y = e.clientY;
    var a = ptrs[ids[0]], b = ptrs[ids[1]];
    var d = Math.hypot(a.x - b.x, a.y - b.y), cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    if (last) {
      var r = stage.getBoundingClientRect();
      tx += (cx - last.cx) / kk; ty += (cy - last.cy) / kk;
      if (last.d > 0) zoomAt(s * d / last.d, (cx - r.left) / kk, (cy - r.top) / kk);
      else apply();
    }
    last = { d: d, cx: cx, cy: cy };
  }
});
stage.addEventListener('wheel', function (e) {
  e.preventDefault();
  var r = stage.getBoundingClientRect(), kk = k();
  zoomAt(s * Math.exp(-e.deltaY * 0.0015), (e.clientX - r.left) / kk, (e.clientY - r.top) / kk);
}, { passive: false });
$('zoom').addEventListener('input', function (e) {
  zoomAt(s0 * Math.pow(MAXZ, e.target.value / 1000), S / 2, S / 2);
});

function status(t) { $('hint').textContent = t; }
function setBusy(b) { busy = b; $('ok').disabled = b; $('cancel').disabled = b; }

$('ok').onclick = function () {
  if (busy || !nw) return;
  if (rectMode) {        /* an animated picture: the crop is only remembered (the loader cuts every frame) */
    var side = S / s;
    setBusy(true); kmod.send({ cmd: 'rect', x: Math.round(-tx / s), y: Math.round(-ty / s), s: Math.round(side) });
    return;
  }
  setBusy(true); status(T.crop_working || '…');
  setTimeout(function () {
    var vis = S / s, out = Math.max(256, Math.min(OUT, Math.round(vis)));
    var c = document.createElement('canvas'); c.width = c.height = out;
    var ctx = c.getContext('2d'); ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, out, out);
    ctx.drawImage(img, -tx / s, -ty / s, vis, vis, 0, 0, out, out);
    var data = c.toDataURL('image/jpeg', 0.92).split(',')[1] || '';
    var n = Math.ceil(data.length / CHUNK), i = 0;
    window.sendNext = function () {
      if (i >= n) { if (kmod.fast) kmod.fast(false); kmod.send({ cmd: 'done', n: n }); return; }
      status((T.crop_sending || '') + ' ' + Math.round(i * 100 / n) + '%');
      kmod.send({ cmd: 'chunk', i: i, n: n, data: data.substr(i * CHUNK, CHUNK) }); i++;
    };
    if (kmod.fast) kmod.fast(true);
    window.sendNext();
  }, 30);
};
$('cancel').onclick = function () { if (!busy) kmod.send({ cmd: 'cancel' }); };
$('x').onclick = function () { kmod.close(); };

kmod.onmessage = function (m) {
  if (m.cmd === 'ack') { if (window.sendNext) window.sendNext(); return; }
  if (m.cmd === 'init') {
    T = m.t || {}; rectMode = !!m.rect;
    $('title').textContent = T.crop_title || ''; $('hint').textContent = T.crop_hint || '';
    $('zl').textContent = T.crop_zoom || ''; $('ok').textContent = T.crop_confirm || 'OK'; $('cancel').textContent = T.crop_cancel || 'Cancel';
    if (m.err) { status(m.err); $('ok').disabled = true; return; }
    img.onload = function () { nw = img.naturalWidth; nh = img.naturalHeight; layout(); fit(true); };
    img.onerror = function () { status(T.crop_bad || 'cannot open the picture'); $('ok').disabled = true; };
    img.src = m.uri;
  }
};
addEventListener('resize', layout);
kmod.dragMove($('bar'));
layout();
