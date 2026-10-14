/*
 * EXT13 Arch-B web-globals stub: a bare JS engine has no web platform
 * surface at all.  An MV3 service worker expects self/caches/indexedDB/
 * fetch/timers/… — these stubs make the context LOOK like a worker
 * global so a real extension script can be evaluated far enough to
 * reveal its chrome.* surface.  Every install is conditional
 * (typeof x === 'undefined') so a runtime that genuinely provides the
 * global keeps its own.
 *
 * Timers are a manual queue — the Rust host has no event loop, so
 * callbacks collect and the host drains them via __drainTimers().
 * Everything records into __apiLog exactly like the chrome shim.
 */
(function () {
  'use strict';
  if (globalThis.__swGlobalsInstalled) return;
  globalThis.__swGlobalsInstalled = true;

  var note = function (op, path) {
    if (globalThis.__apiLog && globalThis.__apiLog.length < 20000)
      globalThis.__apiLog.push(op + ':' + path);
  };

  if (typeof globalThis.self === 'undefined')
    globalThis.self = globalThis;

  /* event-target surface on the global (SW scripts addEventListener) */
  if (typeof globalThis.addEventListener !== 'function') {
    var targets = {};
    globalThis.addEventListener = function (type, fn) {
      note('call', 'globalThis.addEventListener', String(type));
      (targets[type] = targets[type] || []).push(fn);
    };
    globalThis.removeEventListener = function (type, fn) {
      var l = targets[type];
      if (l) { var i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); }
    };
    globalThis.dispatchEvent = function (ev) {
      var l = targets[ev && ev.type] || [];
      l.slice().forEach(function (f) { try { f(ev); } catch (e) {
        globalThis.__errors.push('dispatch ' + ev.type + ': ' + e); } });
      return true;
    };
    globalThis.__swTargets = targets;
  }

  /* fake timers — deterministic manual queue */
  if (typeof globalThis.setTimeout !== 'function') {
    var timerQ = [];
    var nextId = 1;
    globalThis.setTimeout = function (fn, ms) {
      var id = nextId++;
      timerQ.push({ id: id, fn: fn, ms: ms || 0, interval: false });
      return id;
    };
    globalThis.setInterval = function (fn, ms) {
      var id = nextId++;
      timerQ.push({ id: id, fn: fn, ms: ms || 0, interval: true });
      return id;
    };
    globalThis.clearTimeout = globalThis.clearInterval = function (id) {
      timerQ = timerQ.filter(function (t) { return t.id !== id; });
    };
    globalThis.__drainTimers = function (max) {
      var ran = 0;
      var q = globalThis.__timerQ = timerQ;
      for (var i = 0; i < (max || 100) && timerQ.length; i++) {
        var t = timerQ.shift();
        try { t.fn(); ran++; } catch (e) {
          globalThis.__errors.push('timer: ' + e);
        }
        if (t.interval) timerQ.push(t);
      }
      return ran;
    };
  }

  var stubFn = function (path, ret) {
    return function () {
      note('call', path);
      return typeof ret === 'function' ? ret.apply(null, arguments) : ret;
    };
  };

  if (typeof globalThis.fetch !== 'function')
    globalThis.fetch = function (url) {
      note('call', 'fetch', String(url).slice(0, 120));
      return Promise.resolve({
        ok: true, status: 200, url: String(url),
        headers: { get: function () { return null; } },
        text: function () { return Promise.resolve(''); },
        json: function () { return Promise.resolve({}); },
        arrayBuffer: function () { return Promise.resolve(new ArrayBuffer(0)); },
        blob: function () { return Promise.resolve({}); }
      });
    };

  if (typeof globalThis.indexedDB === 'undefined')
    globalThis.indexedDB = {
      open: function (name) {
        note('call', 'indexedDB.open', String(name));
        var req = { result: undefined, error: null };
        setImmediateish(function () {
          req.result = {
            transaction: function () {
              return { objectStore: function () {
                return { get: stubFn('idb.get', {}), put: stubFn('idb.put'),
                         delete: stubFn('idb.delete'), getAll: stubFn('idb.getAll', []) };
              } };
            },
            objectStoreNames: { contains: function () { return false; } },
            createObjectStore: stubFn('idb.createObjectStore', {}),
            close: function () {}
          };
          if (req.onupgradeneeded) { try { req.onupgradeneeded({ target: req }); } catch (e) {} }
          if (req.onsuccess) { try { req.onsuccess({ target: req }); } catch (e) {
            globalThis.__errors.push('idb.onsuccess: ' + e); } }
        });
        return req;
      },
      deleteDatabase: stubFn('indexedDB.deleteDatabase')
    };
  function setImmediateish(fn) {
    if (typeof queueMicrotask === 'function') queueMicrotask(fn);
    else Promise.resolve().then(fn);
  }

  if (typeof globalThis.caches === 'undefined')
    globalThis.caches = {
      open: function (n) {
        note('call', 'caches.open', String(n));
        return Promise.resolve({
          match: function () { return Promise.resolve(undefined); },
          put: function () { return Promise.resolve(); },
          delete: function () { return Promise.resolve(false); },
          keys: function () { return Promise.resolve([]); }
        });
      },
      match: function () { return Promise.resolve(undefined); },
      has: function () { return Promise.resolve(false); },
      delete: function () { return Promise.resolve(false); },
      keys: function () { return Promise.resolve([]); }
    };

  if (typeof globalThis.crypto === 'undefined' || !globalThis.crypto.subtle)
    globalThis.crypto = globalThis.crypto || {};
  if (!globalThis.crypto.subtle)
    globalThis.crypto.subtle = new Proxy({}, { get: function (t, p) {
      note('get', 'crypto.subtle.' + String(p));
      return function () { return Promise.reject(new Error('crypto.subtle stub')); };
    } });
  if (!globalThis.crypto.getRandomValues)
    globalThis.crypto.getRandomValues = function (a) {
      for (var i = 0; i < a.length; i++) a[i] = (i * 1103515245 + 12345) & 0xff;
      return a;
    };

  if (typeof globalThis.atob !== 'function')
    globalThis.atob = function (s) { return ''; };
  if (typeof globalThis.btoa !== 'function')
    globalThis.btoa = function (s) { return ''; };

  if (typeof globalThis.navigator === 'undefined')
    // Extensions sniff the UA to pick their platform device — a bare
    // engine UA maps to nothing, so pose as the Chromium flavour.
    globalThis.navigator = {
      userAgent: 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36'
                 + ' (KHTML, like Gecko) Chrome/139.0.0.0 Safari/537.36',
      language: 'en', onLine: true, hardwareConcurrency: 4 };
  if (typeof globalThis.location === 'undefined')
    globalThis.location = { href: 'chrome-extension://extspike-host/bg.html',
                            origin: 'chrome-extension://extspike-host',
                            protocol: 'chrome-extension:' };
  if (typeof globalThis.URL === 'undefined')
    globalThis.URL = function (u) {
      note('call', 'new URL', String(u).slice(0, 120));
      var m = /^([a-z][a-z0-9+.-]*):\/\/([^\/]*)/i.exec(String(u)) || [];
      return { href: String(u), protocol: (m[1] || '') + ':',
               host: m[2] || '', hostname: m[2] || '', pathname: '/' };
    };
  if (typeof globalThis.Worker === 'undefined')
    globalThis.Worker = function () { note('call', 'new Worker'); return { postMessage: function () {}, terminate: function () {} }; };
  if (typeof globalThis.XMLHttpRequest === 'undefined')
    globalThis.XMLHttpRequest = function () { note('call', 'new XMLHttpRequest'); return { open: stubFn('xhr.open'), send: stubFn('xhr.send'), setRequestHeader: stubFn('xhr.setRequestHeader') }; };
  if (typeof globalThis.WebSocket === 'undefined')
    globalThis.WebSocket = function () { note('call', 'new WebSocket'); return { send: function () {}, close: function () {} }; };
  if (typeof globalThis.importScripts !== 'function')
    globalThis.importScripts = function () { note('call', 'importScripts', String(arguments[0])); };
  if (typeof globalThis.postMessage !== 'function')
    globalThis.postMessage = stubFn('postMessage');
  if (typeof globalThis.DOMParser === 'undefined')
    globalThis.DOMParser = function () {
      this.parseFromString = function () {
        note('call', 'DOMParser.parseFromString');
        return {
          documentElement: {},
          querySelector: function () { return null; },
          querySelectorAll: function () { return []; }
        };
      };
    };
  if (typeof globalThis.Blob === 'undefined')
    globalThis.Blob = function () { note('call', 'new Blob'); return { size: 0, type: '' }; };
  if (typeof globalThis.FileReader === 'undefined')
    globalThis.FileReader = function () { return { readAsArrayBuffer: stubFn('FileReader.readAsArrayBuffer'), readAsText: stubFn('FileReader.readAsText') }; };
  if (typeof globalThis.TextEncoder === 'undefined') {
    globalThis.TextEncoder = function () {
      this.encode = function (s) {
        s = String(s === undefined ? '' : s);
        var out = new Uint8Array(s.length);
        for (var i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
        return out;
      };
      this.encodeInto = function (s, dest) {
        s = String(s === undefined ? '' : s);
        var a = dest || [];
        var n = Math.min(a.length || 0, s.length);
        for (var i = 0; i < n; i++) a[i] = s.charCodeAt(i) & 0xff;
        return { read: n, written: n };
      };
    };
    globalThis.TextDecoder = function () {
      this.decode = function (b) {
        if (b === undefined || b === null) return '';
        var s = '';
        var a = new Uint8Array(b.buffer || b);
        for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
        return s;
      };
    };
  }
  if (typeof globalThis.AbortController === 'undefined')
    globalThis.AbortController = function () {
      this.signal = { aborted: false, addEventListener: function () {} };
      this.abort = function () { this.signal.aborted = true; };
    };
  if (typeof globalThis.clients === 'undefined')
    globalThis.clients = { matchAll: function () { return Promise.resolve([]); },
                           claim: function () { return Promise.resolve(); },
                           openWindow: function () { note('call', 'clients.openWindow'); return Promise.resolve(null); } };
  if (typeof globalThis.registration === 'undefined')
    globalThis.registration = { scope: 'chrome-extension://extspike-host/' };
})();
