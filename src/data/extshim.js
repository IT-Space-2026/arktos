/*
 * EXT15 chrome.* core API shim — runs inside the same per-extension
 * DocumentCreation injection as exthost.js (MainWorld, hosted page at
 * the extension's own chrome-extension://<id>/ origin), after the
 * preamble and before the extension's background scripts.
 *
 * The hosted page already has REAL chrome.runtime / chrome.storage /
 * chrome.i18n bindings (EXT13 measurement) — the shim is a GAPFILL
 * layer over the EXT13-measured 27-entry delta, not a re-skin of what
 * Chromium provides.  Real members are kept; missing or present-but-
 * inert surfaces are replaced by bridge-backed implementations that
 * answer through the 'chromeBridge' QWebChannel object.
 *
 * Contract (spec c): every API method either works or throws a TYPED
 * error the shim surfaces to ExtensionInfo::health — recorded into
 * __aroraShim.gaps and forwarded via invoke('shim','gap').  APIs that
 * are legitimately absent (no browser surface exists) answer
 * {ok:false,error:'unsupported'} the same way — nothing silently
 * no-ops.
 *
 * Namespaces:
 *   chrome.runtime      — real; sendMessage gains a bridge relay for
 *                         hosted->hosted / hosted->popup contexts,
 *                         onMessage wrappers dedupe the two paths
 *   chrome.storage      — real local/session kept when present (it is
 *                         the store the still-running real service
 *                         worker shares); a per-extension bridge
 *                         store backs contexts without bindings
 *   chrome.extension    — getURL/getViews/getBackgroundPage mapped to
 *                         the hosted context
 *   chrome.i18n         — getMessage wrapped unconditionally: the
 *                         native binding answers '' for every key;
 *                         misses fall to the _locales/<lang>/
 *                         messages.json map embedded in the injected
 *                         config (bridge fetch as backup)
 *   chrome.alarms       — bridge-backed (host timers) — the inert
 *                         binding is replaced so onAlarm really fires
 *   chrome.action /
 *   chrome.browserAction— title/badge setters feed the EXT08 button
 *                         state registry
 *   chrome.tabs         — getCurrent/query/get/create/update over a
 *                         host-side window/tab snapshot; on* events
 *                         are shim events EXT16 pushes into
 *   chrome.webNavigation— event objects (delivery is EXT16)
 *   chrome.permissions  — contains/getAll answered from the manifest
 *   chrome.notifications— event objects; create() is a typed
 *                         'unsupported' (no notification surface)
 *   chrome.commands     — onCommand event; getAll from the manifest
 *   chrome.contextMenus — create/update/remove* registry host-side;
 *                         onClicked event (rendering is EXT16)
 *   chrome.sidePanel    — options recorded host-side; no panel UI
 *   other absent ns     — unsupportedNs(): calls answer typed errors,
 *                         on* event registration is recorded
 *
 * Keep this file dependency-free and ES5-plus-Promise: it is inlined
 * into a QWebEngineScript source.
 */
