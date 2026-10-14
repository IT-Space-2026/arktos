// EXT13 Arch-A packaged host bootstrap.  Extension pages are under the
// MV3 CSP (script-src 'self' — eval() is blocked), so the background
// script is loaded by appending a same-origin <script> element — the
// mechanism a compat layer would use on a real extension too.
window.__hostBoot = 'started';
window.__ticks = 0;
setInterval(function () { window.__ticks++; }, 50);

window.__evalProbe = (function () {
  try { (0, eval)('1'); return 'eval-ok'; }
  catch (e) { return 'eval-blocked:' + e; }
})();

var s = document.createElement('script');
s.src = 'sw.js';                     // same directory as host.html
s.onload = function () { window.__hostBoot = 'sw.js-loaded'; };
s.onerror = function (e) { window.__hostBoot = 'sw.js-FAILED:' + e; };
document.head.appendChild(s);
