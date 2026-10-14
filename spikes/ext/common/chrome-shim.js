/*
 * EXT13 shared chrome.* compat shim + API-surface recorder.
 *
 * Two modes, selected by the host setting globalThis.__CHROME_SHIM_MODE
 * before evaluating this file:
 *
 *   'full'    — the runtime has no chrome object at all (Rust JS host).
 *               chrome.* is synthesized wholesale: generic deep-proxy
 *               stubs for every namespace plus real pub/sub for
 *               runtime.onMessage/sendMessage/onConnect and a real
 *               object-backed storage.local so round-trips are honest.
 *
 *   'gapfill' — a real engine chrome object exists (Qt WebEngine
 *               extension page) but namespaces are missing (EXT11
 *               matrix).  Only the absent members are overlaid with
 *               logging stubs; everything the engine provides stays
 *               real.
 *
 * Both modes append every chrome.* touch to globalThis.__apiLog as
 * "<op>:<dotted.path>" lines — that log IS the compat-surface
 * measurement.  __apiLog is capped so a hostile script cannot grow it
 * without bound.
 *
 * Host protocol (both architectures):
 *   __runPayload(src, label)  — indirect-eval the script under
 *                               try/catch; returns {ok,label,error?}.
 *   __fireMessage(msg)        — invoke every registered
 *                               chrome.runtime.onMessage listener with
 *                               the message; returns the array of
 *                               sendResponse payloads (the round-trip).
 *   __report()                — JSON {apiLog, errors, listeners}.
 */