(function () {
    'use strict';
    var host = window.__aroraHost;
    var cfg = window.__ARORA_HOST_CONFIG;
    if (!host || !cfg || !cfg.id || window.__aroraShim)
        return;

    var LOG_CAP = 500;
    var shim = window.__aroraShim = {
        version: 3,
        gaps: [],       // typed errors observed (health surface)
        notes: [],      // benign diagnostics (recorded registrations,
                        // accepted-but-unsourced state)
        touched: {},    // api -> count (the live delta measurement)
        events: {},     // name -> event objects the host pushes into
        seenMids: {},   // relay/native message dedupe (bounded TTL)
        recentDeliveries: {}, // content-hash dedupe for relay+native
        seq: 0
    };

    function note(text) {
        if (shim.notes.length < LOG_CAP)
            shim.notes.push(String(text));
        if (host.bridgeUp && host.bridge) {
            try {
                host.bridge.invoke('shim', 'note',
                    { message: String(text) }, function () {});
            } catch (e) {}
        }
    }

    function recordGap(api, error) {
        var text = api + ': ' + error;
        for (var i = 0; i < shim.gaps.length; ++i)
            if (shim.gaps[i] === text)
                return;
        if (shim.gaps.length < LOG_CAP)
            shim.gaps.push(text);
        if (host.bridgeUp && host.bridge) {
            try {
                host.bridge.invoke('shim', 'gap',
                    { api: String(api), error: String(error) },
                    function () {});
            } catch (e) {}
        }
    }

    function touch(api) {
        shim.touched[api] = (shim.touched[api] || 0) + 1;
    }

    // --- bridge plumbing -------------------------------------------
    // host.bridge is set by exthost.js once the QWebChannel handshake
    // lands; calls made before then queue and flush on arrival.  A
    // call that waits past the watchdog rejects typed rather than
    // hanging the extension's startup forever.
    var bridgeQueue = [];
    var bridgeWatch = null;
    var hostEventWired = false;

    function flushBridgeQueue() {
        var q = bridgeQueue;
        bridgeQueue = [];
        for (var i = 0; i < q.length; ++i)
            q[i](host.bridge);
    }

    function onHostEvent(name, payload) {
        // Host->page pushes: relay envelopes/responses and (from
        // EXT16) real event deliveries into the shim event registry.
        if (name === '__arora.relay') {
            onRelayMessage(payload);
            return;
        }
        if (name === '__arora.relayResponse') {
            onRelayResponse(payload);
            return;
        }
        if (name === '__arora.portConnect') {
            // A sibling context offered a port — accept it on behalf
            // of this context's runtime.onConnect listeners.
            if (payload && payload.chan) {
                var localId = 'p' + (++shim.seq) + '#' + Date.now();
                var port = newPort(payload.name || '', payload.chan,
                                   localId, payload.sender);
                call('relay', 'portAccept',
                     { chan: payload.chan, portId: localId })
                    .catch(function () {});
                var connEv = shim.events['runtime.onConnect'];
                if (connEv && connEv.__fire)
                    connEv.__fire([port]);
            }
            return;
        }
        if (name === '__arora.portMessage') {
            if (payload && shim.ports[payload.portId])
                shim.ports[payload.portId].onMessage
                    .__fire([payload.msg,
                             shim.ports[payload.portId]]);
            return;
        }
        if (name === '__arora.portDisconnect') {
            if (payload && shim.ports[payload.portId]) {
                var dp = shim.ports[payload.portId];
                dp.disconnected = true;
                delete shim.ports[payload.portId];
                dp.onDisconnect.__fire([dp]);
            }
            return;
        }
        var ev = shim.events[name];
        if (!ev && name.indexOf('chrome.') === 0)
            ev = shim.events[name.slice('chrome.'.length)];
        if (!ev)
            ev = shim.events['chrome.' + name];
        if (ev && ev.__fire) {
            // EXT16 multi-arg convention: the host spreads listener
            // arguments through __args ([tabId, changeInfo, tab]
            // for tabs.onUpdated).  Named-object payloads without
            // it still fan out through the arg-order table so an
            // event lands correctly either way; a plain scalar
            // payload stays a single argument.
            var args;
            if (payload && payload.__args !== undefined)
                args = payload.__args;
            else if (payload && ARG_KEYS[name])
                args = ARG_KEYS[name].map(function (k) {
                    return payload[k];
                });
            else
                args = payload === undefined ? [] : [payload];
            ev.__fire(args);
        }
    }

    // Fallback argument order for events the host pushes as a named
    // object rather than an __args array.
    var ARG_KEYS = {
        'tabs.onCreated': ['tab'],
        'tabs.onUpdated': ['tabId', 'changeInfo', 'tab'],
        'tabs.onMoved': ['tabId', 'moveInfo'],
        'tabs.onActivated': ['tabId', 'windowId'],
        'tabs.onHighlighted': ['highlightInfo'],
        'tabs.onDetached': ['tabId', 'detachInfo'],
        'tabs.onAttached': ['tabId', 'attachInfo'],
        'tabs.onRemoved': ['tabId', 'removeInfo'],
        'tabs.onReplaced': ['addedTabId', 'removedTabId'],
        'tabs.onZoomChange': ['zoomInfo'],
        'windows.onCreated': ['window'],
        'windows.onRemoved': ['windowId'],
        'windows.onFocusChanged': ['windowId'],
        'contextMenus.onClicked': ['info', 'tab'],
        'contextMenus.onShown': ['info', 'tab']
    };

    // EXT17 context kind — 'background' is the hidden host page;
    // a sibling document is 'popup' when its URL is the manifest's
    // declared action popup, else 'page' (options tab, offscreen
    // document — reported as TAB by getContexts).
    function contextKind() {
        if (host.kind === 'background')
            return 'background';
        var path = '';
        try {
            path = decodeURIComponent(String(location.pathname))
                .replace(/^\/+/, '');
        } catch (e) {
            path = String(location.pathname).replace(/^\/+/, '');
        }
        var m = manifest();
        var decls = [m.action, m.browser_action, m.page_action];
        for (var i = 0; i < decls.length; ++i) {
            if (decls[i] && decls[i].default_popup === path)
                return 'popup';
        }
        return 'page';
    }

    // chrome.extension.getViews/getBackgroundPage answers that are
    // honest in every context kind — a sibling page cannot return
    // the background's real Window object, so it reports its own
    // type and answers null with a recorded gap rather than handing
    // back the wrong window.
    function thisViewType() {
        return host.kind === 'background'
            ? 'BACKGROUND'
            : (contextKind() === 'popup' ? 'POPUP' : 'TAB');
    }
    function extensionGetViews(fetchProperties) {
        touch('chrome.extension.getViews');
        var type = fetchProperties && fetchProperties.type;
        if (!type || type === thisViewType())
            return [window];
        note('chrome.extension.getViews(' + type + '): only the '
             + thisViewType() + ' context is enumerable here');
        return [];
    }
    function extensionGetBackgroundPage(cb) {
        touch('chrome.extension.getBackgroundPage');
        if (host.kind === 'background') {
            if (cb) {
                try { cb(window); } catch (e) {}
            }
            return window;
        }
        if (manifest().manifest_version === 3)
            // Chrome itself answers null under MV3 — there is no
            // persistent background page to hand back.  Not a gap.
            note('chrome.extension.getBackgroundPage: null under '
                 + 'MV3 matches Chrome (service worker, no window)');
        else
            recordGap('chrome.extension.getBackgroundPage',
                      'cross-context');
        if (cb) {
            try { cb(null); } catch (e) {}
        }
        return null;
    }

    function wireHostEvent() {
        if (hostEventWired || !host.bridge)
            return;
        hostEventWired = true;
        try {
            if (host.bridge.hostEvent
                    && host.bridge.hostEvent.connect)
                host.bridge.hostEvent.connect(onHostEvent);
        } catch (e) {
            recordGap('chromeBridge.hostEvent', String(e));
        }
        // Register this hosted context with the runtime's message
        // relay — sibling shimmed contexts (other hosted pages,
        // EXT17 popups/options pages) reach us via hostEvent.  The
        // kind+url feed chrome.runtime.getContexts.
        call('relay', 'register',
             { kind: contextKind(), url: String(location.href) })
            .then(function (id) { shim.contextId = id; })
            .catch(function () {});
    }

    function call(ns, method, args) {
        touch(ns + '.' + method);
        return new Promise(function (resolve, reject) {
            var done = false;
            var watchdog = setTimeout(function () {
                if (!done) {
                    done = true;
                    reject(typedError('bridge-timeout',
                                      ns + '.' + method));
                }
            }, 30000);
            var issue = function (bridge) {
                try {
                    bridge.invoke(ns, method, args || {},
                        function (reply) {
                            if (done)
                                return;
                            done = true;
                            clearTimeout(watchdog);
                            if (reply && reply.ok)
                                resolve(reply.result);
                            else
                                reject(typedError(
                                    reply && reply.error
                                        ? String(reply.error)
                                        : 'bridge-error',
                                    ns + '.' + method));
                        });
                } catch (e) {
                    if (!done) {
                        done = true;
                        clearTimeout(watchdog);
                        reject(typedError('bridge-threw',
                                          ns + '.' + method));
                    }
                }
            };
            if (host.bridgeUp && host.bridge) {
                wireHostEvent();   // idempotent — hostEvent must be
                                   // connected before any push lands
                issue(host.bridge);
            } else {
                bridgeQueue.push(issue);
            }
        });
    }

    // Eager hook — exthost.js calls this the instant the handshake
    // reply lands.  A setTimeout poll here would lose the race:
    // hidden-page timers are clamped to ~1s, so hostEvent.connect()
    // could land after the first push and drop it.
    host.onBridgeUp = function () {
        wireHostEvent();
        flushBridgeQueue();
    };
    (function waitBridge() {
        if (host.bridgeUp && host.bridge) {
            wireHostEvent();
            flushBridgeQueue();
            return;
        }
        if (host.state === 'ready'
                || String(host.state).indexOf('failed') === 0)
            return;   // ready without bridge: channel-less context;
                      // failed:<reason> — stop polling, calls queue
                      // into the watchdog instead of spinning.
        setTimeout(waitBridge, 50);
    })();

    function typedError(code, api) {
        var name = api.indexOf('chrome.') === 0 ? api
                                                 : 'chrome.' + api;
        var e = new Error(name + ': ' + code);
        e.code = code;
        e.api = api;
        recordGap(name, code);
        return e;
    }

    // The extension's raw manifest.json — carried by the chromeBridge
    // handshake reply so synchronous lookups (permissions, commands,
    // getManifest fallback, default_locale) never block on a call.
    function manifest() {
        return (host.bridgeReply && host.bridgeReply.manifest) || {};
    }

    function setLastError(message) {
        // chrome.runtime.lastError is a read-only getter when the
        // real bindings exist — best-effort assign, else park the
        // message where the shim (and tests) can still read it.
        try { chrome.runtime.lastError = { message: message }; }
        catch (e) {}
        shim.lastError = { message: message };
    }

    function clearLastError() {
        // Chrome semantics: lastError exists only for the duration of
        // the failing call's callback.  Where the binding exposes it
        // as a plain writable property a set survives its callback
        // and poisons every later lastError read — a real extension's
        // storage.get retried forever on exactly this.  Clear
        // explicitly; on the read-only getter the assign no-ops.
        try { chrome.runtime.lastError = undefined; } catch (e) {}
        shim.lastError = null;
    }

    // The one funnel every shim-callback answer goes through: the
    // error is visible inside cb exactly like Chrome scopes it, then
    // cleared; a success answer clears any residue first.
    function answerCb(cb, value, message) {
        if (!cb)
            return;
        if (message) setLastError(message);
        else clearLastError();
        try { cb(value); } catch (e) {}
        if (message) clearLastError();
    }

    // A chrome.* method dual-call convention: trailing function arg
    // is the callback; the call ALSO returns a promise (MV3).  fail()
    // honours both: cb(undefined)+lastError AND a rejected promise —
    // the typed error still reaches health via recordGap.
    function failCall(api, code, cb) {
        var err = typedError(code, api);
        answerCb(cb, undefined, err.message);
        return Promise.reject(err);
    }

    function okCall(value, cb) {
        answerCb(cb, value);
        return Promise.resolve(value);
    }

    // --- shim event factory ------------------------------------------
    function makeEvent(name, opts) {
        opts = opts || {};
        var listeners = [];
        var ev = {
            addListener: function (fn) {
                if (typeof fn === 'function'
                        && listeners.indexOf(fn) < 0) {
                    listeners.push(fn);
                    if (opts.onAdd)
                        opts.onAdd(fn);
                }
            },
            removeListener: function (fn) {
                var i = listeners.indexOf(fn);
                if (i >= 0)
                    listeners.splice(i, 1);
            },
            hasListener: function (fn) {
                return listeners.indexOf(fn) >= 0;
            },
            hasListeners: function () {
                return listeners.length > 0;
            }
        };
        ev.__fire = function (args) {
            var replies = [];
            for (var i = 0; i < listeners.length; ++i) {
                try {
                    var r = listeners[i].apply(null, args);
                    if (r !== undefined)
                        replies.push(r);
                } catch (e) {
                    record('shim event ' + name + ' listener: ' + e);
                }
            }
            return replies;
        };
        ev.__count = function () { return listeners.length; };
        shim.events[name] = ev;
        return ev;
    }

    function record(what) {
        var text = String(what && what.message || what);
        var stack = what && what.stack;
        if (stack)
            text += ' @' + String(stack).split('\n')
                    .slice(1, 4).join(' <-');
        if (host.errors.length < 200)
            host.errors.push(text);
        if (host.bridgeUp && host.bridge) {
            try { host.bridge.reportError(text); } catch (e) {}
        }
    }

    // --- namespace installation ----------------------------------------
    // Qt's chrome.* bindings are a SUBSET and some namespace
    // properties resist replacement (read-only accessors, setter
    // no-ops — Bitwarden's hosted page kept a native chrome.tabs).
    // safeSet never lets a locked property throw through the shim
    // init; installNs replaces a namespace when it can and otherwise
    // fills the native object's missing members one by one.
    function safeSet(obj, key, val, api) {
        try {
            obj[key] = val;
            return true;
        } catch (e) {
            if (api)
                recordGap(api, String(e));
            return false;
        }
    }

    // Namespaces installed through installNs are tracked so the
    // post-bootstrap repair pass can re-assert them — Chromium
    // lazily REDEFINES api bindings after DocumentCreation (the
    // hosted Bitwarden page swapped our chrome.tabs proxy for the
    // native subset mid-boot).
    var installedNs = {};

    function installNs(name, obj, repairable) {
        if (repairable !== false)
            installedNs[name] = obj;
        var cur;
        try { cur = chrome[name]; } catch (e) { cur = undefined; }
        if (cur === obj)
            return;
        if (safeSet(chrome, name, obj, 'chrome.' + name)) {
            var now;
            try { now = chrome[name]; } catch (e) { now = cur; }
            if (now === obj)
                return;
        }
        if (!cur || typeof cur !== 'object') {
            recordGap('chrome.' + name, 'namespace-locked');
            return;
        }
        note('chrome.' + name + ': native binding kept — filling '
             + 'missing members');
        for (var k in obj) {
            if (cur[k] === undefined)
                safeSet(cur, k, obj[k],
                        'chrome.' + name + '.' + k);
        }
    }

    // --- post-bootstrap repair -----------------------------------------
    // Re-assert every shim surface the engine may have redefined.
    // Idempotent by construction — a no-op when nothing was swapped.
    var tabsProxyRef = null, tabsNsRef = null;
    var bridgeLocalRef = null, bridgeSessionRef = null;
    var wrappedOnMessageRef = null, shimSendMessageRef = null;
    var shimConnectRef = null;
    var rtImpls = {}, rtEventNames = null;
    // Connection events are re-asserted by IDENTITY, not presence —
    // a late native binding swap would otherwise strand the
    // extension's listeners on an event the relay can never fire.
    var rtReassert = {};

    // Assert chrome.tabs is our proxy.  First try a getter+no-op-setter
    // descriptor — Chromium's bootstrap rewrites 'tabs' by assignment
    // on this page (measured mid-boot) and a setter property shrugs
    // that off where a plain data property is overwritten.  If the
    // descriptor cannot be installed, fall back to assignment, then
    // to member-fill as before.
    function assertTabs() {
        if (!tabsProxyRef)
            return;
        try {
            if (chrome.tabs === tabsProxyRef)
                return;
        } catch (e) {
            return;
        }
        var ok = false;
        try {
            Object.defineProperty(chrome, 'tabs', {
                configurable: true,
                enumerable: true,
                get: function () { return tabsProxyRef; },
                set: function () {}
            });
            ok = (chrome.tabs === tabsProxyRef);
        } catch (e) {}
        if (!ok) {
            try {
                chrome.tabs = tabsProxyRef;
                ok = (chrome.tabs === tabsProxyRef);
            } catch (e2) {}
        }
        if (!ok && chrome.tabs && typeof chrome.tabs === 'object') {
            // The native 'tabs' property resisted replacement — fill
            // its missing members individually.
            note('chrome.tabs: native binding kept — filling missing '
                 + 'members');
            for (var tk in tabsNsRef) {
                if (tk === '__aroraShim')
                    continue;
                if (chrome.tabs[tk] === undefined)
                    safeSet(chrome.tabs, tk, tabsNsRef[tk],
                            'chrome.tabs.' + tk);
            }
        }
        if (!ok && !chrome.tabs)
            recordGap('chrome.tabs', 'namespace-locked');
    }

    function repairInstalls() {
        if (typeof chrome === 'undefined' || !chrome)
            return;
        for (var nm in installedNs) {
            try {
                if (chrome[nm] !== installedNs[nm])
                    installNs(nm, installedNs[nm]);
            } catch (e) {}
        }
        try {
            assertTabs();
        } catch (e) {}
        var st = chrome.storage;
        if (st && typeof st === 'object') {
            if (bridgeLocalRef && st.local !== bridgeLocalRef)
                safeSet(st, 'local', bridgeLocalRef,
                        'chrome.storage.local');
            if (bridgeSessionRef && st.session !== bridgeSessionRef)
                safeSet(st, 'session', bridgeSessionRef,
                        'chrome.storage.session');
        }
        var rt2 = chrome.runtime;
        if (rt2 && typeof rt2 === 'object') {
            if (rtEventNames) {
                for (var e = 0; e < rtEventNames.length; ++e) {
                    var en = rtEventNames[e];
                    if (!rt2[en])
                        safeSet(rt2, en, makeEvent('runtime.' + en),
                                'chrome.runtime.' + en);
                }
            }
            if (wrappedOnMessageRef
                    && (!rt2.onMessage || !rt2.onMessage.addListener))
                safeSet(rt2, 'onMessage', wrappedOnMessageRef,
                        'chrome.runtime.onMessage');
            if (shimSendMessageRef
                    && typeof rt2.sendMessage !== 'function')
                safeSet(rt2, 'sendMessage', shimSendMessageRef,
                        'chrome.runtime.sendMessage');
            // connect is overridden even when a native binding
            // exists — identity check, not presence check (the
            // native connect reaches the dead service worker, never
            // the hosted background).
            if (shimConnectRef && rt2.connect !== shimConnectRef)
                safeSet(rt2, 'connect', shimConnectRef,
                        'chrome.runtime.connect');
            for (var mn in rtImpls) {
                if (rt2[mn] === undefined)
                    safeSet(rt2, mn, rtImpls[mn],
                            'chrome.runtime.' + mn);
            }
            // Identity re-assert for the connection events — a late
            // native swap would strand listeners where the relay can
            // never reach them (see the install block).
            for (var ra in rtReassert) {
                if (rt2[ra] !== rtReassert[ra])
                    safeSet(rt2, ra, rtReassert[ra],
                            'chrome.runtime.' + ra);
            }
        }
    }

    // --- messaging -----------------------------------------------------
    // Envelope: messages the shim sends itself are wrapped so a
    // shimmed peer can correlate a native delivery with the relay
    // copy (same mid) — whichever arrives second is dropped.  The
    // ORIGINAL payload goes to native sendMessage too so the real
    // service worker (which is not shimmed and wouldn't unwrap) still
    // sees the plain message — duplication between SW and shimmed
    // peers is inherent to the dual-context design and matches the
    // extension's own broadcast expectations.
    var msgPending = {};   // mid -> {finish, peers, replies}
    var relayPeers = 0;

    function contentKey(msg) {
        try { return String(JSON.stringify(msg)).slice(0, 512); }
        catch (e) { return String(msg).slice(0, 512); }
    }

    function markDelivery(key) {
        var now = Date.now();
        shim.recentDeliveries[key] = now;
        var keys = Object.keys(shim.recentDeliveries);
        if (keys.length > 200) {
            for (var i = 0; i < keys.length; ++i) {
                if (now - shim.recentDeliveries[keys[i]] > 3000)
                    delete shim.recentDeliveries[keys[i]];
            }
            keys = Object.keys(shim.recentDeliveries);
            while (keys.length > 200)
                delete shim.recentDeliveries[keys.shift()];
        }
    }

    function seenDelivery(key) {
        var t = shim.recentDeliveries[key];
        return t !== undefined && (Date.now() - t) < 3000;
    }

    function seenMid(mid) {
        if (shim.seenMids[mid])
            return true;
        shim.seenMids[mid] = 1;
        var keys = Object.keys(shim.seenMids);
        while (keys.length > 500)
            delete shim.seenMids[keys.shift()];
        return false;
    }

    function envelopeOf(msg) {
        if (msg && msg.__aroraMsg && msg.__aroraMsg.mid)
            return msg.__aroraMsg;
        return null;
    }

    function onRelayResponse(payload) {
        if (!payload || !payload.mid)
            return;
        var entry = msgPending[payload.mid];
        if (!entry)
            return;
        // The runtime forwards exactly one terminal answer per mid:
        // the first defined response, or null once every peer
        // answered empty.
        entry.finish((payload.response === null
                      || payload.response === undefined)
                     ? undefined : payload.response);
    }

    function onRelayMessage(payload) {
        if (!payload || !payload.mid)
            return;
        var env = envelopeOf(payload.msg);
        var mid = payload.mid;
        var msg = env ? env.msg : payload.msg;
        if (seenMid(mid))
            return;   // already delivered (native won the race)
        markDelivery(contentKey(msg));
        var responded = false;
        var listeners = shim.messageListeners.slice();
        function respond(value) {
            if (responded)
                return;
            responded = true;
            call('relay', 'respond', { mid: mid, response: value })
                .catch(function () {});
        }
        var wantAsync = false;
        for (var i = 0; i < listeners.length; ++i) {
            try {
                var r = listeners[i].call(
                    null, msg,
                    // A real MessageSender — sender.origin must equal
                    // the extension origin or senderIsInternal-style
                    // gates (Bitwarden's session/message handlers)
                    // drop the delivery.
                    { id: env && env.extId || cfg.id,
                      url: env && env.senderUrl || undefined,
                      origin: 'chrome-extension://'
                              + (env && env.extId || cfg.id) },
                    respond);
                if (r === true)
                    wantAsync = true;
                else if (r !== undefined && !responded)
                    respond(r);
            } catch (e) {
                record('relay onMessage listener: ' + e);
            }
        }
        if (!responded && !wantAsync)
            call('relay', 'respond',
                 { mid: mid, response: null, hadListeners:
                       listeners.length > 0 }).catch(function () {});
    }

    shim.messageListeners = [];

    // --- ports ----------------------------------------------------------
    // runtime.connect() returns a Port synchronously — it cannot
    // answer a typed error without breaking the contract, so an
    // unserviceable connect yields a real Port whose postMessage is
    // a typed-error no-op and whose onDisconnect fires on the next
    // tick.  Same-extension connects route through the runtime
    // relay: 'portConnect' fans the offer out to registered sibling
    // contexts, an accepter answers 'portAccept', and messages ride
    // 'portMessage'/'portDisconnect' addressed by each end's local
    // port id.
    function makeLocalEvent() {
        var listeners = [];
        return {
            addListener: function (fn) {
                if (typeof fn === 'function'
                        && listeners.indexOf(fn) < 0)
                    listeners.push(fn);
            },
            removeListener: function (fn) {
                var i = listeners.indexOf(fn);
                if (i >= 0)
                    listeners.splice(i, 1);
            },
            hasListener: function (fn) {
                return listeners.indexOf(fn) >= 0;
            },
            hasListeners: function () { return listeners.length > 0; },
            __fire: function (args) {
                for (var i = 0; i < listeners.length; ++i) {
                    try { listeners[i].apply(null, args); }
                    catch (e) { record('port listener: ' + e); }
                }
            }
        };
    }

    shim.ports = {};   // local port id -> Port

    function newPort(name, chan, localId, sender) {
        // port.sender is the REMOTE end's MessageSender — Bitwarden's
        // session handler drops any connect whose sender.origin does
        // not equal chrome.runtime.getURL('') (senderIsInternal), so
        // the field must be complete, not just {id}.
        if (!sender || typeof sender !== 'object') {
            sender = { id: cfg.id,
                       url: String(location.href),
                       origin: 'chrome-extension://' + cfg.id };
        }
        var port = {
            name: name,
            sender: sender,
            onMessage: makeLocalEvent(),
            onDisconnect: makeLocalEvent(),
            disconnected: false
        };
        port.postMessage = function (msg) {
            if (port.disconnected)
                return;
            call('relay', 'portMessage',
                 { chan: chan, portId: localId, msg: msg })
                .catch(function () {});
        };
        port.disconnect = function () {
            if (port.disconnected)
                return;
            port.disconnected = true;
            call('relay', 'portDisconnect',
                 { chan: chan, portId: localId })
                .catch(function () {});
            delete shim.ports[localId];
        };
        shim.ports[localId] = port;
        return port;
    }

    function deadPort(name, why) {
        // Honest non-port: exists, immediately disconnects, posts
        // record the gap — never a missing function.
        var port = {
            name: name,
            sender: { id: cfg.id },
            onMessage: makeLocalEvent(),
            onDisconnect: makeLocalEvent(),
            disconnected: true
        };
        port.postMessage = function () {
            recordGap('chrome.runtime.Port.postMessage', why);
        };
        port.disconnect = function () {};
        setTimeout(function () {
            port.onDisconnect.__fire([port]);
        }, 0);
        return port;
    }

    // --- chrome.runtime ------------------------------------------------
    var rt = (typeof chrome !== 'undefined') ? chrome.runtime : null;
    if (rt) {
        // Installs go through rtSet so rtImpls remembers them — the
        // post-bootstrap repair re-fills any member the engine's own
        // binding swap wiped out.
        var rtSet = function (name, fn, api) {
            rtImpls[name] = fn;
            safeSet(rt, name, fn, api);
        };
        // onMessage listeners register BOTH on the real event (native
        // deliveries keep working) and in shim.messageListeners (the
        // relay path).  The real-event wrapper unwraps envelopes and
        // dedupes mid/content so a message arriving over both paths
        // fires once.
        if (rt.onMessage && rt.onMessage.addListener) {
            var realOnMessage = rt.onMessage;
            var bound = [];  // {user, wrapped}
            var wrapped = {
                addListener: function (fn) {
                    if (typeof fn !== 'function')
                        return;
                    var w = function (msg, sender, respond) {
                        var env = envelopeOf(msg);
                        if (env) {
                            if (seenMid(env.mid))
                                return false;
                            markDelivery(contentKey(env.msg));
                            sender = { id: env.extId || cfg.id };
                            msg = env.msg;
                        } else {
                            markDelivery(contentKey(msg));
                        }
                        return fn.call(this, msg, sender, respond);
                    };
                    bound.push({ user: fn, wrapped: w });
                    shim.messageListeners.push(fn);
                    realOnMessage.addListener(w);
                },
                removeListener: function (fn) {
                    for (var i = 0; i < bound.length; ++i) {
                        if (bound[i].user === fn) {
                            realOnMessage.removeListener(bound[i].wrapped);
                            bound.splice(i, 1);
                            break;
                        }
                    }
                    var j = shim.messageListeners.indexOf(fn);
                    if (j >= 0)
                        shim.messageListeners.splice(j, 1);
                },
                hasListener: function (fn) {
                    for (var i = 0; i < bound.length; ++i)
                        if (bound[i].user === fn)
                            return true;
                    return false;
                },
                hasListeners: function () { return bound.length > 0; },
                addRules: realOnMessage.addRules
                            ? function () {
                                  return realOnMessage.addRules
                                      .apply(realOnMessage, arguments);
                              }
                            : undefined,
                getRules: realOnMessage.getRules,
                removeRules: realOnMessage.removeRules
            };
            wrappedOnMessageRef = wrapped;
            try { rt.onMessage = wrapped; }
            catch (e) { recordGap('chrome.runtime.onMessage', String(e)); }
        }

        if (rt.sendMessage) {
            var realSend = rt.sendMessage.bind(rt);
            shimSendMessageRef = function () {
                var a = Array.prototype.slice.call(arguments);
                var extId = cfg.id;
                if (typeof a[0] === 'string')
                    extId = a.shift();
                var msg = a.shift();
                var cb = null;
                for (var i = a.length - 1; i >= 0; --i) {
                    if (typeof a[i] === 'function') {
                        cb = a.splice(i, 1)[0];
                        break;
                    }
                }
                var options = a.length ? a[0] : undefined;

                if (extId !== cfg.id) {
                    // cross-extension: Chromium owns delivery
                    touch('chrome.runtime.sendMessage');
                    try {
                        var p = realSend(extId, msg, options, cb);
                        return p && p.then ? p
                            : Promise.resolve(undefined);
                    } catch (e) {
                        return failCall('runtime.sendMessage',
                                        'native-failed', cb);
                    }
                }

                touch('chrome.runtime.sendMessage');
                var mid = cfg.id + '#' + (++shim.seq) + '#'
                          + Date.now();
                var envelope = { __aroraMsg: { mid: mid, msg: msg,
                                               extId: cfg.id,
                                               senderUrl:
                                                   String(location
                                                          .href) } };
                var promise = new Promise(function (resolve) {
                    var entry = { peers: -1, replies: 0, done: false,
                                  nativeDone: false };
                    entry.maybeFinish = function () {
                        if (entry.done)
                            return;
                        if (entry.peers >= 0 && entry.nativeDone
                                && entry.replies >= entry.peers)
                            entry.finish(undefined);
                    };
                    entry.finish = function (v) {
                        if (entry.done)
                            return;
                        entry.done = true;
                        delete msgPending[mid];
                        answerCb(cb, v,
                                 v === undefined
                                     ? 'The message port closed '
                                       + 'before a response was '
                                       + 'received.'
                                     : null);
                        resolve(v);
                    };
                    msgPending[mid] = entry;
                    // Chrome parity: nobody answered -> callback with
                    // undefined + lastError, no 30s hang.
                    setTimeout(function () { entry.finish(undefined); },
                               15000);
                    // Native path — real SW and any extension context
                    // Chromium reaches get the ORIGINAL payload.
                    try {
                        realSend(msg, function (reply) {
                            entry.nativeDone = true;
                            if (!rt.lastError && reply !== undefined)
                                entry.finish(reply);
                            else
                                entry.maybeFinish();
                        });
                    } catch (e) {
                        entry.nativeDone = true;
                        entry.maybeFinish();
                    }
                    // Relay path — shimmed contexts (hosted<->hosted
                    // now, popup once EXT17 registers them).
                    call('relay', 'send',
                         { mid: mid, msg: envelope })
                        .then(function (info) {
                            entry.peers = (info
                                && typeof info.peers === 'number')
                                ? info.peers : 0;
                            entry.maybeFinish();
                        })
                        .catch(function () {
                            entry.peers = 0;
                            entry.maybeFinish();
                        });
                });
                return promise;
            };
            rtSet('sendMessage', shimSendMessageRef,
                    'chrome.runtime.sendMessage');
        }

        if (!rt.id) {
            rtSet('id', cfg.id, 'chrome.runtime.id');
        }
        if (!rt.getURL) {
            rtSet('getURL', function (path) {
                return 'chrome-extension://' + cfg.id + '/'
                       + String(path || '');
            }, 'chrome.runtime.getURL');
        }
        if (!rt.getManifest) {
            rtSet('getManifest',
                    function () { return manifest(); },
                    'chrome.runtime.getManifest');
        }
        if (!rt.onInstalled)
            rtSet('onInstalled',
                    makeEvent('runtime.onInstalled'),
                    'chrome.runtime.onInstalled');
        if (!rt.onStartup)
            rtSet('onStartup',
                    makeEvent('runtime.onStartup'),
                    'chrome.runtime.onStartup');
        {
            // EXT17: the runtime's relay registry is the authoritative
            // context list — the hosted background plus every injected
            // sibling page that completed its handshake.  A native
            // getContexts may exist (popup pages are real extension
            // contexts to Chromium) but cannot see the hosted
            // background — merge, don't defer to it.
            var nativeGetContexts = rt.getContexts;
            rtSet('getContexts', function (filter, cb) {
                var bridgeP = call('runtime', 'getContexts', {})
                    .then(function (v) {
                        return (v && v.length) ? v : [];
                    }, function () {
                        return [];
                    });
                var nativeP = nativeGetContexts
                    ? new Promise(function (res) {
                        try {
                            var r = nativeGetContexts.call(
                                rt, filter || {},
                                function (v) { res(v || []); });
                            if (r && r.then)
                                r.then(function (v) {
                                    res(v || []);
                                }, function () { res([]); });
                        } catch (e) { res([]); }
                    })
                    : Promise.resolve([]);
                var p = Promise.all([bridgeP, nativeP])
                    .then(function (both) {
                        var list = both[0].slice();
                        var seen = {};
                        list.forEach(function (c) {
                            seen[String(c.contextType) + '|'
                                 + String(c.documentUrl)] = true;
                        });
                        both[1].forEach(function (c) {
                            var k = String(c.contextType) + '|'
                                    + String(c.documentUrl);
                            if (!seen[k]) {
                                seen[k] = true;
                                list.push(c);
                            }
                        });
                        var types = filter && filter.contextTypes;
                        if (types && types.length) {
                            list = list.filter(function (c) {
                                return types.indexOf(c.contextType)
                                    >= 0;
                            });
                        }
                        return list;
                    });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); });
                return p;
            }, 'chrome.runtime.getContexts');
        }

        // The native runtime exists but is a SUBSET — Bitwarden's
        // boot crashed on runtime.onConnect.addListener.  Fill every
        // lifecycle/connection event the binding lacks so event
        // registration can never be a TypeError.
        var rtEvents = ['onConnect', 'onConnectExternal',
            'onMessageExternal', 'onInstalled', 'onStartup',
            'onSuspend', 'onSuspendCanceled', 'onUpdateAvailable',
            'onBrowserUpdateAvailable', 'onRestartRequired'];
        rtEventNames = rtEvents;
        for (var re = 0; re < rtEvents.length; ++re) {
            if (!rt[rtEvents[re]])
                safeSet(rt, rtEvents[re],
                        makeEvent('runtime.' + rtEvents[re]),
                        'chrome.runtime.' + rtEvents[re]);
        }

        // EXT17: connection events get the onMessage treatment.  A
        // native onConnect may exist — but a native connect can only
        // target the dead service worker, so the extension's listener
        // must ALSO land on the shim event the __arora.portConnect
        // delivery fires.  The real Bitwarden popup opened its
        // 'session' port into exactly this dead end (the bg-side port
        // registered but onConnect never fired — Angular's bootstrap
        // awaited the session IPC forever).  Dual-register so a real
        // external connect still reaches the listener.
        var connEvents = ['onConnect', 'onConnectExternal',
                          'onMessageExternal'];
        for (var ce = 0; ce < connEvents.length; ++ce) {
            (function (cname) {
                var evName = 'runtime.' + cname;
                var shimEv = shim.events[evName]
                             || makeEvent(evName);
                var realEv;
                try { realEv = rt[cname]; } catch (e) {
                    realEv = null;
                }
                var installed = shimEv;
                if (realEv && realEv !== shimEv
                        && typeof realEv.addListener === 'function') {
                    installed = {
                        addListener: function (fn) {
                            shimEv.addListener(fn);
                            try {
                                realEv.addListener(fn);
                            } catch (e) {}
                        },
                        removeListener: function (fn) {
                            shimEv.removeListener(fn);
                            try {
                                realEv.removeListener(fn);
                            } catch (e) {}
                        },
                        hasListener: function (fn) {
                            return shimEv.hasListener(fn)
                                || (typeof realEv.hasListener
                                        === 'function'
                                    && realEv.hasListener(fn));
                        },
                        hasListeners: function () {
                            return shimEv.hasListeners()
                                || (typeof realEv.hasListeners
                                        === 'function'
                                    && realEv.hasListeners());
                        }
                    };
                }
                rtImpls[cname] = installed;
                rtReassert[cname] = installed;
                safeSet(rt, cname, installed,
                        'chrome.runtime.' + cname);
            })(connEvents[ce]);
        }

        // runtime.connect — a same-extension connect ALWAYS rides
        // the relay: the native binding targets the extension's own
        // service worker, which the hosted runtime replaced, so a
        // native connect can only reach dead air (the real Bitwarden
        // popup awaited its background-port handshake forever on
        // exactly this — bootstrap never reached the unlock gate).
        // A cross-extension connect still delegates to the native
        // binding when it has one, else answers a deadPort (a typed
        // gap, never a missing function).
        var realConnect = (typeof rt.connect === 'function')
            ? rt.connect.bind(rt) : null;
        shimConnectRef = function (a1, a2) {
            var extId = cfg.id;
            var info = {};
            if (typeof a1 === 'string') {
                extId = a1;
                info = a2 || {};
            } else if (a1) {
                info = a1;
            }
            var name = info.name || '';
            if (extId !== cfg.id) {
                if (realConnect) {
                    touch('chrome.runtime.connect');
                    try { return realConnect(a1, a2); } catch (e) {}
                }
                note('chrome.runtime.connect(' + extId
                     + '): cross-extension connect unsupported');
                return deadPort(name, 'cross-extension');
            }
            touch('chrome.runtime.connect');
            var localId = 'p' + (++shim.seq) + '#' + Date.now();
            var chan = cfg.id + '#' + localId;
            var port = newPort(name, chan, localId);
            call('relay', 'portConnect',
                 { chan: chan, portId: localId, name: name,
                   senderUrl: String(location.href) })
                .catch(function () {});
            return port;
        };
        rtSet('connect', shimConnectRef, 'chrome.runtime.connect');
        if (typeof rt.getPlatformInfo !== 'function') {
            rtSet('getPlatformInfo', function (cb) {
                var p = call('runtime', 'platformInfo', {})
                    .then(function (v) {
                        return v || { os: 'linux', arch: 'x86-64',
                                      nacl_arch: 'x86-64' };
                    }, function () {
                        return { os: 'linux', arch: 'x86-64',
                                 nacl_arch: 'x86-64' };
                    });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); });
                return p;
            }, 'chrome.runtime.getPlatformInfo');
        }
        if (typeof rt.setUninstallURL !== 'function') {
            rtSet('setUninstallURL', function (url, cb) {
                note('chrome.runtime.setUninstallURL recorded '
                     + '(no uninstall flow)');
                return okCall(undefined, cb);
            }, 'chrome.runtime.setUninstallURL');
        }
        if (typeof rt.openOptionsPage !== 'function') {
            rtSet('openOptionsPage', function (cb) {
                // EXT17: opens options_ui.page in a normal tab via
                // the backend — the sibling-page injection gives it
                // the shim there.
                var p = call('runtime', 'openOptionsPage', {});
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            }, 'chrome.runtime.openOptionsPage');
        }
        if (typeof rt.requestUpdateCheck !== 'function') {
            rtSet('requestUpdateCheck', function (cb) {
                return failCall('runtime.requestUpdateCheck',
                                'unsupported', cb);
            }, 'chrome.runtime.requestUpdateCheck');
        }
        if (typeof rt.restartAfterDelay !== 'function') {
            rtSet('restartAfterDelay', function (delay, cb) {
                return failCall('runtime.restartAfterDelay',
                                'unsupported', cb);
            }, 'chrome.runtime.restartAfterDelay');
        }
        if (typeof rt.getPackageDirectoryEntry !== 'function') {
            rtSet('getPackageDirectoryEntry', function (cb) {
                return failCall('runtime.getPackageDirectoryEntry',
                                'unsupported', cb);
            }, 'chrome.runtime.getPackageDirectoryEntry');
        }
        if (typeof rt.sendNativeMessage !== 'function') {
            rtSet('sendNativeMessage', function (app, msg, cb) {
                return failCall('runtime.sendNativeMessage',
                                'unsupported', cb);
            }, 'chrome.runtime.sendNativeMessage');
        }
        if (typeof rt.connectNative !== 'function') {
            rtSet('connectNative', function (app) {
                note('chrome.runtime.connectNative(' + app
                     + '): no native messaging host');
                return deadPort(app, 'no-native-messaging');
            }, 'chrome.runtime.connectNative');
        }
        if (typeof rt.reload !== 'function') {
            rtSet('reload', function () {
                recordGap('chrome.runtime.reload', 'unsupported');
            }, 'chrome.runtime.reload');
        }
    }

    // --- chrome.extension (legacy alias surface) ----------------------
    if (typeof chrome !== 'undefined') {
        installNs('extension', {
            getURL: function (p) {
                return 'chrome-extension://' + cfg.id + '/'
                       + String(p || '');
            },
            // Window objects cannot cross the bridge — a context
            // answers its OWN window for its own type; EXT17 page
            // contexts answer null for the background page rather
            // than the wrong object.
            getViews: extensionGetViews,
            getBackgroundPage: extensionGetBackgroundPage,
            inIncognitoContext: false,
            isAllowedIncognitoAccess: function (cb) {
                return okCall(false, cb);
            },
            isAllowedFileSchemeAccess: function (cb) {
                return okCall(false, cb);
            },
            onRequest: makeEvent('extension.onRequest'),
            lastError: null
        });
    }

    // --- chrome.storage -------------------------------------------------
    // The hosted page's real storage.local IS the store the still-
    // running service worker and real extension pages share — keep it
    // (the EXT13 spike measured it working, and replacing it would
    // fork the extension's state).  The bridge store backs contexts
    // where the bindings are absent and is exercised directly via
    // __aroraShim.storageArea().
    function bridgeStorageArea(areaName) {
        var areaEvent = makeEvent('storage.' + areaName + '.onChanged');
        function afterMutate(result) {
            var changes = result && result.changes || {};
            var k;
            var any = false;
            for (k in changes)
                any = true;
            if (!any)
                return;
            areaEvent.__fire([changes, areaName]);
            if (shim.events['storage.onChanged'])
                shim.events['storage.onChanged']
                    .__fire([changes, areaName]);
            if (chrome.storage && chrome.storage.onChanged
                    && chrome.storage.onChanged.__fire
                    && chrome.storage.onChanged
                           !== shim.events['storage.onChanged'])
                chrome.storage.onChanged.__fire([changes, areaName]);
        }
        return {
            get: function (keys, cb) {
                var p = call('storage', areaName + '.get',
                             { keys: keys === undefined ? null : keys });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, {}, e.message);
                           });
                return p;
            },
            set: function (items, cb) {
                var p = call('storage', areaName + '.set',
                             { items: items || {} });
                p.then(afterMutate, function () {});
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p.then(function () {});
            },
            remove: function (keys, cb) {
                var p = call('storage', areaName + '.remove',
                             { keys: keys });
                p.then(afterMutate, function () {});
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p.then(function () {});
            },
            clear: function (cb) {
                var p = call('storage', areaName + '.clear', {});
                p.then(afterMutate, function () {});
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p.then(function () {});
            },
            getBytesInUse: function (keys, cb) {
                var p = call('storage', areaName + '.getBytesInUse',
                             { keys: keys === undefined ? null : keys });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(0); });
                return p;
            },
            setAccessLevel: function (level, cb) {
                return okCall(undefined, cb);
            },
            onChanged: areaEvent
        };
    }
    shim.storageArea = bridgeStorageArea;   // test seam

    if (typeof chrome !== 'undefined') {
        if (!chrome.storage)
            safeSet(chrome, 'storage', {}, 'chrome.storage');
        if (chrome.storage) {
        if (!chrome.storage.local
                || typeof chrome.storage.local.get !== 'function') {
            bridgeLocalRef = bridgeStorageArea('local');
            if (safeSet(chrome.storage, 'local', bridgeLocalRef,
                        'chrome.storage.local'))
                note('chrome.storage.local: bridge-backed store '
                     + '(per-extension namespaced, persisted '
                     + 'host-side)');
        }
        if (!chrome.storage.session
                || typeof chrome.storage.session.get !== 'function') {
            bridgeSessionRef = bridgeStorageArea('session');
            safeSet(chrome.storage, 'session', bridgeSessionRef,
                    'chrome.storage.session');
        }
        if (!chrome.storage.managed) {
            safeSet(chrome.storage, 'managed', {
                get: function (keys, cb) {
                    touch('chrome.storage.managed.get');
                    return okCall({}, cb);
                },
                onChanged: makeEvent('storage.managed.onChanged')
            }, 'chrome.storage.managed');
        }
        if (!chrome.storage.onChanged)
            safeSet(chrome.storage, 'onChanged',
                    makeEvent('storage.onChanged'),
                    'chrome.storage.onChanged');
        }
    }

    // --- chrome.i18n -----------------------------------------------------
    // EXT17: the native getMessage EXISTS on this engine's extension
    // pages but resolves no message catalog — every key answers ''
    // and the real Bitwarden popup rendered fully blank chrome (its
    // first-run carousel's CTAs carried no text to match).  Wrap
    // getMessage unconditionally: a working native answer wins, every
    // empty/native miss falls through to the _locales map embedded
    // in the injected config (synchronous — page scripts calling
    // getMessage at import time cannot wait for a bridge round-trip),
    // with the bridge fetch behind it for hosts built without one.
    if (typeof chrome !== 'undefined') {
        if (!chrome.i18n)
            safeSet(chrome, 'i18n', {}, 'chrome.i18n');
        if (cfg.i18n && typeof cfg.i18n === 'object'
                && !shim.i18nCache)
            shim.i18nCache = cfg.i18n;
        var i18nLookup = function (name, substitutions) {
            var cached = shim.i18nCache && shim.i18nCache[name];
            if (cached === undefined)
                return String(name);
            var out = String(cached);
            var subs = substitutions === undefined ? []
                : (Array.isArray(substitutions)
                       ? substitutions : [substitutions]);
            // Chrome substitutes $1..$9 positionally; $$ escapes a
            // literal dollar so it is shielded during the $n pass.
            var ESC = '\u0001';
            out = out.split('$$').join(ESC);
            for (var i = 0; i < subs.length && i < 9; ++i)
                out = out.split('$' + (i + 1)).join(String(subs[i]));
            return out.split(ESC).join('$');
        };
        if (chrome.i18n) {
            var nativeGetMessage = null;
            try {
                if (typeof chrome.i18n.getMessage === 'function')
                    nativeGetMessage =
                        chrome.i18n.getMessage.bind(chrome.i18n);
            } catch (e) {
                nativeGetMessage = null;
            }
            safeSet(chrome.i18n, 'getMessage',
                    function (name, substitutions) {
                touch('chrome.i18n.getMessage');
                var out = '';
                if (nativeGetMessage) {
                    try {
                        out = nativeGetMessage(name, substitutions)
                                  || '';
                    } catch (e) {
                        out = '';
                    }
                }
                if (!out)
                    out = i18nLookup(name, substitutions);
                return out;
            }, 'chrome.i18n.getMessage');
            if (!shim.i18nCache) {
                // sync API: ask the host for the whole locale map
                // once and consult it thereafter.
                call('i18n', 'messages', {}).then(function (map) {
                    shim.i18nCache = map || {};
                }).catch(function () { shim.i18nCache = {}; });
            }
            if (!chrome.i18n.getUILanguage) {
                safeSet(chrome.i18n, 'getUILanguage', function () {
                    return manifest().default_locale || 'en';
                }, 'chrome.i18n.getUILanguage');
            }
            if (!chrome.i18n.getAcceptLanguages) {
                safeSet(chrome.i18n, 'getAcceptLanguages',
                        function (cb) {
                    var langs = [];
                    try {
                        langs = [(chrome.i18n.getUILanguage
                                      ? chrome.i18n.getUILanguage()
                                      : 'en')];
                    } catch (e) {
                        langs = ['en'];
                    }
                    answerCb(cb, langs);
                    return Promise.resolve(langs);
                }, 'chrome.i18n.getAcceptLanguages');
            }
        }
    }

    // --- chrome.alarms — host timers -------------------------------------
    // A real alarms binding can be present-but-inert on the hosted
    // page (no embedder delegate): replace it so onAlarm genuinely
    // fires.
    if (typeof chrome !== 'undefined') {
        var alarmsOnAlarm = makeEvent('alarms.onAlarm');
        installNs('alarms', {
            create: function (name, info) {
                if (typeof name === 'object') {
                    info = name;
                    name = '';
                }
                return call('alarms', 'create',
                            { name: name || '', info: info || {} })
                    .then(function () {});
            },
            get: function (name, cb) {
                var p = call('alarms', 'get', { name: name || '' });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(undefined); });
                return p;
            },
            getAll: function (cb) {
                var p = call('alarms', 'getAll', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb([]); });
                return p;
            },
            clear: function (name, cb) {
                var p = call('alarms', 'clear', { name: name || '' });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(false); });
                return p;
            },
            clearAll: function (cb) {
                var p = call('alarms', 'clearAll', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(false); });
                return p;
            },
            onAlarm: alarmsOnAlarm
        });
    }

    // --- chrome.action / chrome.browserAction ----------------------------
    // No native toolbar delegate exists — setters feed the per-
    // extension action-state registry EXT08's button strip reads.
    if (typeof chrome !== 'undefined') {
        var actionOnClicked = makeEvent('action.onClicked');
        var actionApi = {
            setTitle: function (details, cb) {
                var p = call('action', 'set',
                    { key: 'title', value: details && details.title
                        || '', tabId: details && details.tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getTitle: function (details, cb) {
                var p = call('action', 'get',
                             { key: 'title',
                               tabId: details && details.tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(''); });
                return p;
            },
            setBadgeText: function (details, cb) {
                var p = call('action', 'set',
                    { key: 'badgeText', value: details && details.text
                        || '', tabId: details && details.tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getBadgeText: function (details, cb) {
                var p = call('action', 'get',
                             { key: 'badgeText',
                               tabId: details && details.tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(''); });
                return p;
            },
            setBadgeBackgroundColor: function (details, cb) {
                var p = call('action', 'set',
                    { key: 'badgeColor', value: details && details.color,
                      tabId: details && details.tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getBadgeBackgroundColor: function (details, cb) {
                var p = call('action', 'get',
                             { key: 'badgeColor',
                               tabId: details && details.tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb([0, 0, 0, 0]); });
                return p;
            },
            setBadgeTextColor: function (details, cb) {
                var p = call('action', 'set',
                    { key: 'badgeTextColor',
                      value: details && details.color,
                      tabId: details && details.tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getBadgeTextColor: function (details, cb) {
                var p = call('action', 'get',
                             { key: 'badgeTextColor',
                               tabId: details && details.tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(null); });
                return p;
            },
            setIcon: function (details, cb) {
                return failCall('action.setIcon',
                                'unsupported', cb);
            },
            setPopup: function (details, cb) {
                var p = call('action', 'set',
                    { key: 'popup', value: details && details.popup
                        || '', tabId: details && details.tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getPopup: function (details, cb) {
                var p = call('action', 'get',
                             { key: 'popup',
                               tabId: details && details.tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(''); });
                return p;
            },
            enable: function (tabId, cb) {
                var p = call('action', 'set',
                    { key: 'enabled', value: true, tabId: tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            disable: function (tabId, cb) {
                var p = call('action', 'set',
                    { key: 'enabled', value: false, tabId: tabId });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            isEnabled: function (tabId, cb) {
                var p = call('action', 'get',
                             { key: 'enabled', tabId: tabId });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(true); });
                return p;
            },
            onClicked: actionOnClicked
        };
        installNs('action', actionApi);
        var b = {};
        for (var k in actionApi)
            b[k] = actionApi[k];
        b.onClicked = actionOnClicked;
        installNs('browserAction', b);
    }

    // --- chrome.tabs ------------------------------------------------------
    var tabsEvents = ['onCreated', 'onUpdated', 'onMoved', 'onActivated',
        'onHighlighted', 'onDetached', 'onAttached', 'onRemoved',
        'onReplaced', 'onZoomChange'];
    if (typeof chrome !== 'undefined') {
        var realTabs = chrome.tabs || {};
        var tabsNs = { __aroraShim: true };
        for (var te = 0; te < tabsEvents.length; ++te) {
            // Shim events replace inert/absent event objects — EXT16
            // pushes real browser signals into them via hostEvent.
            tabsNs[tabsEvents[te]] =
                makeEvent('tabs.' + tabsEvents[te]);
        }
        tabsNs.getCurrent = function (cb) {
            // EXT17: context-local — the bridge resolves the tab
            // hosting THIS page (an extension page in a real tab
            // answers itself; the background page and detached
            // popup answer undefined).
            touch('chrome.tabs.getCurrent');
            var err = null;
            var p = call('tabs', 'getCurrent', {})
                .then(function (v) {
                    return (v === null) ? undefined : v;
                }, function (e) {
                    err = e.message;
                    return undefined;
                });
            if (cb)
                p.then(function (v) { answerCb(cb, v, err); });
            return p;
        };
        tabsNs.query = function (queryInfo, cb) {
            var p = call('tabs', 'query', { query: queryInfo || {} });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, [], e.message); });
            return p;
        };
        tabsNs.get = function (tabId, cb) {
            var p = call('tabs', 'get', { id: tabId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.create = function (props, cb) {
            var p = call('tabs', 'create', { props: props || {} });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.update = function (tabId, props, cb) {
            if (typeof tabId === 'object') {
                cb = props;
                props = tabId;
                tabId = null;
            }
            var p = call('tabs', 'update',
                         { id: tabId, props: props || {} });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.sendMessage = function (tabId, msg, opts, cb) {
            if (typeof opts === 'function') {
                cb = opts;
                opts = undefined;
            }
            if (realTabs.sendMessage) {
                touch('chrome.tabs.sendMessage');
                try {
                    var r = realTabs.sendMessage.call(realTabs, tabId,
                                                      msg, opts, cb);
                    return r && r.then
                        ? r : Promise.resolve(undefined);
                } catch (e) {}
            }
            return failCall('tabs.sendMessage', 'unsupported', cb);
        };
        var tabsProxy = new Proxy(tabsNs, {
            get: function (t, prop) {
                if (prop === 'then')
                    return undefined;
                if (prop in t)
                    return t[prop];
                var rv;
                try { rv = realTabs[prop]; } catch (e) {}
                if (rv !== undefined)
                    return rv;
                var p = 'chrome.tabs.' + String(prop);
                if (typeof prop === 'string'
                        && prop.indexOf('on') === 0
                        && prop.length > 2
                        && prop.charAt(2)
                               === prop.charAt(2).toUpperCase())
                    return shim.events[p] || makeEvent(p);
                touch(p);
                return function () {
                    var a = arguments;
                    var cb = (a.length
                              && typeof a[a.length - 1] === 'function')
                        ? a[a.length - 1] : null;
                    return failCall(p, 'unsupported', cb);
                };
            },
            has: function () { return true; }
        });
        tabsProxyRef = tabsProxy;
        tabsNsRef = tabsNs;
        assertTabs();
    }

    // --- chrome.windows -----------------------------------------------------
    // EXT17: real window-level answers over the bridge (the EXT16
    // hub's window set — private/tor windows are invisible); events
    // are the shim events EXT16 pushes.  create/update/remove keep
    // their typed-error stubs via fillNs below.
    if (typeof chrome !== 'undefined') {
        var winNs = { __aroraShim: true };
        var winEventNames = ['onCreated', 'onRemoved',
            'onFocusChanged', 'onBoundsChanged'];
        for (var wne = 0; wne < winEventNames.length; ++wne)
            winNs[winEventNames[wne]] =
                makeEvent('windows.' + winEventNames[wne]);
        winNs.WINDOW_ID_NONE = -1;
        winNs.WINDOW_ID_CURRENT = -2;
        function winMethod(method) {
            return function () {
                var a = Array.prototype.slice.call(arguments);
                var cb = (a.length
                          && typeof a[a.length - 1] === 'function')
                    ? a.pop() : null;
                var args = {};
                if (a.length && typeof a[0] === 'number')
                    args.id = a[0];
                var p = call('windows', method, args);
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            };
        }
        winNs.get = winMethod('get');
        winNs.getCurrent = winMethod('getCurrent');
        winNs.getLastFocused = winMethod('getLastFocused');
        winNs.getAll = winMethod('getAll');
        // create/update/remove take an object first arg (or id then
        // object), not the id-only shape winMethod maps.
        winNs.create = function (createData, cb) {
            var p = call('windows', 'create',
                         { props: createData || {} });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) {
                           answerCb(cb, undefined, e.message);
                       });
            return p;
        };
        winNs.update = function (windowId, updateInfo, cb) {
            var p = call('windows', 'update',
                         { id: windowId, props: updateInfo || {} });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) {
                           answerCb(cb, undefined, e.message);
                       });
            return p;
        };
        winNs.remove = function (windowId, cb) {
            var p = call('windows', 'remove', { id: windowId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) {
                           answerCb(cb, undefined, e.message);
                       });
            return p;
        };
        installNs('windows', winNs);
    }

    // --- chrome.webNavigation ------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('webNavigation', {
            onBeforeNavigate: makeEvent('webNavigation.onBeforeNavigate'),
            onCommitted: makeEvent('webNavigation.onCommitted'),
            onCompleted: makeEvent('webNavigation.onCompleted'),
            onDOMContentLoaded:
                makeEvent('webNavigation.onDOMContentLoaded'),
            onErrorOccurred: makeEvent('webNavigation.onErrorOccurred'),
            onCreatedNavigationTarget:
                makeEvent('webNavigation.onCreatedNavigationTarget'),
            onHistoryStateUpdated:
                makeEvent('webNavigation.onHistoryStateUpdated'),
            onReferenceFragmentUpdated:
                makeEvent('webNavigation.onReferenceFragmentUpdated'),
            onTabReplaced: makeEvent('webNavigation.onTabReplaced'),
            getFrame: function (details, cb) {
                return failCall('webNavigation.getFrame',
                                'unsupported', cb);
            },
            getAllFrames: function (details, cb) {
                return failCall('webNavigation.getAllFrames',
                                'unsupported', cb);
            }
        });
    }

    // --- chrome.permissions ---------------------------------------------------
    if (typeof chrome !== 'undefined') {
        var permSet = function () {
            var m = manifest();
            var perms = (m.permissions || []).concat(
                m.optional_permissions || []);
            var origins = (m.host_permissions || []);
            return { permissions: perms, origins: origins };
        };
        installNs('permissions', {
            contains: function (query, cb) {
                touch('chrome.permissions.contains');
                var have = permSet();
                var ok = true;
                var asked = query && query.permissions || [];
                for (var i = 0; i < asked.length; ++i) {
                    if (have.permissions.indexOf(asked[i]) < 0) {
                        ok = false;
                        break;
                    }
                }
                var askedO = query && query.origins || [];
                for (var j = 0; ok && j < askedO.length; ++j) {
                    if (have.origins.indexOf(askedO[j]) < 0)
                        ok = false;
                }
                return okCall(ok, cb);
            },
            getAll: function (cb) {
                touch('chrome.permissions.getAll');
                return okCall(permSet(), cb);
            },
            request: function (permissions, cb) {
                return failCall('permissions.request',
                                'unsupported', cb);
            },
            remove: function (permissions, cb) {
                return failCall('permissions.remove',
                                'unsupported', cb);
            },
            onAdded: makeEvent('permissions.onAdded'),
            onRemoved: makeEvent('permissions.onRemoved')
        });
    }

    // --- chrome.notifications ---------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('notifications', {
            onClicked: makeEvent('notifications.onClicked'),
            onButtonClicked:
                makeEvent('notifications.onButtonClicked'),
            onClosed: makeEvent('notifications.onClosed'),
            onShown: makeEvent('notifications.onShown'),
            onPermissionLevelChanged:
                makeEvent('notifications.onPermissionLevelChanged'),
            create: function (id, options, cb) {
                if (typeof id === 'object') {
                    cb = options;
                    options = id;
                    id = '';
                }
                var p = call('notifications', 'create',
                             { id: id || '', options: options || {} });
                var q = p.then(function (v) { return v.id || ''; });
                if (cb)
                    q.then(function (v) { answerCb(cb, v); },
                           function (e) { answerCb(cb, '', e.message); });
                return q;
            },
            update: function (id, options, cb) {
                var p = call('notifications', 'update',
                             { id: id || '', options: options || {} });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(false); });
                return p;
            },
            clear: function (id, cb) {
                var p = call('notifications', 'clear', { id: id || '' });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(false); });
                return p;
            },
            getAll: function (cb) {
                var p = call('notifications', 'getAll', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb({}); });
                return p;
            },
            getPermissionLevel: function (cb) {
                return okCall('granted', cb);
            }
        });
    }

    // --- chrome.commands ---------------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('commands', {
            onCommand: makeEvent('commands.onCommand'),
            getAll: function (cb) {
                touch('chrome.commands.getAll');
                var m = manifest();
                var out = [];
                var cmds = m.commands || {};
                for (var name in cmds) {
                    var c = cmds[name] || {};
                    var key = c.suggested_key || {};
                    out.push({
                        name: name,
                        description: c.description || '',
                        shortcut: key['default'] || key['linux']
                                  || ''
                    });
                }
                return okCall(out, cb);
            }
        });
    }

    // --- chrome.contextMenus -------------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('contextMenus', {
            onClicked: makeEvent('contextMenus.onClicked'),
            onShown: makeEvent('contextMenus.onShown'),
            onHidden: makeEvent('contextMenus.onHidden'),
            create: function (props, cb) {
                var p = call('contextMenus', 'create',
                             { props: props || {} });
                var id = props && props.id;
                if (cb)
                    p.then(function () { cb(); },
                           function (e) { answerCb(cb, undefined, e.message); });
                return id !== undefined ? id : p.then(
                    function (v) { return v && v.id; });
            },
            update: function (id, props, cb) {
                var p = call('contextMenus', 'update',
                             { id: id, props: props || {} });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) { answerCb(cb, undefined, e.message); });
                return p.then(function () {});
            },
            remove: function (id, cb) {
                var p = call('contextMenus', 'remove', { id: id });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) { answerCb(cb, undefined, e.message); });
                return p.then(function () {});
            },
            removeAll: function (cb) {
                var p = call('contextMenus', 'removeAll', {});
                if (cb)
                    p.then(function () { cb(); },
                           function (e) { answerCb(cb, undefined, e.message); });
                return p.then(function () {});
            },
            refresh: function (cb) {
                return okCall(undefined, cb);
            }
        });
    }

    // --- chrome.sidePanel ------------------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('sidePanel', {
            setOptions: function (options, cb) {
                var p = call('sidePanel', 'setOptions',
                             { options: options || {} });
                note('chrome.sidePanel.setOptions: options recorded '
                     + 'host-side; Arora has no side-panel surface');
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getOptions: function (options, cb) {
                var p = call('sidePanel', 'getOptions',
                             { options: options || {} });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb({}); });
                return p;
            },
            setPanelBehavior: function (behavior, cb) {
                var p = call('sidePanel', 'setPanelBehavior',
                             { behavior: behavior || {} });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p.then(function () {});
            },
            getPanelBehavior: function (cb) {
                var p = call('sidePanel', 'getPanelBehavior', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb({}); });
                return p;
            }
        });
    }

    // --- native-but-partial namespace repair ------------------------------
    // Qt ships SUBSET bindings: a namespace can exist natively while
    // individual members are absent — Bitwarden's boot crashed on
    // chrome.runtime.onConnect.addListener, and the same shape
    // (storage.local.onChanged, idle.onStateChanged,
    // contextMenus.onClicked, ...) is the other classic.  For every
    // namespace that exists as a real object, fill missing on*
    // events with shim events and missing methods with typed-error
    // functions — real members are never touched.
    function fillNs(nsName, events, methods) {
        var ns = chrome[nsName];
        if (!ns || typeof ns !== 'object')
            return;
        for (var i = 0; events && i < events.length; ++i) {
            var en = events[i];
            if (!ns[en]) {
                try {
                    ns[en] = makeEvent(nsName + '.' + en);
                } catch (e) {
                    recordGap('chrome.' + nsName + '.' + en,
                              String(e));
                }
            }
        }
        for (var j = 0; methods && j < methods.length; ++j) {
            var mn = methods[j];
            if (typeof ns[mn] !== 'function') {
                (function (obj, member, api) {
                    try {
                        obj[member] = function () {
                            var a = arguments;
                            var cb = (a.length
                                && typeof a[a.length - 1]
                                       === 'function')
                                ? a[a.length - 1] : null;
                            touch(api);
                            return failCall(api, 'unsupported', cb);
                        };
                    } catch (e) {
                        recordGap('chrome.' + api, String(e));
                    }
                })(ns, mn, nsName + '.' + mn);
            }
        }
    }

    if (typeof chrome !== 'undefined') {
        // storage areas: a real local/session/sync/managed area can
        // lack its own onChanged event object.
        if (chrome.storage && typeof chrome.storage === 'object') {
            var stAreas = ['local', 'session', 'managed', 'sync'];
            for (var sa = 0; sa < stAreas.length; ++sa) {
                var area = chrome.storage[stAreas[sa]];
                if (area && typeof area === 'object'
                        && !area.onChanged) {
                    try {
                        area.onChanged = makeEvent(
                            'storage.' + stAreas[sa] + '.onChanged');
                    } catch (e) {
                        recordGap('chrome.storage.' + stAreas[sa]
                                  + '.onChanged', String(e));
                    }
                }
            }
        }
        // chrome.extension: if a native subset exists, fill it with
        // the same hosted-context implementations the absent-ns
        // block above installs.
        var ext = chrome.extension;
        if (ext && typeof ext === 'object') {
            if (typeof ext.getURL !== 'function') {
                try {
                    ext.getURL = function (p) {
                        return 'chrome-extension://' + cfg.id + '/'
                               + String(p || '');
                    };
                } catch (e) { recordGap('chrome.extension.getURL',
                                        String(e)); }
            }
            if (typeof ext.getViews !== 'function') {
                try {
                    ext.getViews = extensionGetViews;
                } catch (e) { recordGap('chrome.extension.getViews',
                                        String(e)); }
            }
            if (typeof ext.getBackgroundPage !== 'function') {
                try {
                    ext.getBackgroundPage = extensionGetBackgroundPage;
                } catch (e) { recordGap(
                        'chrome.extension.getBackgroundPage',
                        String(e)); }
            }
            fillNs('extension', ['onRequest', 'onRequestExternal'],
                   ['isAllowedIncognitoAccess',
                    'isAllowedFileSchemeAccess']);
            if (ext.inIncognitoContext === undefined) {
                try { ext.inIncognitoContext = false; }
                catch (e) {}
            }
        }
        fillNs('idle', ['onStateChanged'],
               ['queryState', 'setDetectionInterval']);
        // When chrome.tabs resisted the proxy install (locked
        // native accessor) the block above already filled every
        // tabsNs member; this covers the rest of the surface with
        // typed stubs.  On a shim proxy every check is a no-op.
        fillNs('tabs', tabsEvents,
               ['get', 'getCurrent', 'query', 'create', 'update',
                'sendMessage', 'remove', 'reload', 'duplicate',
                'highlight', 'move', 'goBack', 'goForward', 'discard',
                'detectLanguage', 'captureVisibleTab',
                'executeScript', 'insertCSS', 'removeCSS',
                'setZoom', 'getZoom', 'setZoomSettings',
                'getZoomSettings', 'connect', 'sendRequest',
                'getAllInWindow', 'getSelected']);
        fillNs('windows',
               ['onFocusChanged', 'onCreated', 'onRemoved',
                'onBoundsChanged'],
               ['get', 'getCurrent', 'getLastFocused', 'getAll',
                'create', 'update', 'remove']);
        fillNs('contextMenus',
               ['onClicked', 'onShown', 'onHidden'],
               ['create', 'update', 'remove', 'removeAll', 'refresh']);
        fillNs('notifications',
               ['onClicked', 'onButtonClicked', 'onClosed',
                'onShown', 'onPermissionLevelChanged'],
               ['create', 'update', 'clear', 'getAll',
                'getPermissionLevel']);
        fillNs('webNavigation',
               ['onBeforeNavigate', 'onCommitted', 'onCompleted',
                'onDOMContentLoaded', 'onErrorOccurred',
                'onCreatedNavigationTarget', 'onHistoryStateUpdated',
                'onReferenceFragmentUpdated', 'onTabReplaced'],
               ['getFrame', 'getAllFrames']);
        fillNs('permissions', ['onAdded', 'onRemoved'],
               ['contains', 'getAll', 'request', 'remove']);
        fillNs('commands', ['onCommand'], ['getAll']);
        fillNs('browserAction', ['onClicked'],
               ['setTitle', 'getTitle', 'setBadgeText',
                'getBadgeText', 'setBadgeBackgroundColor',
                'getBadgeBackgroundColor', 'setPopup', 'getPopup',
                'enable', 'disable']);
        fillNs('webRequest',
               ['onBeforeRequest', 'onBeforeSendHeaders',
                'onSendHeaders', 'onHeadersReceived',
                'onAuthRequired', 'onResponseStarted',
                'onBeforeRedirect', 'onCompleted',
                'onErrorOccurred'],
               ['handlerBehaviorChanged']);
        fillNs('declarativeNetRequest',
               ['onRuleMatchedDebug'],
               ['updateDynamicRules', 'updateSessionRules',
                'getEnabledRulesets', 'updateEnabledRulesets',
                'getDynamicRules', 'getSessionRules',
                'setExtensionActionOptions',
                'getMatchedRules', 'isRegexSupported',
                'testMatchOutcome']);
        fillNs('declarativeContent', [],
               []);
        fillNs('sessions', ['onChanged'],
               ['getRecentlyClosed', 'getDevices', 'restore']);
        fillNs('bookmarks',
               ['onCreated', 'onRemoved', 'onChanged', 'onMoved',
                'onChildrenReordered', 'onImportBegan',
                'onImportEnded'],
               ['get', 'getChildren', 'getRecent', 'getTree',
                'getSubTree', 'search', 'create', 'move', 'update',
                'remove', 'removeTree']);
        fillNs('history',
               ['onVisited', 'onVisitRemoved'],
               ['search', 'getVisits', 'addUrl', 'deleteUrl',
                'deleteRange', 'deleteAll']);
        fillNs('omnibox',
               ['onInputStarted', 'onInputChanged',
                'onInputEntered', 'onInputCancelled'],
               ['setDefaultSuggestion']);
        fillNs('topSites', [], ['get']);
        fillNs('management',
               ['onInstalled', 'onUninstalled', 'onEnabled',
                'onDisabled'],
               ['getAll', 'get', 'getSelf', 'getPermissionWarningsById',
                'getPermissionWarningsByManifest',
                'setEnabled', 'uninstall', 'uninstallSelf',
                'launchApp', 'generateAppForLink']);
        fillNs('browsingData', [],
               ['settings', 'remove', 'removeAppcache', 'removeCache',
                'removeCacheStorage', 'removeCookies',
                'removeDownloads', 'removeFileSystems',
                'removeFormData', 'removeHistory',
                'removeIndexedDB', 'removeLocalStorage',
                'removeServiceWorkers', 'removePluginData',
                'removePasswords', 'removeWebSQL']);
        fillNs('scripting', [],
               ['executeScript', 'insertCSS', 'removeCSS',
                'registerContentScripts', 'getRegisteredContentScripts',
                'unregisterContentScripts', 'updateContentScripts']);
        fillNs('offscreen', [],
               ['createDocument', 'closeDocument', 'hasDocument']);
        fillNs('sidePanel', [],
               ['setOptions', 'getOptions', 'setPanelBehavior',
                'getPanelBehavior', 'open']);
        fillNs('tabGroups',
               ['onCreated', 'onUpdated', 'onMoved', 'onRemoved'],
               ['get', 'query', 'update', 'move', 'ungroup']);
        fillNs('cookies',
               ['onChanged'],
               ['get', 'getAll', 'set', 'remove',
                'getAllCookieStores']);
        fillNs('identity',
               ['onSignInChanged'],
               ['getAccounts', 'getAuthToken',
                'clearAllCachedAuthTokens', 'getRedirectURL',
                'launchWebAuthFlow', 'getProfileUserInfo',
                'removeCachedAuthToken']);
        fillNs('downloads',
               ['onCreated', 'onErased', 'onChanged',
                'onDeterminingFilename'],
               ['download', 'search', 'pause', 'resume', 'cancel',
                'getFileIcon', 'open', 'show', 'showDefaultFolder',
                'erase', 'removeFile', 'acceptDanger', 'drag',
                'setShelfEnabled']);
        fillNs('gcm',
               ['onMessage', 'onMessagesDeleted', 'onSendError'],
               ['register', 'unregister', 'send',
                'setMaxSendMessageSize']);
        fillNs('tts',
               ['onPause', 'onResume', 'onReset', 'onStart',
                'onEnd', 'onError', 'onCancel', 'onInterrupted',
                'onWord', 'onSentence', 'onMarker', 'onBoundary',
                'onEvent'],
               ['speak', 'stop', 'pause', 'resume', 'isSpeaking',
                'getVoices']);
        fillNs('input.ime',
               ['onActivate', 'onDeactivated', 'onFocus',
                'onBlur', 'onInputContextUpdate',
                'onKeyEvent', 'onCandidateClicked',
                'onMenuItemActivated', 'onSurroundingTextChanged',
                'onReset'],
               []);
        fillNs('privacy', [],
               []);
        fillNs('nativeMessaging', [], []);
    }

    // --- everything else: honest typed-error namespaces --------------------
    // Absent namespaces get a guarded proxy: method calls answer a
    // TYPED 'unsupported' error (recorded into gaps -> health) and
    // on* properties yield real shim events whose registrations are
    // noted.  Nothing claims to work that doesn't.
    var unsupportedNames = [
        'windows', 'cookies', 'bookmarks', 'identity', 'gcm',
        'nativeMessaging', 'privacy', 'topSites', 'history',
        'scripting', 'offscreen', 'omnibox', 'devtools',
        'fontSettings', 'tabGroups', 'userScripts', 'webRequest',
        'declarativeNetRequest', 'declarativeContent', 'idle',
        'browsingData', 'management', 'sessions', 'downloads',
        'power', 'system', 'tts', 'enterprise', 'debugger',
        'processes', 'readingList', 'search', 'favicon', 'tabCapture',
        'input', 'dns', 'documentScan', 'fileSystemProvider',
        'printing', 'vpnProvider', 'wallpaper', 'audio',
        'accessibilityFeatures', 'autofill', 'certificateProvider',
        'contentSettings', 'desktopCapture', 'displaySource',
        'fileBrowserHandler', 'loginState', 'mediaPerception',
        'platformKeys', 'printerProvider', 'signedInDevices',
        'systemIndicator', 'chromeOs'
    ];
    function unsupportedNs(path) {
        var noted = {};
        return new Proxy({}, {
            get: function (t, prop) {
                if (prop === 'then')
                    return undefined;    // stay non-thenable
                if (typeof prop !== 'string')
                    return undefined;
                var p = path + '.' + prop;
                if (prop.indexOf('on') === 0
                        && prop.length > 2
                        && prop.charAt(2) === prop.charAt(2).toUpperCase()) {
                    if (!shim.events[p]) {
                        makeEvent(p, { onAdd: function () {
                            if (!noted[p]) {
                                noted[p] = 1;
                                note(p + ' registered — no event '
                                     + 'source for this namespace');
                            }
                        } });
                    }
                    return shim.events[p];
                }
                if (!noted[p]) {
                    noted[p] = 1;
                    note(p + ' accessed — ' + path
                         + ' has no Arora surface');
                }
                return function () {
                    var cb = null;
                    var a = arguments;
                    if (a.length
                            && typeof a[a.length - 1] === 'function')
                        cb = a[a.length - 1];
                    touch(p);
                    return failCall(p, 'unsupported', cb);
                };
            },
            has: function () { return true; },
            set: function () { return true; }
        });
    }
    if (typeof chrome !== 'undefined') {
        for (var u = 0; u < unsupportedNames.length; ++u) {
            var ns = unsupportedNames[u];
            try {
                // repairable=false — if the engine materializes a
                // real binding later it wins over our typed-error
                // proxy, never the reverse.
                if (chrome[ns] === undefined)
                    installNs(ns, unsupportedNs('chrome.' + ns),
                              false);
            } catch (e) {}
        }
        // windows.* does have a real event source (the EXT16 hub
        // pushes onCreated/onRemoved/onFocusChanged): pre-create them
        // so the typed-error proxy hands out real events without the
        // 'no event source' note; windows.get et al still answer
        // honest 'unsupported' errors.
        if (chrome.windows) {
            var winEvents = ['onCreated', 'onRemoved',
                             'onFocusChanged'];
            for (var we = 0; we < winEvents.length; ++we) {
                var wn = 'windows.' + winEvents[we];
                if (!shim.events[wn])
                    makeEvent(wn);
            }
        }
    }

    // --- compat-surface audit -----------------------------------------
    // The EXT15 checklist: every surface the EXT13 measurement (plus
    // the real-boot delta found since) requires to EXIST.  Presence
    // here does not claim the member fully works — proxied
    // namespaces answer typed errors — it proves the access can
    // never be an unshimmed TypeError.  Smokes/tests diff
    // audit().missing against the empty list.
    var AUDIT_PATHS = [
        'chrome.runtime.id', 'chrome.runtime.getURL',
        'chrome.runtime.getManifest', 'chrome.runtime.sendMessage',
        'chrome.runtime.connect', 'chrome.runtime.getPlatformInfo',
        'chrome.runtime.getContexts', 'chrome.runtime.reload',
        'chrome.runtime.setUninstallURL', 'chrome.runtime.openOptionsPage',
        'chrome.runtime.requestUpdateCheck',
        'chrome.runtime.sendNativeMessage',
        'chrome.runtime.connectNative',
        'chrome.runtime.getPackageDirectoryEntry',
        'chrome.runtime.onMessage.addListener',
        'chrome.runtime.onInstalled.addListener',
        'chrome.runtime.onStartup.addListener',
        'chrome.runtime.onConnect.addListener',
        'chrome.runtime.onConnectExternal.addListener',
        'chrome.runtime.onMessageExternal.addListener',
        'chrome.runtime.onSuspend.addListener',
        'chrome.runtime.onUpdateAvailable.addListener',
        'chrome.extension.getURL', 'chrome.extension.getViews',
        'chrome.extension.getBackgroundPage',
        'chrome.extension.isAllowedIncognitoAccess',
        'chrome.i18n.getMessage', 'chrome.i18n.getUILanguage',
        'chrome.storage.local.get', 'chrome.storage.local.set',
        'chrome.storage.local.onChanged.addListener',
        'chrome.storage.session.get', 'chrome.storage.session.set',
        'chrome.storage.managed.get',
        'chrome.storage.onChanged.addListener',
        'chrome.alarms.create', 'chrome.alarms.get',
        'chrome.alarms.getAll', 'chrome.alarms.clear',
        'chrome.alarms.clearAll',
        'chrome.alarms.onAlarm.addListener',
        'chrome.action.setTitle', 'chrome.action.getTitle',
        'chrome.action.setBadgeText', 'chrome.action.setPopup',
        'chrome.action.onClicked.addListener',
        'chrome.browserAction.setTitle',
        'chrome.browserAction.onClicked.addListener',
        'chrome.tabs.getCurrent', 'chrome.tabs.query',
        'chrome.tabs.get', 'chrome.tabs.create', 'chrome.tabs.update',
        'chrome.tabs.sendMessage',
        'chrome.tabs.onCreated.addListener',
        'chrome.tabs.onUpdated.addListener',
        'chrome.tabs.onActivated.addListener',
        'chrome.tabs.onRemoved.addListener',
        'chrome.tabs.onReplaced.addListener',
        'chrome.tabs.onHighlighted.addListener',
        'chrome.webNavigation.onBeforeNavigate.addListener',
        'chrome.webNavigation.onCommitted.addListener',
        'chrome.webNavigation.onCompleted.addListener',
        'chrome.webNavigation.onDOMContentLoaded.addListener',
        'chrome.webNavigation.onErrorOccurred.addListener',
        'chrome.commands.onCommand.addListener',
        'chrome.commands.getAll',
        'chrome.contextMenus.create', 'chrome.contextMenus.update',
        'chrome.contextMenus.remove', 'chrome.contextMenus.removeAll',
        'chrome.contextMenus.onClicked.addListener',
        'chrome.sidePanel.setOptions', 'chrome.sidePanel.getOptions',
        'chrome.sidePanel.setPanelBehavior',
        'chrome.notifications.create', 'chrome.notifications.getAll',
        'chrome.notifications.onClicked.addListener',
        'chrome.notifications.onButtonClicked.addListener',
        'chrome.permissions.contains', 'chrome.permissions.getAll',
        'chrome.permissions.onAdded.addListener',
        'chrome.idle.onStateChanged.addListener',
        'chrome.idle.setDetectionInterval', 'chrome.idle.queryState',
        'chrome.windows.getCurrent', 'chrome.windows.getAll',
        'chrome.windows.onFocusChanged.addListener',
        'chrome.sessions.getRecentlyClosed',
        'chrome.sessions.onChanged.addListener',
        'chrome.bookmarks.getTree',
        'chrome.history.search', 'chrome.history.onVisited.addListener',
        'chrome.scripting.executeScript',
        'chrome.offscreen.createDocument',
        'chrome.management.getSelf',
        'chrome.browsingData.remove',
        'chrome.webRequest.onBeforeRequest.addListener',
        'chrome.webRequest.onHeadersReceived.addListener',
        'chrome.webRequest.onAuthRequired.addListener',
        'chrome.declarativeNetRequest.updateDynamicRules',
        'chrome.omnibox.onInputEntered.addListener',
        'chrome.topSites.get',
        'chrome.cookies.get', 'chrome.cookies.onChanged.addListener',
        'chrome.identity.getProfileUserInfo',
        'chrome.downloads.download',
        'chrome.gcm.register', 'chrome.tts.getVoices',
        'chrome.tabGroups.onCreated.addListener',
        'chrome.privacy', 'chrome.devtools', 'chrome.fontSettings',
        'chrome.enterprise'
    ];
    shim.audit = function () {
        var missing = [];
        var present = 0;
        for (var i = 0; i < AUDIT_PATHS.length; ++i) {
            var parts = AUDIT_PATHS[i].split('.');
            var o = window;
            for (var j = 0; j < parts.length; ++j) {
                if (o === undefined || o === null)
                    break;
                o = o[parts[j]];
            }
            if (o === undefined || o === null)
                missing.push(AUDIT_PATHS[i]);
            else
                ++present;
        }
        return { checked: AUDIT_PATHS.length, present: present,
                 missing: missing };
    };

    // Chromium lazily installs (or rewrites) extension api bindings
    // AFTER DocumentCreation injection — the hosted Bitwarden page
    // swapped our chrome.tabs proxy for the native subset mid-boot,
    // measured via the compat audit.  Re-assert the shim surface at
    // a few bootstrap ticks; every pass is an idempotent no-op when
    // nothing was displaced.
    if (typeof window !== 'undefined'
            && typeof window.setTimeout === 'function') {
        window.setTimeout(repairInstalls, 0);
        window.setTimeout(repairInstalls, 50);
        // A bounded interval outlives the bootstrap window — the
        // binding rewrite landed anywhere between a few and a few
        // hundred ms on the measured boot.
        var repairs = 0;
        var repairTimer = window.setInterval(function () {
            repairInstalls();
            if (++repairs >= 40)          // ~10 s ceiling
                window.clearInterval(repairTimer);
        }, 250);
        if (typeof window.addEventListener === 'function')
            window.addEventListener('load', repairInstalls);
    }

    // Diagnostics surface for tests/smokes.
    shim.probe = function () {
        return {
            version: shim.version,
            bridgeUp: !!host.bridgeUp,
            gaps: shim.gaps.slice(),
            notes: shim.notes.slice(),
            touched: shim.touched,
            audit: shim.audit()
        };
    };
})();