(function () {
  'use strict';
  if (globalThis.__chromeShimInstalled)
    return;
  globalThis.__chromeShimInstalled = true;

  var MODE = globalThis.__CHROME_SHIM_MODE || 'full';
  var LOG_CAP = 20000;
  var apiLog = [];
  var errors = [];
  var seen = {};
  globalThis.__apiLog = apiLog;
  globalThis.__errors = errors;

  function note(op, path, detail) {
    var key = op + ':' + path;
    if (seen[key]) {
      // count repeats on the same entry so hot paths stay visible
      var idx = seen[key];
      apiLog[idx] = key + ' x' + (++seen[key + '#n']);
      return;
    }
    if (apiLog.length >= LOG_CAP) return;
    seen[key] = apiLog.length;
    seen[key + '#n'] = 1;
    apiLog.push(key + (detail ? ' ' + detail : ''));
  }

  /* Deep generic stub: any property touch/call is recorded and returns
   * another stub so arbitrary chains survive. */
  var stubCache = {};
  function makeStub(path) {
    if (stubCache[path]) return stubCache[path];
    var fn = function () {
      note('call', path, arguments.length + 'args');
      return makeStub(path + '()');
    };
    var proxy = new Proxy(fn, {
      get: function (t, prop) {
        if (prop === Symbol.toPrimitive)
          return function () { return '[stub ' + path + ']'; };
        if (prop === 'then') return undefined;      // stay non-thenable
        if (prop === Symbol.iterator)
          return function () { return [][Symbol.iterator](); };
        if (prop === 'addListener' || prop === 'addRules' ||
            prop === 'removeListener' || prop === 'hasListener')
          return function () {
            note('call', path + '.' + String(prop),
                 (typeof arguments[0] === 'function') ? 'fn' : 'args');
            return undefined;
          };
        var p = path + '.' + String(prop);
        note('get', p);
        return makeStub(p);
      },
      set: function (t, prop, v) {
        note('set', path + '.' + String(prop));
        return true;
      },
      has: function (t, prop) {
        note('has', path + '.' + String(prop));
        return true;   // claim everything exists — maximal-survival probe
      },
      construct: function () {
        note('new', path);
        return makeStub(path + '#new');
      }
    });
    stubCache[path] = proxy;
    return proxy;
  }

  /* Minimal event-emitter matching the chrome.* Event contract the
   * way extensions actually use it. */
  function makeEvent(path) {
    var listeners = [];
    return {
      addListener: function (fn) {
        note('call', path + '.addListener');
        listeners.push(fn);
      },
      removeListener: function (fn) {
        var i = listeners.indexOf(fn);
        if (i >= 0) listeners.splice(i, 1);
      },
      hasListener: function (fn) { return listeners.indexOf(fn) >= 0; },
      __fire: function (args, respond) {
        var replies = [];
        for (var i = 0; i < listeners.length; i++) {
          try {
            var r = listeners[i].apply(null, args.concat([respond]));
            if (r !== undefined) replies.push(r);
          } catch (e) { errors.push('listener ' + path + ': ' + e); }
        }
        return replies;
      },
      __count: function () { return listeners.length; }
    };
  }

  function buildFullChrome() {
    var store = {};
    var onMessage = makeEvent('chrome.runtime.onMessage');
    var onInstalled = makeEvent('chrome.runtime.onInstalled');
    var onConnect = makeEvent('chrome.runtime.onConnect');
    var onStartup = makeEvent('chrome.runtime.onStartup');
    var listeners = {
      onMessage: onMessage, onInstalled: onInstalled,
      onConnect: onConnect, onStartup: onStartup
    };

    var runtime = {
      id: 'extspike-host',
      getManifest: function () { note('call', 'chrome.runtime.getManifest'); return {}; },
      getURL: function (p) { note('call', 'chrome.runtime.getURL', String(p)); return 'chrome-extension://extspike-host/' + p; },
      getBackgroundPage: function (cb) { note('call', 'chrome.runtime.getBackgroundPage'); if (cb) cb(globalThis); },
      getContexts: function () { note('call', 'chrome.runtime.getContexts'); return []; },
      onMessage: onMessage, onInstalled: onInstalled,
      onConnect: onConnect, onStartup: onStartup,
      sendMessage: function (msg, opts, cb) {
        if (typeof opts === 'function') { cb = opts; opts = undefined; }
        note('call', 'chrome.runtime.sendMessage');
        var replies = onMessage.__fire([msg, { id: 'self' }], function (r) {
          if (cb) cb(r);
        });
        if (cb && !replies.length)
          cb(undefined);   // nobody answered — honest empty
        return Promise.resolve(replies[0]);
      },
      lastError: null
    };

    var storageArea = function (areaPath, backing) {
      return {
        get: function (keys, cb) {
          note('call', areaPath + '.get');
          var out = {};
          if (keys == null) { for (var k in backing) out[k] = backing[k]; }
          else if (typeof keys === 'string') {
            if (k in backing) out[k] = backing[k];
            else if (keys in backing) out[keys] = backing[keys];
          } else if (Array.isArray(keys)) {
            keys.forEach(function (k) { if (k in backing) out[k] = backing[k]; });
          }
          if (cb) cb(out);
          return Promise.resolve(out);
        },
        set: function (items, cb) {
          note('call', areaPath + '.set');
          for (var k in items) backing[k] = items[k];
          if (cb) cb();
          return Promise.resolve();
        },
        remove: function (keys, cb) {
          note('call', areaPath + '.remove');
          (Array.isArray(keys) ? keys : [keys]).forEach(function (k) { delete backing[k]; });
          if (cb) cb();
          return Promise.resolve();
        },
        clear: function (cb) {
          for (var k in backing) delete backing[k];
          if (cb) cb();
          return Promise.resolve();
        },
        onChanged: makeEvent(areaPath + '.onChanged')
      };
    };

    var chrome = {
      runtime: runtime,
      extension: {   // the legacy namespace Bitwarden's popup needs
        getBackgroundPage: runtime.getBackgroundPage,
        getViews: function () { note('call', 'chrome.extension.getViews'); return []; },
        inIncognitoContext: false,
        isAllowedIncognitoAccess: function (cb) { if (cb) cb(false); },
        isAllowedFileSchemeAccess: function (cb) { if (cb) cb(false); }
      },
      storage: {
        local: storageArea('chrome.storage.local', store),
        session: storageArea('chrome.storage.session', {}),
        sync: storageArea('chrome.storage.sync', {}),
        managed: storageArea('chrome.storage.managed', {}),
        onChanged: makeEvent('chrome.storage.onChanged')
      },
      tabs: makeStub('chrome.tabs'),
      windows: makeStub('chrome.windows'),
      alarms: makeStub('chrome.alarms'),
      i18n: {
        getMessage: function (n) { note('call', 'chrome.i18n.getMessage', n); return n; },
        getUILanguage: function () { return 'en'; }
      }
    };
    // everything else resolves through the generic deep stub
    return new Proxy(chrome, {
      get: function (t, prop) {
        if (typeof prop !== 'string') return t[prop];
        if (prop in t) return t[prop];
        note('get', 'chrome.' + prop);
        var s = makeStub('chrome.' + prop);
        t[prop] = s;
        return s;
      },
      set: function (t, prop, v) { t[prop] = v; return true; },
      has: function () { return true; }
    });
  }

  function gapfill(realChrome) {
    /* Overlay only the namespaces EXT11 measured missing on Qt 6.12.
     * Real members are preserved by wrapping, not replacing. */
    var missingTop = ['webNavigation', 'contextMenus', 'sidePanel',
      'notifications', 'declarativeNetRequest', 'permissions', 'action',
      'windows', 'cookies', 'bookmarks', 'identity', 'gcm',
      'nativeMessaging', 'privacy', 'topSites', 'history', 'scripting',
      'offscreen', 'omnibox', 'commands', 'devtools', 'fontSettings',
      'tabGroups', 'userScripts', 'webRequest'];
    var realTabs = realChrome.tabs || {};
    realChrome.tabs = new Proxy(realTabs, {
      get: function (t, prop) {
        if (prop in t) return t[prop];
        var p = 'chrome.tabs.' + String(prop);
        note('get', p);
        var s = makeStub(p);
        if (String(prop).indexOf('on') === 0)
          s = { addListener: function (f) { note('call', p + '.addListener'); },
                removeListener: function () {}, hasListener: function () { return false; } };
        return s;
      },
      set: function (t, p, v) { t[p] = v; return true; }
    });
    if (!realChrome.extension) {
      realChrome.extension = {
        getBackgroundPage: function (cb) {
          note('call', 'chrome.extension.getBackgroundPage');
          var w = makeStub('chrome.extension.getBackgroundPage()');
          if (cb) cb(w);
          return w;
        },
        getViews: function (o) {
          note('call', 'chrome.extension.getViews');
          return [];
        },
        inIncognitoContext: false,
        isAllowedIncognitoAccess: function (cb) { if (cb) cb(false); },
        isAllowedFileSchemeAccess: function (cb) { if (cb) cb(false); },
        lastError: null
      };
    }
    if (realChrome.tabs && !realChrome.tabs.getCurrent)
      realChrome.tabs.getCurrent = function (cb) {
        note('call', 'chrome.tabs.getCurrent');
        var tab = { id: 0, index: 0, windowId: 0, active: true,
                    url: 'chrome-extension://' + (realChrome.runtime && realChrome.runtime.id) + '/' };
        if (cb) cb(tab);
        return Promise.resolve(tab);
      };
    missingTop.forEach(function (ns) {
      if (realChrome[ns] === undefined)
        realChrome[ns] = makeStub('chrome.' + ns);
    });
    return realChrome;
  }

  var listenersRegistry = { onMessage: null };

  if (MODE === 'full') {
    globalThis.chrome = buildFullChrome();
    listenersRegistry.onMessage = globalThis.chrome.runtime.onMessage;
  } else {
    var c = globalThis.chrome;
    if (c) {
      gapfill(c);
      if (c.runtime && c.runtime.onMessage)
        listenersRegistry.onMessage = c.runtime.onMessage;   // real event
    }
  }
  globalThis.__shimListeners = listenersRegistry;

  /* Host protocol ------------------------------------------------ */
  globalThis.__runPayload = function (src, label) {
    try {
      (0, eval)(src);
      return { ok: true, label: label };
    } catch (e) {
      var msg = String(e) + ' || ' +
                String(e && e.stack || '').split('\n').slice(0, 6).join(' | ');
      errors.push(label + ': ' + msg);
      return { ok: false, label: label, error: msg };
    }
  };

  /* Parse without executing — Function() compiles the body but does
   * not run it.  Hosts that SIGSEGV on execution can still answer
   * "does it parse". */
  globalThis.__parseOnly = function (src, label) {
    try {
      (0, Function)(src);
      return { ok: true, label: label, parseOnly: true };
    } catch (e) {
      return { ok: false, label: label, parseOnly: true,
               error: String(e) };
    }
  };

  globalThis.__fireMessage = function (msg) {
    var responses = [];
    globalThis.__fireResponses = responses;
    var fire = listenersRegistry.onMessage &&
               listenersRegistry.onMessage.__fire;
    if (fire) {
      fire.call(listenersRegistry.onMessage, [msg, { id: 'host' }],
                function (r) { responses.push(r); });
    } else {
      errors.push('__fireMessage: no onMessage registry');
    }
    return responses;
  };

  globalThis.__report = function () {
    var counts = {};
    for (var k in listenersRegistry) {
      var l = listenersRegistry[k];
      counts[k] = l && l.__count ? l.__count() : -1;
    }
    return JSON.stringify({
      apiLog: apiLog, errors: errors, listeners: counts
    });
  };
})();
