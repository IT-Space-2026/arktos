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
 * __arktosShim.gaps and forwarded via invoke('shim','gap').  APIs that
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
 *   chrome.extension    — getURL; ER15 getViews composes from the
 *                         runtime's pushed context registry (own
 *                         Window + capability handles for siblings);
 *                         getBackgroundPage answers window in the
 *                         background context and the documented MV3
 *                         null elsewhere
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
 *   chrome.notifications— ER12: create/update/clear/getAll over the
 *                         host's record registry + freedesktop
 *                         presentation; onClicked/onButtonClicked/
 *                         onClosed/onShown dispatch back from the
 *                         daemon through hostEvent
 *   chrome.idle         — ER13: queryState/setDetectionInterval/
 *                         onStateChanged over the host's in-app
 *                         input watcher (in-app activity only;
 *                         'locked' never reported)
 *   chrome.commands     — ER14: onCommand dispatched by the C++
 *                         shortcut registry; getAll answers the
 *                         real bound-state rows
 *   chrome.contextMenus — create/update/remove* registry host-side;
 *                         onClicked event (rendering is EXT16)
 *   chrome.sidePanel    — options recorded host-side; no panel UI
 *   chrome.management   — ER15: getSelf answers the real ExtensionInfo
 *                         shape (id/name/version/enabled/installType)
 *                         host-side; other members typed no-permission/
 *                         unsupported
 *   chrome.sidebarAction— ER15: Firefox-shape sugar when the manifest
 *                         declares sidebar_action — panel/title/icon
 *                         bookkeeping; open/close typed unsupported
 *   other absent ns     — unsupportedNs(): calls answer typed errors,
 *                         on* event registration is recorded
 *
 * Keep this file dependency-free and ES5-plus-Promise: it is inlined
 * into a QWebEngineScript source.
 */
(function () {
    'use strict';
    var host = window.__arktosHost;
    var cfg = window.__ARKTOS_HOST_CONFIG;
    if (!host || !cfg || !cfg.id || window.__arktosShim)
        return;

    var LOG_CAP = 500;
    var shim = window.__arktosShim = {
        version: 3,
        gaps: [],       // typed errors observed (health surface)
        notes: [],      // benign diagnostics (recorded registrations,
                        // accepted-but-unsourced state)
        touched: {},    // api -> count (the live delta measurement)
        contexts: [],   // ER15 — registered sibling contexts, pushed
                        // by the runtime ('__arktos.contexts') so
                        // chrome.extension.getViews answers
                        // synchronously
        events: {},     // name -> event objects the host pushes into
        seenMids: {},   // relay/native message dedupe (bounded TTL)
        recentDeliveries: {}, // content-hash dedupe for relay+native
        seq: 0
    };

    function note(text) {
        if (shim.notes.length < LOG_CAP)
            shim.notes.push(String(text));
        // Through call() so a diagnostic recorded before the
        // handshake lands rides the bridge queue — health must see
        // boot-time gaps, not just post-bridge ones.
        call('shim', 'note', { message: String(text) })
            .catch(function () {});
    }

    function recordGap(api, error) {
        var text = api + ': ' + error;
        for (var i = 0; i < shim.gaps.length; ++i)
            if (shim.gaps[i] === text)
                return;
        if (shim.gaps.length < LOG_CAP)
            shim.gaps.push(text);
        call('shim', 'gap',
             { api: String(api), error: String(error) })
            .catch(function () {});
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
        if (name === '__arktos.relay') {
            onRelayMessage(payload);
            return;
        }
        if (name === '__arktos.relayResponse') {
            onRelayResponse(payload);
            return;
        }
        if (name === '__arktos.deliverResponse') {
            onDeliverResponse(payload);
            return;
        }
        if (name === '__arktos.execResult') {
            onExecResult(payload);
            return;
        }
        if (name === '__arktos.offscreenDone') {
            onOffscreenDone(payload);
            return;
        }
        if (name === '__arktos.contexts') {
            // ER15: the runtime's registered-context snapshot —
            // getViews composes its return array from this.
            if (payload && payload.contexts)
                shim.contexts = payload.contexts;
            return;
        }
        if (name === '__arktos.portConnect') {
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
        if (name === '__arktos.portMessage') {
            if (payload && shim.ports[payload.portId])
                shim.ports[payload.portId].onMessage
                    .__fire([payload.msg,
                             shim.ports[payload.portId]]);
            return;
        }
        if (name === '__arktos.portDisconnect') {
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
        // tabs.onActivated has NO entry on purpose: its single
        // argument IS the {tabId, windowId} activeInfo object, so a
        // named-object payload fires as one arg — key mapping would
        // wrongly spread it into two scalars.
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
    // honest in every context kind.  ER15: getViews composes its
    // array from the runtime's pushed context registry — the
    // calling document contributes its real Window; a sibling
    // context gets a capability handle (its true documentUrl, a
    // real location.reload() routed through the bridge, focus
    // honestly unmeasured) — never a fake Window.  getBackgroundPage
    // still answers null for non-background contexts because a
    // Window cannot cross documents.
    function thisViewType() {
        return host.kind === 'background'
            ? 'BACKGROUND'
            : (contextKind() === 'popup' ? 'POPUP' : 'TAB');
    }
    function viewHandle(ctx) {
        return {
            // Explicitly NOT a Window — extension code that needs
            // the real DOM can only reach it in its own document.
            __arktosViewHandle: true,
            contextId: ctx.contextId,
            contextType: ctx.contextType,
            tabId: ctx.tabId,
            windowId: ctx.windowId,
            location: {
                href: String(ctx.documentUrl || ''),
                reload: function () {
                    call('extension', 'reloadView',
                         { contextId: ctx.contextId })
                        .catch(function (e) {
                            recordGap(
                                'chrome.extension.getViews.reload',
                                String(e && e.code || e));
                        });
                }
            },
            document: {
                hasFocus: function () {
                    if (!shim.__focusNoteDone) {
                        shim.__focusNoteDone = 1;
                        note('chrome.extension.getViews: another '
                             + 'document\'s focus is unmeasured — '
                             + 'handle hasFocus() answers false');
                    }
                    return false;
                }
            }
        };
    }
    function extensionGetViews(fetchProperties) {
        touch('chrome.extension.getViews');
        var type = fetchProperties && fetchProperties.type
            ? String(fetchProperties.type).toUpperCase() : null;
        var tabId = fetchProperties ? fetchProperties.tabId
                                    : undefined;
        var windowId = fetchProperties ? fetchProperties.windowId
                                       : undefined;
        var ctxs = shim.contexts;
        if (!ctxs || !ctxs.length) {
            // The registry snapshot has not arrived yet — answer
            // for the one context we can prove: our own document.
            if (!type || type === thisViewType())
                return [window];
            note('chrome.extension.getViews(' + type + '): only the '
                 + thisViewType() + ' context is enumerable here');
            return [];
        }
        var views = [];
        var sawSelf = false;
        for (var i = 0; i < ctxs.length; ++i) {
            var c = ctxs[i];
            if (!c)
                continue;
            // Offscreen documents are not views in Chrome's sense —
            // they carry no visible window the API enumerates.
            if (c.contextType === 'OFFSCREEN_DOCUMENT')
                continue;
            if (type && String(c.contextType) !== type)
                continue;
            if (tabId !== undefined && tabId !== null
                    && Number(c.tabId) !== Number(tabId))
                continue;
            if (windowId !== undefined && windowId !== null
                    && Number(c.windowId) !== Number(windowId))
                continue;
            if (!sawSelf
                    && (c.contextId === shim.contextId
                        || String(c.documentUrl)
                           === String(location.href))) {
                views.push(window);
                sawSelf = true;
                continue;
            }
            views.push(viewHandle(c));
        }
        // The snapshot may predate this context's registration —
        // never drop the caller's own window when it fits the
        // filter.
        if (!sawSelf && (!type || type === thisViewType()))
            views.unshift(window);
        return views;
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
            .then(function (id) {
                shim.contextId = id;
                // The registration lands a '__arktos.contexts' push
                // as well; fetching here covers the case where the
                // push raced the contextId assignment.
                call('runtime', 'getContexts', {})
                    .then(function (v) {
                        if (v && v.length)
                            shim.contexts = v;
                    })
                    .catch(function () {});
            })
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
        // ER15: manifest-gated installs (sidebarAction) had to wait
        // for this — manifest() is only the real manifest once the
        // handshake reply has landed.
        installSidebarAction();
    };
    (function waitBridge() {
        if (host.bridgeUp && host.bridge) {
            wireHostEvent();
            flushBridgeQueue();
            installSidebarAction();
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

    // Member-level identity assert for members Qt binds NATIVELY but
    // we must serve ourselves (ER15 management.getSelf — the native
    // answer lacks the fields our records keep).  A plain assignment
    // silently no-ops on a non-writable native member — sloppy mode
    // never throws — so verify after the write and fall back to
    // defineProperty.  Still-locked after both -> recorded gap.
    function forceMember(obj, key, val, api) {
        if (obj[key] === val)
            return;
        safeSet(obj, key, val, api);
        if (obj[key] !== val) {
            try {
                Object.defineProperty(obj, key, {
                    configurable: true,
                    writable: true,
                    enumerable: true,
                    value: val
                });
            } catch (e) {}
        }
        if (obj[key] !== val)
            recordGap(api, 'native-member-locked');
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
    var offscreenNsRef = null;   // ER10 — member-identity re-assert
    var mgmtNsRef = null;        // ER15 — member-identity re-assert
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
                if (tk === '__arktosShim')
                    continue;
                // sendMessage/connect are identity overrides like
                // runtime.connect: the native binding can only reach
                // the dead service worker, the hosted route is the
                // live one — fill them even over a native member.
                if (chrome.tabs[tk] === undefined
                        || tk === 'sendMessage' || tk === 'connect')
                    safeSet(chrome.tabs, tk, tabsNsRef[tk],
                            'chrome.tabs.' + tk);
            }
        }
        if (!ok && !chrome.tabs)
            recordGap('chrome.tabs', 'namespace-locked');
    }

    // Assert chrome.management is OURS — same shape as assertTabs:
    // Qt binds a native namespace whose getSelf answers Chromium's
    // records (no manifestVersion, not our enabled state), and the
    // mid-boot binding swap rewrites 'management' by assignment.  A
    // getter+no-op-setter descriptor shrugs that off; a locked
    // native object degrades to per-member force asserts, and total
    // refusal lands as a recorded gap.
    function assertManagement() {
        if (!mgmtNsRef)
            return;
        try {
            if (chrome.management === mgmtNsRef)
                return;
        } catch (e) {
            return;
        }
        var ok = false;
        try {
            Object.defineProperty(chrome, 'management', {
                configurable: true,
                enumerable: true,
                get: function () { return mgmtNsRef; },
                set: function () {}
            });
            ok = (chrome.management === mgmtNsRef);
        } catch (e) {}
        if (!ok) {
            try {
                chrome.management = mgmtNsRef;
                ok = (chrome.management === mgmtNsRef);
            } catch (e2) {}
        }
        if (!ok && chrome.management
                && typeof chrome.management === 'object')
            for (var mk in mgmtNsRef)
                forceMember(chrome.management, mk, mgmtNsRef[mk],
                            'chrome.management.' + mk);
        if (!ok && !chrome.management)
            recordGap('chrome.management', 'namespace-locked');
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
        // ER15: extension.getViews/getBackgroundPage are identity
        // overrides for the same reason — a native binding cannot
        // see the hosted background.
        var ext4 = chrome.extension;
        if (ext4 && typeof ext4 === 'object') {
            if (ext4.getViews !== extensionGetViews)
                forceMember(ext4, 'getViews', extensionGetViews,
                            'chrome.extension.getViews');
            if (ext4.getBackgroundPage !== extensionGetBackgroundPage)
                forceMember(ext4, 'getBackgroundPage',
                            extensionGetBackgroundPage,
                            'chrome.extension.getBackgroundPage');
        }
        // ER15: management's members are identity overrides too —
        // Qt injects a native chrome.management whose getSelf
        // answers Chromium's records, not ours (no manifestVersion,
        // no our-side enabled state).
        try {
            assertManagement();
        } catch (e) {}
        // ER10: offscreen's members are identity overrides (the
        // native binding may exist and MUST be replaced — its
        // document would carry no bridge/relay), so re-assert by
        // identity the same way runtime.connect is.
        if (offscreenNsRef) {
            try {
                if (!chrome.offscreen
                        || typeof chrome.offscreen !== 'object') {
                    installNs('offscreen', offscreenNsRef);
                } else {
                    for (var om in offscreenNsRef) {
                        if (chrome.offscreen[om]
                                !== offscreenNsRef[om])
                            safeSet(chrome.offscreen, om,
                                    offscreenNsRef[om],
                                    'chrome.offscreen.' + om);
                    }
                }
            } catch (e) {}
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
    var deliverWait = {};  // mid -> {cb, resolve, reject, done} —
                           // ER02 tabs.sendMessage host->page waits
    var execWait = {};     // execId -> {cb, resolve, reject, done,
                           // fail} — ER07 scripting.executeScript
                           // waits on '__arktos.execResult'
    var docWait = {};      // docId -> {cb, resolve, reject, done,
                           // fail} — ER10 offscreen.createDocument
                           // waits on '__arktos.offscreenDone'
    var relayPeers = 0;

    // ER02: the page agent's 'respond' lands here — resolve the
    // matching tabs.sendMessage with Chrome's own error vocabulary:
    // an answered delivery passes its value through; an empty answer
    // distinguishes "receiving end does not exist" (no listeners)
    // from "port closed" (listeners present, none responded).
    function onDeliverResponse(payload) {
        if (!payload || !payload.mid)
            return;
        var entry = deliverWait[payload.mid];
        if (!entry || entry.done)
            return;
        var v = (payload.response === null
                 || payload.response === undefined)
            ? undefined : payload.response;
        if (v !== undefined) {
            entry.done = true;
            delete deliverWait[payload.mid];
            answerCb(entry.cb, v);
            entry.resolve(v);
            return;
        }
        // entry.fail owns the done flag — marking it here would
        // turn the call below into a silent no-op and strand the
        // callback AND the safety timer on a done entry.
        entry.fail(payload.hadListeners
            ? 'The message port closed before a response was received.'
            : 'Could not establish connection. '
              + 'Receiving end does not exist.');
    }

    // ER07: the per-frame results of a scripting.executeScript land
    // here.  Chrome resolves the call once injection has started —
    // even when the script threw — so the payload is always the
    // results array (a throwing frame carries `error` on its entry,
    // the documented InjectionResult shape); only injection-level
    // failures rejected the call earlier via call()'s typed error.
    function onExecResult(payload) {
        if (!payload || !payload.execId)
            return;
        var entry = execWait[payload.execId];
        if (!entry || entry.done)
            return;
        var results = payload.results || [];
        if (entry.css) {
            // ER08 — insertCSS/removeCSS resolve void like Chrome,
            // but a frame-level failure (dead document, no insertion
            // host) is a real injection failure and rejects the call.
            for (var i = 0; i < results.length; ++i) {
                if (results[i] && results[i].error) {
                    entry.fail(String(results[i].error));
                    return;
                }
            }
            entry.done = true;
            delete execWait[payload.execId];
            answerCb(entry.cb, undefined);
            entry.resolve(undefined);
            return;
        }
        entry.done = true;
        delete execWait[payload.execId];
        answerCb(entry.cb, results);
        entry.resolve(results);
    }

    // ER10: offscreen.createDocument resolves when the spawned
    // document's shim registered its relay context — the earliest
    // moment sendMessage really reaches it.  A typed failure lands
    // as {ok:false,error}; 'single-document' maps onto Chrome's
    // verbatim message (Bitwarden's withDocument compares
    // e.message === "Only a single offscreen document may be
    // created." — the exact string is part of the contract).
    function onOffscreenDone(payload) {
        if (!payload || !payload.docId)
            return;
        var entry = docWait[payload.docId];
        if (!entry || entry.done)
            return;
        if (payload.ok) {
            entry.done = true;
            delete docWait[payload.docId];
            answerCb(entry.cb);
            entry.resolve();
            return;
        }
        entry.fail(payload.error || 'offscreen-failed');
    }

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
        if (msg && msg.__arktosMsg && msg.__arktosMsg.mid)
            return msg.__arktosMsg;
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
        // ER01: page-originated messages carry a full C++-built
        // MessageSender ({id, url: frameUrl, origin: page origin,
        // tab, frameId}) — forward it verbatim so internal-only
        // gates still see the honest external shape; a shim-context
        // sender keeps its extension origin (senderIsInternal-style
        // gates like Bitwarden's accept it).
        var sender = (env && env.sender)
            || { id: env && env.extId || cfg.id,
                 url: env && env.senderUrl || undefined,
                 origin: 'chrome-extension://'
                         + (env && env.extId || cfg.id) };
        for (var i = 0; i < listeners.length; ++i) {
            try {
                var r = listeners[i].call(
                    null, msg, sender, respond);
                if (r === true)
                    wantAsync = true;
                else if (r && typeof r.then === 'function') {
                    // MV3: a returned Promise settles into the
                    // response — hold the channel open for it like
                    // Chrome does.  Any OTHER return value is not a
                    // response: Bitwarden's allMessages$ tap returns
                    // its RxJS Subscriber, and answering with it
                    // latched the channel before the real async
                    // sendResponse (i.e. dropped
                    // getUrlAutofillTargetingRules' answer).
                    wantAsync = true;
                    r.then(function (v) { respond(v); },
                           function () { respond(undefined); });
                }
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
                var envelope = { __arktosMsg: { mid: mid, msg: msg,
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
        // must ALSO land on the shim event the __arktos.portConnect
        // delivery fires.  The real Bitwarden popup opened its
        // 'session' port into exactly this dead end (the bg-side port
        // registered but onConnect never fired — Angular's bootstrap
        // awaited the session IPC forever).  Dual-register so a real
        // external connect still reaches the listener.
        // ER05: the lifecycle events take the same treatment — a
        // native onInstalled/onStartup object may exist in the
        // schema while no embedder delegate ever dispatches it, so
        // the hosted runtime's push is the only source that can fire
        // them; listeners must land on the shim event.
        var connEvents = ['onConnect', 'onConnectExternal',
                          'onMessageExternal', 'onInstalled',
                          'onStartup'];
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
    // The isAllowed*Access verdicts live host-side in invokeExtension
    // — the bridge call is what lets a future incognito/file: grant
    // surface flip the answer in one place.  Today's truthful reply
    // is false either way, which is also the bridge-failure fallback
    // (never a typed gap for a question we can answer).
    function extensionAccessQuery(method, cb) {
        var p = call('extension', method, {});
        if (cb)
            p.then(function (v) { answerCb(cb, v === true); },
                   function () { cb(false); });
        return p;
    }
    var extAllowedIncognitoRef = function (cb) {
        return extensionAccessQuery('isAllowedIncognitoAccess', cb);
    };
    var extAllowedFileSchemeRef = function (cb) {
        return extensionAccessQuery('isAllowedFileSchemeAccess', cb);
    };
    if (typeof chrome !== 'undefined') {
        installNs('extension', {
            getURL: function (p) {
                return 'chrome-extension://' + cfg.id + '/'
                       + String(p || '');
            },
            // Window objects cannot cross the bridge — getViews
            // hands back the caller's own Window for its own
            // context plus honest capability handles for sibling
            // contexts (true url, working reload()); EXT17 page
            // contexts answer null for the background page rather
            // than the wrong object.
            getViews: extensionGetViews,
            getBackgroundPage: extensionGetBackgroundPage,
            inIncognitoContext: false,
            isAllowedIncognitoAccess: extAllowedIncognitoRef,
            isAllowedFileSchemeAccess: extAllowedFileSchemeRef,
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
    // __arktosShim.storageArea().
    // ER17 — the onChanged fan-out.  The engine's storage delegate
    // never emits storage.onChanged in ANY context (verified: a
    // listener on the real area hears nothing after its own set),
    // so the shim fires it itself: the writer's own
    // storage.onChanged + storage.<area>.onChanged run
    // synchronously in the mutation's resolution tick — the same
    // delivery Chrome gives the writing context — then the
    // computed record posts to the backend's storage.changed op,
    // which fans it out to the extension's OTHER shim contexts
    // (the op excludes the poster, so nothing double-delivers).
    // Between the two halves every context of the extension sees
    // the event Chrome promises Bitwarden's state framework.
    function storageChangedPost(areaName, changes) {
        var any = false;
        for (var k in changes) {
            any = true;
            break;
        }
        if (!any)
            return;
        var args = [changes, areaName];
        var ev = shim.events['storage.onChanged'];
        if (ev)
            ev.__fire(args);
        ev = shim.events['storage.' + areaName + '.onChanged'];
        if (ev)
            ev.__fire(args);
        call('storage', 'changed',
             { area: areaName, changes: changes })
            .catch(function () {});
    }

    function bridgeStorageArea(areaName) {
        var areaEvent = makeEvent('storage.' + areaName + '.onChanged');
        function afterMutate(result) {
            var changes = result && result.changes || {};
            var any = false;
            for (var k in changes)
                any = true;
            if (!any)
                return;
            storageChangedPost(areaName, changes);
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

    // A KEPT real area still needs us for onChanged: wrap its
    // mutators in place so each write snapshots the touched keys,
    // performs the real mutation, then posts the change record for
    // the fan-out above; its dead native onChanged is replaced by
    // the shim event the fan-out fires.  The area object keeps its
    // identity — get/getKeys/getBytesInUse/setAccessLevel/
    // QUOTA_BYTES stay the real members.
    //
    // Native areas bind members as getter-only accessors — a plain
    // assignment throws a TypeError that would flood shimGaps even
    // though defineProperty succeeds.  defineMember therefore leads
    // with defineProperty and only records a gap when the member
    // really cannot be replaced.
    function defineMember(obj, key, val, api) {
        if (obj[key] === val)
            return;
        try {
            Object.defineProperty(obj, key, {
                configurable: true,
                writable: true,
                enumerable: true,
                value: val
            });
        } catch (e) {}
        if (obj[key] === val)
            return;
        safeSet(obj, key, val, api);
        if (obj[key] !== val)
            recordGap(api, 'native-member-locked');
    }
    var wrappedAreas = {};   // areaName -> {area, srcSet, wrappedSet…}
    function adoptRealArea(areaName) {
        var area;
        try {
            area = chrome.storage && chrome.storage[areaName];
        } catch (e) { return; }
        if (!area || typeof area !== 'object'
                || typeof area.get !== 'function'
                || typeof area.set !== 'function')
            return;
        var rec = wrappedAreas[areaName];
        if (!rec || rec.area !== area
                || (area.set !== rec.wrappedSet
                    && area.set !== rec.srcSet)) {
            // Fresh object or a mid-boot native redefinition — wrap
            // against whatever the members are now.
            var src = { get: area.get, set: area.set,
                        remove: area.remove, clear: area.clear };
            rec = { area: area, srcSet: src.set };
            function runOp(fn, argv) {
                return new Promise(function (resolve, reject) {
                    var done = false;
                    var args = argv.slice();
                    args.push(function () {
                        if (done)
                            return;
                        done = true;
                        var e;
                        try {
                            e = chrome.runtime
                                && chrome.runtime.lastError;
                        } catch (x) { e = null; }
                        if (e && (e.message || typeof e === 'string'))
                            reject(new Error(e.message || e));
                        else
                            resolve();
                    });
                    try {
                        var r = fn.apply(area, args);
                        if (r && typeof r.then === 'function')
                            r.then(function () {
                                if (!done) { done = true; resolve(); }
                            }, function (e) {
                                if (!done) { done = true; reject(e); }
                            });
                    } catch (e) {
                        if (!done) {
                            done = true;
                            reject(e);
                        }
                    }
                });
            }
            function readOld(keys) {
                return new Promise(function (resolve) {
                    var done = false;
                    var finish = function (v) {
                        if (!done) {
                            done = true;
                            resolve(v || {});
                        }
                    };
                    try {
                        var r = src.get.call(area, keys, finish);
                        if (r && typeof r.then === 'function')
                            r.then(finish, function () { finish({}); });
                    } catch (e) { finish({}); }
                });
            }
            var has = function (o, k) {
                return o && Object.prototype.hasOwnProperty
                        .call(o, k);
            };
            rec.wrappedSet = function (items, cb) {
                var p = readOld(items ? Object.keys(items) : [])
                    .then(function (old) {
                        return runOp(src.set, [items || {}])
                            .then(function () {
                                var changes = {};
                                for (var k in items) {
                                    var change = {};
                                    if (has(old, k))
                                        change.oldValue = old[k];
                                    change.newValue = items[k];
                                    changes[k] = change;
                                }
                                storageChangedPost(areaName,
                                                   changes);
                            });
                    });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                           });
                return p.then(function () {});
            };
            rec.wrappedRemove = function (keys, cb) {
                var list = typeof keys === 'string'
                    ? [keys] : (keys || []).slice();
                var p = readOld(list).then(function (old) {
                    return runOp(src.remove, [list])
                        .then(function () {
                            var changes = {};
                            for (var i = 0; i < list.length; ++i) {
                                var k = list[i];
                                if (has(old, k))
                                    changes[k] =
                                        { oldValue: old[k] };
                            }
                            storageChangedPost(areaName, changes);
                        });
                });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                           });
                return p.then(function () {});
            };
            rec.wrappedClear = function (cb) {
                var p = readOld(null).then(function (old) {
                    return runOp(src.clear, []).then(function () {
                        var changes = {};
                        for (var k in old)
                            changes[k] = { oldValue: old[k] };
                        storageChangedPost(areaName, changes);
                    });
                });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                           });
                return p.then(function () {});
            };
            wrappedAreas[areaName] = rec;
        }
        var areaEvent =
            shim.events['storage.' + areaName + '.onChanged']
            || makeEvent('storage.' + areaName + '.onChanged');
        defineMember(area, 'set', rec.wrappedSet,
                     'chrome.storage.' + areaName + '.set');
        defineMember(area, 'remove', rec.wrappedRemove,
                     'chrome.storage.' + areaName + '.remove');
        defineMember(area, 'clear', rec.wrappedClear,
                     'chrome.storage.' + areaName + '.clear');
        defineMember(area, 'onChanged', areaEvent,
                     'chrome.storage.' + areaName + '.onChanged');
    }

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
        } else {
            adoptRealArea('local');
        }
        if (!chrome.storage.session
                || typeof chrome.storage.session.get !== 'function') {
            bridgeSessionRef = bridgeStorageArea('session');
            safeSet(chrome.storage, 'session', bridgeSessionRef,
                    'chrome.storage.session');
        } else {
            adoptRealArea('session');
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
        var topChanged = null;
        try { topChanged = chrome.storage.onChanged; } catch (e) {}
        if (!topChanged) {
            safeSet(chrome.storage, 'onChanged',
                    makeEvent('storage.onChanged'),
                    'chrome.storage.onChanged');
        } else if (typeof topChanged.__fire !== 'function') {
            // Native object — dead on this engine, so nothing it
            // holds can ever fire; swap it for the event our
            // fan-out reaches.
            note('chrome.storage.onChanged: native event replaced '
                 + '— the engine never fires it');
            defineMember(chrome.storage, 'onChanged',
                         shim.events['storage.onChanged']
                         || makeEvent('storage.onChanged'),
                         'chrome.storage.onChanged');
        }
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

    // Binary payloads for the bridge — ImageData.data is a
    // Uint8ClampedArray, which marshals as a sparse object instead of
    // bytes; chunked fromCharCode + btoa keeps it compact and exact.
    function bytesToBase64(bytes) {
        var bin = '';
        for (var i = 0; i < bytes.length; i += 8192)
            bin += String.fromCharCode.apply(
                null, bytes.subarray(i, i + 8192));
        return btoa(bin);
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
                details = details || {};
                // ImageData can't cross the bridge — encode the raw
                // RGBA as base64 ({width,height,rgba}); a size->image
                // map carries the same shape per key.  path entries
                // pass through verbatim; the host applies the
                // bundle-path hygiene.
                var args = { tabId: details.tabId };
                var encImage = function (img) {
                    if (!img || !img.data || !img.width || !img.height)
                        return null;
                    return { width: img.width, height: img.height,
                             rgba: bytesToBase64(img.data) };
                };
                if (details.path) {
                    if (typeof details.path === 'string')
                        args.path = details.path;
                    else {
                        var pm = {};
                        for (var pk in details.path)
                            pm[pk] = String(details.path[pk]);
                        args.path = pm;
                    }
                }
                if (details.imageData) {
                    if (details.imageData.data)
                        args.imageData = encImage(details.imageData);
                    else {
                        var im = {};
                        for (var ik in details.imageData)
                            im[ik] = encImage(details.imageData[ik]);
                        args.imageData = im;
                    }
                }
                var p = call('action', 'setIcon', args);
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message); });
                return p.then(function () {});
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
        var tabsNs = { __arktosShim: true };
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
        // MD10 — the core op set.  Every member rides the real
        // dispatch; the callback conventions match the query/get
        // wrappers above (undefined + lastError on a typed failure).
        tabsNs.remove = function (tabIds, cb) {
            var ids = Array.isArray(tabIds) ? tabIds : [tabIds];
            var p = call('tabs', 'remove', { ids: ids });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.reload = function (tabId, reloadProperties, cb) {
            if (typeof tabId === 'function') {
                cb = tabId; tabId = null; reloadProperties = null;
            } else if (typeof reloadProperties === 'function') {
                cb = reloadProperties; reloadProperties = null;
            }
            var p = call('tabs', 'reload',
                         { id: tabId,
                           bypassCache: !!(reloadProperties
                               && reloadProperties.bypassCache) });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.duplicate = function (tabId, cb) {
            var p = call('tabs', 'duplicate', { id: tabId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.highlight = function (highlightInfo, cb) {
            var info = highlightInfo || {};
            var p = call('tabs', 'highlight',
                         { windowId: info.windowId,
                           indexes: info.tabs });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.move = function (tabIds, moveProperties, cb) {
            var info = moveProperties || {};
            var ids = Array.isArray(tabIds) ? tabIds : [tabIds];
            var p = call('tabs', 'move',
                         { ids: ids, index: info.index,
                           windowId: info.windowId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.goBack = function (tabId, cb) {
            if (typeof tabId === 'function') { cb = tabId; tabId = null; }
            var p = call('tabs', 'goBack', { id: tabId });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.goForward = function (tabId, cb) {
            if (typeof tabId === 'function') { cb = tabId; tabId = null; }
            var p = call('tabs', 'goForward', { id: tabId });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.discard = function (tabId, cb) {
            if (typeof tabId === 'function') { cb = tabId; tabId = null; }
            var p = call('tabs', 'discard', { id: tabId })
                .then(function (v) {
                    // Chrome hands back undefined for a tab it could
                    // not discard — the backend's null is that answer.
                    return (v === null) ? undefined : v;
                });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        // Deprecated Chrome spellings — aliases over the query
        // surface, kept for MV2-era callers.
        tabsNs.getAllInWindow = function (windowId, cb) {
            var p = call('tabs', 'query',
                         { query: { windowId:
                             (windowId === undefined
                              || windowId === null) ? -2 : windowId } });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, [], e.message); });
            return p;
        };
        tabsNs.getSelected = function (windowId, cb) {
            var p = call('tabs', 'query',
                         { query: { active: true,
                             windowId:
                             (windowId === undefined
                              || windowId === null) ? -2 : windowId } })
                .then(function (v) {
                    return (v && v.length) ? v[0] : undefined;
                });
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
            // ER02: the hosted route — the page agent is the only
            // live receiving end (the native binding can only reach
            // the dead service worker), so there is no native
            // attempt: the bridge resolves the tab, gates the
            // extension's host coverage BEFORE any push, emits
            // '__arktos.deliver', and the reply returns as
            // '__arktos.deliverResponse' -> onDeliverResponse.
            touch('chrome.tabs.sendMessage');
            var mid = cfg.id + '#' + (++shim.seq) + '#' + Date.now();
            return new Promise(function (resolve, reject) {
                var entry = { cb: cb, resolve: resolve,
                              reject: reject, done: false };
                entry.fail = function (message) {
                    if (entry.done)
                        return;
                    entry.done = true;
                    delete deliverWait[mid];
                    if (cb) {
                        // Callback form: lastError inside the call,
                        // and the (dual-convention) promise resolves
                        // rather than reject — Chrome hands back no
                        // promise at all in the callback shape, so an
                        // unhandled rejection must not appear.
                        answerCb(cb, undefined, message);
                        resolve(undefined);
                        return;
                    }
                    var e = new Error(message);
                    e.api = 'chrome.tabs.sendMessage';
                    reject(e);
                };
                deliverWait[mid] = entry;
                // Safety net: an accepted delivery whose answer never
                // lands (channel died mid-flight) still resolves.
                setTimeout(function () {
                    entry.fail('The message port closed before a '
                               + 'response was received.');
                }, 15000);
                call('tabs', 'sendMessage',
                     { id: tabId, mid: mid, msg: msg,
                       opts: opts || {} })
                    .then(function () {
                        // Delivery accepted — the answer arrives on
                        // the deliverResponse push.
                    })
                    .catch(function (e) {
                        entry.fail(e && e.message
                            ? e.message
                            : 'chrome.tabs.sendMessage failed');
                    });
            });
        };
        // MD12 — tabs.connect(tabId, connectInfo): a long-lived Port
        // to the tab's page agent over the same ER02 relay machinery
        // sendMessage uses.  The bridge call registers THIS context
        // as the local end on the chan and pushes the offer to the
        // agent; its runtime.onConnect answers 'portAccept' and from
        // then on postMessage/onMessage/onDisconnect ride the shared
        // 'relay' port ops.  The API cannot refuse synchronously —
        // a typed failure lands as an immediate onDisconnect, the
        // same surface Chrome's 'Could not establish connection'
        // takes.
        tabsNs.connect = function (tabId, connectInfo) {
            var info = connectInfo || {};
            touch('chrome.tabs.connect');
            var localId = 'p' + (++shim.seq) + '#' + Date.now();
            var chan = cfg.id + '#' + localId;
            var port = newPort(info.name || '', chan, localId);
            call('tabs', 'connect',
                 { id: tabId, chan: chan, portId: localId,
                   name: info.name || '', opts: info })
                .then(function (sender) {
                    // The reply carries the tab end's MessageSender —
                    // Chrome fills the connecting port's sender.tab.
                    if (sender && typeof sender === 'object')
                        port.sender = sender;
                })
                .catch(function (e) {
                    var msg = (e && e.message)
                        ? String(e.message)
                        : 'chrome.tabs.connect failed';
                    recordGap('chrome.tabs.connect', msg);
                    if (port.disconnected)
                        return;
                    port.disconnected = true;
                    delete shim.ports[localId];
                    setTimeout(function () {
                        // Chrome sets lastError for the duration of
                        // the onDisconnect listeners only.
                        setLastError(msg);
                        port.onDisconnect.__fire([port]);
                        clearLastError();
                    }, 0);
                });
            return port;
        };
        // MD11 — zoom + capture.  Zoom rides the tab's real zoom
        // machinery; zoomSettings are a recorded model the backend
        // merges over honest defaults (mode:'disabled' refuses later
        // setZoom calls); captureVisibleTab is gated by the backend's
        // seesUrls coverage check — an uncovered tab answers a typed
        // no-permission rejection, never a blank image.
        tabsNs.getZoom = function (tabId, cb) {
            if (typeof tabId === 'function') { cb = tabId; tabId = null; }
            var p = call('tabs', 'getZoom', { id: tabId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.setZoom = function (tabId, zoomFactor, cb) {
            // setZoom(tabId, factor) or setZoom(factor) — a lone
            // number is always the factor, never the tabId.
            if (typeof tabId === 'function') {
                cb = tabId; tabId = null; zoomFactor = undefined;
            }
            if (typeof zoomFactor === 'function') {
                cb = zoomFactor; zoomFactor = undefined;
            }
            if (zoomFactor === undefined || zoomFactor === null) {
                zoomFactor = tabId;
                tabId = null;
            }
            var p = call('tabs', 'setZoom',
                         { id: tabId, zoom: zoomFactor });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.getZoomSettings = function (tabId, cb) {
            if (typeof tabId === 'function') { cb = tabId; tabId = null; }
            var p = call('tabs', 'getZoomSettings', { id: tabId });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
        };
        tabsNs.setZoomSettings = function (tabId, zoomSettings, cb) {
            // setZoomSettings(tabId, settings) or
            // setZoomSettings(settings).
            if (typeof tabId === 'function') {
                cb = tabId; tabId = null; zoomSettings = undefined;
            }
            if (typeof zoomSettings === 'function') {
                cb = zoomSettings; zoomSettings = undefined;
            }
            if (zoomSettings === undefined || zoomSettings === null) {
                zoomSettings = tabId;
                tabId = null;
            }
            var p = call('tabs', 'setZoomSettings',
                         { id: tabId, settings: zoomSettings || {} });
            if (cb)
                p.then(function () { answerCb(cb); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p.then(function () {});
        };
        tabsNs.captureVisibleTab = function (windowId, options, cb) {
            // captureVisibleTab(windowId?, options?, cb) — a lone
            // object is the options bag, a lone number the windowId.
            if (typeof windowId === 'function') {
                cb = windowId; windowId = null; options = undefined;
            }
            if (typeof options === 'function') {
                cb = options; options = undefined;
            }
            if (options === undefined || options === null) {
                if (windowId !== null
                        && typeof windowId === 'object') {
                    options = windowId;
                    windowId = null;
                } else {
                    options = {};
                }
            }
            var p = call('tabs', 'captureVisibleTab',
                         { windowId: windowId, options: options });
            if (cb)
                p.then(function (v) { answerCb(cb, v); },
                       function (e) { answerCb(cb, undefined, e.message); });
            return p;
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
    // are the shim events EXT16 pushes.  MD13: the getInfo argument
    // (populate, windowTypes) crosses too — the host populates the
    // tabs array and filters honestly.
    if (typeof chrome !== 'undefined') {
        var winNs = { __arktosShim: true };
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
                // Chrome's signatures: get(windowId, getInfo),
                // getCurrent/getLastFocused/getAll(getInfo) — a
                // number is the id, an object is getInfo.
                for (var i = 0; i < a.length; ++i) {
                    if (typeof a[i] === 'number')
                        args.id = a[i];
                    else if (a[i] && typeof a[i] === 'object')
                        args.getInfo = a[i];
                }
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
        // ER05: a native chrome.windows binding keeps its inert
        // event objects — the schema ships them but no embedder
        // delegate dispatches them, so a listener registered on the
        // native object alone would never fire.  Dual-register each
        // hub-pushed event the same way the runtime lifecycle events
        // are handled above: listeners land on the shim event the
        // EXT16 fan-out fires, and on any native object as well.
        if (chrome.windows && typeof chrome.windows === 'object') {
            var winDual = ['onCreated', 'onRemoved',
                           'onFocusChanged'];
            for (var wd = 0; wd < winDual.length; ++wd) {
                (function (cname) {
                    var evName = 'windows.' + cname;
                    var shimEv = shim.events[evName]
                                 || makeEvent(evName);
                    var realEv;
                    try {
                        realEv = chrome.windows[cname];
                    } catch (e) {
                        realEv = null;
                    }
                    var installed = shimEv;
                    if (realEv && realEv !== shimEv
                            && typeof realEv.addListener
                                   === 'function') {
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
                    safeSet(chrome.windows, cname, installed,
                            'chrome.windows.' + cname);
                })(winDual[wd]);
            }
        }
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
            // Frame lookup: the bridge answers the registered main
            // frame — deeper frames come back as Chrome's own 'no
            // frame' typed error, not a fabricated entry.
            getFrame: function (details, cb) {
                var p = call('webNavigation', 'getFrame',
                             details || {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            getAllFrames: function (details, cb) {
                var p = call('webNavigation', 'getAllFrames',
                             details || {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            }
        });
    }

    // --- chrome.webRequest ----------------------------------------------------
    // ER11 — observe-only webRequest.  Each event keeps its
    // (listener, filter, extraInfoSpec) triples locally and registers
    // one record per addListener with the C++ watcher through
    // webRequest.subscribe; deliveries arrive as 'webRequest.<event>'
    // hostEvents already unioned per extension, so __fire re-checks
    // each listener's own filter.  'blocking'/'requestBody'
    // extraInfoSpec can never be honored — the watcher still
    // registers the listener observe-only (Chrome's MV3 answer) but
    // answers typed 'unsupported', which typedError records into
    // gaps -> health.  Events no observation source exists for
    // (onBeforeRedirect/onResponseStarted/onAuthRequired) answer the
    // same typed error at subscribe — never a silent no-fire.
    var wrSeq = 0;
    function wrUrlMatches(patterns, urlString) {
        // Chrome match-pattern check against a concrete URL — the
        // local half of the watcher's grammar (the C++ side already
        // applies it per extension; this re-checks per listener).
        if (!Array.isArray(patterns) || !patterns.length)
            return true;
        var u;
        try { u = new URL(urlString); } catch (e) { return false; }
        var scheme = u.protocol.replace(/:$/, '');
        for (var i = 0; i < patterns.length; ++i) {
            var p = patterns[i];
            if (p === '<all_urls>') {
                if (scheme === 'http' || scheme === 'https'
                        || scheme === 'file' || scheme === 'ftp')
                    return true;
                continue;
            }
            var sep = p.indexOf('://');
            if (sep < 0)
                continue;
            var ps = p.slice(0, sep);
            var rest = p.slice(sep + 3);
            var slash = rest.indexOf('/');
            var ph = slash < 0 ? rest : rest.slice(0, slash);
            var pp = slash < 0 ? '/*' : rest.slice(slash);
            // 'host:port' — Chrome lets patterns pin a port.
            var pport = '';
            var colon = ph.lastIndexOf(':');
            if (colon >= 0) {
                pport = ph.slice(colon + 1);
                ph = ph.slice(0, colon);
            }
            if (ps === '*') {
                if (scheme !== 'http' && scheme !== 'https')
                    continue;
            } else if (ps !== scheme) {
                continue;
            }
            if (pport && pport !== '*') {
                var defPort = scheme === 'https' ? 443
                    : scheme === 'http' ? 80 : -1;
                var uport = u.port ? parseInt(u.port, 10) : defPort;
                if (uport !== parseInt(pport, 10))
                    continue;
            }
            if (ph === '*') {
                // any host
            } else if (ph.indexOf('*.') === 0) {
                var base = ph.slice(2);
                if (u.hostname !== base
                        && !u.hostname.endsWith('.' + base))
                    continue;
            } else if (ph !== u.hostname) {
                continue;
            }
            var re;
            try {
                re = new RegExp('^'
                    + pp.replace(/[.+^${}()|[\]\\]/g, '\\$&')
                        .replace(/\*/g, '.*')
                        .replace(/\?/g, '.')
                    + '$');
            } catch (e) { continue; }
            if (re.test(u.pathname + (u.search || '')))
                return true;
        }
        return false;
    }
    function makeWebRequestEvent(name) {
        var eventName = name.slice('webRequest.'.length);
        var listeners = [];   // {fn, filter, spec, regId}
        var ev = {
            addListener: function (fn, filter, extraInfoSpec) {
                if (typeof fn !== 'function')
                    return;
                filter = filter || {};
                var spec = Array.isArray(extraInfoSpec)
                    ? extraInfoSpec : [];
                var entry = {
                    fn: fn, filter: filter, spec: spec,
                    regId: cfg.id + '#wr' + (++wrSeq) + '#'
                           + Date.now()
                };
                listeners.push(entry);
                call('webRequest', 'subscribe', {
                    regId: entry.regId,
                    event: eventName,
                    filter: {
                        urls: Array.isArray(filter.urls)
                            ? filter.urls : [],
                        types: Array.isArray(filter.types)
                            ? filter.types : null,
                        tabId: filter.tabId,
                        windowId: filter.windowId
                    },
                    extraInfoSpec: spec
                }).then(function (r) {
                    if (r && r.note)
                        note('chrome.' + name + ': ' + r.note);
                }).catch(function () {
                    // The typed failure (unsupported spec, unservable
                    // event, missing permission) already recorded
                    // its gap inside call().
                });
            },
            removeListener: function (fn) {
                for (var i = 0; i < listeners.length; ++i) {
                    if (listeners[i].fn === fn) {
                        var regId = listeners[i].regId;
                        listeners.splice(i, 1);
                        call('webRequest', 'unsubscribe',
                             { regId: regId })
                            .catch(function () {});
                        return;
                    }
                }
            },
            hasListener: function (fn) {
                for (var i = 0; i < listeners.length; ++i)
                    if (listeners[i].fn === fn)
                        return true;
                return false;
            },
            hasListeners: function () { return listeners.length > 0; }
        };
        ev.__fire = function (args) {
            var details = args && args[0] ? args[0] : {};
            for (var i = 0; i < listeners.length; ++i) {
                var l = listeners[i];
                var f = l.filter;
                if (f.tabId !== undefined
                        && f.tabId !== details.tabId)
                    continue;
                if (f.windowId !== undefined
                        && f.windowId !== details.windowId)
                    continue;
                if (Array.isArray(f.types) && f.types.length
                        && f.types.indexOf(details.type) < 0)
                    continue;
                if (!wrUrlMatches(f.urls, details.url))
                    continue;
                try {
                    l.fn(details);
                } catch (e) {
                    record('shim event ' + name + ' listener: ' + e);
                }
            }
        };
        shim.events[name] = ev;
        return ev;
    }
    if (typeof chrome !== 'undefined') {
        installNs('webRequest', {
            __arktosShim: true,
            onBeforeRequest:
                makeWebRequestEvent('webRequest.onBeforeRequest'),
            onBeforeSendHeaders:
                makeWebRequestEvent('webRequest.onBeforeSendHeaders'),
            onSendHeaders:
                makeWebRequestEvent('webRequest.onSendHeaders'),
            onHeadersReceived:
                makeWebRequestEvent('webRequest.onHeadersReceived'),
            onAuthRequired:
                makeWebRequestEvent('webRequest.onAuthRequired'),
            onResponseStarted:
                makeWebRequestEvent('webRequest.onResponseStarted'),
            onBeforeRedirect:
                makeWebRequestEvent('webRequest.onBeforeRedirect'),
            onCompleted:
                makeWebRequestEvent('webRequest.onCompleted'),
            onErrorOccurred:
                makeWebRequestEvent('webRequest.onErrorOccurred'),
            // Chrome's call flushes the in-memory blocking-filter
            // cache — observe-only has nothing to flush, so a plain
            // resolve is the honest answer (never a stub).
            handlerBehaviorChanged: function (cb) {
                touch('chrome.webRequest.handlerBehaviorChanged');
                return okCall(undefined, cb);
            },
            MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20
        });
    }

    // --- chrome.scripting -----------------------------------------------------
    // ER07 — the product API.  executeScript is bridge-backed: the
    // host resolves the tab, re-proves 'scripting' + host coverage
    // BEFORE any source reaches the page, then evals the payload per
    // frame (ISOLATED -> the shared agent world with this extension's
    // scoped chrome binding, MAIN -> the page's own world).  The func
    // a caller hands us crosses the channel as serialized source —
    // functions cannot marshal — exactly like Chrome's own
    // serialize-then-inject.  The results ride back on
    // '__arktos.execResult'.  ER08 — insertCSS/removeCSS ride the
    // same execId machinery (a per-frame css payload, the same
    // '__arktos.execResult' answer) but resolve void like Chrome.
    // ER09 — registerContentScripts/unregisterContentScripts/
    // getRegisteredContentScripts/updateContentScripts are
    // synchronous registry ops: the C++ side normalizes the spec,
    // re-proves every matches[] pattern against the extension's host
    // coverage BEFORE arming it, then joins the spec to the
    // page-agent injection set (profile script in the shared agent
    // world, same gate shape as a declared content script).
    function cssOp(method, injection, cb) {
        touch('chrome.scripting.' + method);
        injection = injection || {};
        var target = injection.target || {};
        var execId = cfg.id + '#css' + (++shim.seq) + '#'
                     + Date.now();
        return new Promise(function (resolve, reject) {
            var entry = { cb: cb, resolve: resolve,
                          reject: reject, done: false, css: true };
            entry.fail = function (message) {
                if (entry.done)
                    return;
                entry.done = true;
                delete execWait[execId];
                if (cb) {
                    answerCb(cb, undefined, message);
                    resolve(undefined);
                    return;
                }
                var e = new Error(message);
                e.api = 'chrome.scripting.' + method;
                reject(e);
            };
            execWait[execId] = entry;
            // The same safety net executeScript carries — a delivery
            // whose answer never lands resolves typed rather than
            // hanging forever.
            setTimeout(function () {
                entry.fail('chrome.scripting.' + method
                           + ': exec-timeout');
            }, 15000);
            call('scripting', method, {
                execId: execId,
                target: {
                    tabId: target.tabId,
                    frameIds: target.frameIds,
                    allFrames: target.allFrames,
                    documentIds: target.documentIds
                },
                css: injection.css,
                files: Array.isArray(injection.files)
                    ? injection.files : null,
                origin: injection.origin || 'AUTHOR'
            }).then(function () {
                // Accepted — the result lands on
                // '__arktos.execResult'.
            }).catch(function (e) {
                entry.fail(e && e.message
                    ? e.message
                    : 'chrome.scripting.' + method + ' failed');
            });
        });
    }
    if (typeof chrome !== 'undefined') {
        installNs('scripting', {
            __arktosShim: true,
            ExecutionWorld: { ISOLATED: 'ISOLATED', MAIN: 'MAIN' },
            executeScript: function (injection, cb) {
                touch('chrome.scripting.executeScript');
                injection = injection || {};
                var target = injection.target || {};
                var execId = cfg.id + '#exec' + (++shim.seq) + '#'
                             + Date.now();
                return new Promise(function (resolve, reject) {
                    var entry = { cb: cb, resolve: resolve,
                                  reject: reject, done: false };
                    // The call-level failure path — a typed error
                    // from invoke (bad target, no host coverage) or
                    // the safety net below.  Callback form answers
                    // lastError + undefined like Chrome does.
                    entry.fail = function (message) {
                        if (entry.done)
                            return;
                        entry.done = true;
                        delete execWait[execId];
                        if (cb) {
                            answerCb(cb, undefined, message);
                            resolve(undefined);
                            return;
                        }
                        var e = new Error(message);
                        e.api = 'chrome.scripting.executeScript';
                        reject(e);
                    };
                    execWait[execId] = entry;
                    // A delivery whose answer never lands (frame
                    // gone, channel dead) resolves typed rather
                    // than hanging forever — same net sendMessage
                    // carries.
                    setTimeout(function () {
                        entry.fail('chrome.scripting.executeScript: '
                                   + 'exec-timeout');
                    }, 15000);
                    var funcSource = null;
                    if (injection.func !== undefined
                            && injection.func !== null)
                        funcSource = String(injection.func);
                    call('scripting', 'executeScript', {
                        execId: execId,
                        target: {
                            tabId: target.tabId,
                            frameIds: target.frameIds,
                            allFrames: target.allFrames,
                            documentIds: target.documentIds
                        },
                        func: funcSource,
                        args: Array.isArray(injection.args)
                            ? injection.args : [],
                        files: Array.isArray(injection.files)
                            ? injection.files : null,
                        world: injection.world || 'ISOLATED',
                        injectImmediately:
                            !!injection.injectImmediately
                    }).then(function () {
                        // Accepted — the results land on
                        // '__arktos.execResult'.
                    }).catch(function (e) {
                        entry.fail(e && e.message
                            ? e.message
                            : 'chrome.scripting.executeScript '
                              + 'failed');
                    });
                });
            },
            insertCSS: function (injection, cb) {
                return cssOp('insertCSS', injection, cb);
            },
            removeCSS: function (injection, cb) {
                return cssOp('removeCSS', injection, cb);
            },
            registerContentScripts: function (scripts, cb) {
                touch('chrome.scripting.registerContentScripts');
                var p = call('scripting', 'registerContentScripts', {
                    scripts: Array.isArray(scripts) ? scripts : null
                }).then(function (v) {
                    if (v && v.notes)
                        v.notes.forEach(function (n) { note(n); });
                });
                if (cb)
                    p.then(function () { answerCb(cb); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p.then(function () {});
            },
            unregisterContentScripts: function (filter, cb) {
                touch('chrome.scripting.unregisterContentScripts');
                var p = call('scripting', 'unregisterContentScripts', {
                    filter: { ids: filter && filter.ids || null }
                }).then(function () {});
                if (cb)
                    p.then(function () { answerCb(cb); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            getRegisteredContentScripts: function (filter, cb) {
                touch('chrome.scripting.getRegisteredContentScripts');
                var p = call('scripting',
                             'getRegisteredContentScripts', {
                    filter: { ids: filter && filter.ids || null }
                });
                if (cb)
                    p.then(function (v) { answerCb(cb, v || []); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            updateContentScripts: function (scripts, cb) {
                touch('chrome.scripting.updateContentScripts');
                var p = call('scripting', 'updateContentScripts', {
                    scripts: Array.isArray(scripts) ? scripts : null
                }).then(function (v) {
                    if (v && v.notes)
                        v.notes.forEach(function (n) { note(n); });
                });
                if (cb)
                    p.then(function () { answerCb(cb); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p.then(function () {});
            }
        });
    }

    // --- chrome.contentScripts ------------------------------------------------
    // ER09 — the Firefox-shape dynamic registration surface.  The
    // bundled webextension-polyfill Bitwarden ships resolves
    // browser.contentScripts.* to chrome.contentScripts.* in a
    // Chromium-shaped context, so this namespace must exist even
    // though Chromium itself never offers it.  register() resolves a
    // RegisteredContentScript-style handle — a plain object carrying
    // unregister(), since the registry lives host-side.  No API
    // permission exists for this surface; the per-match host-coverage
    // check host-side is the whole gate (the MV2 model).
    if (typeof chrome !== 'undefined') {
        var csHandle = function (id) {
            return {
                id: id,
                unregister: function (cb) {
                    var p = call('contentScripts', 'unregister',
                                 { id: id }).then(function () {});
                    if (cb)
                        p.then(function () { answerCb(cb); },
                               function (e) {
                                   answerCb(cb, undefined, e.message);
                               });
                    return p;
                }
            };
        };
        installNs('contentScripts', {
            __arktosShim: true,
            register: function (contentScriptOptions, cb) {
                touch('chrome.contentScripts.register');
                var p = call('contentScripts', 'register', {
                    options: contentScriptOptions || {}
                }).then(function (v) {
                    if (v && v.notes)
                        v.notes.forEach(function (n) { note(n); });
                    return csHandle(v && v.id);
                });
                if (cb)
                    p.then(function (h) { answerCb(cb, h); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            unregister: function (handle, cb) {
                touch('chrome.contentScripts.unregister');
                var id = (typeof handle === 'string') ? handle
                    : (handle && handle.id);
                var p = call('contentScripts', 'unregister',
                             { id: id }).then(function () {});
                if (cb)
                    p.then(function () { answerCb(cb); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            getScripts: function (cb) {
                touch('chrome.contentScripts.getScripts');
                var p = call('contentScripts', 'getScripts', {})
                    .then(function (list) {
                        return (list || []).map(function (s) {
                            var h = csHandle(s && s.id);
                            for (var k in s)
                                if (k !== 'id')
                                    h[k] = s[k];
                            return h;
                        });
                    });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            }
        });
    }

    // --- chrome.offscreen -------------------------------------------------
    // ER10 — one second-class host page per extension (Chrome's
    // single-document rule): the host spawns a hidden navigation-
    // locked page at the declared bundle-relative url, carrying this
    // same shim, its own chromeBridge and a relay registration of
    // kind 'offscreen' — chrome.* and runtime messaging work inside
    // it exactly like the popup's do.  The engine ships a NATIVE
    // binding here (ER06 measured hasDocument answering false), but a
    // natively-spawned document never gets our channel — a dead
    // context for the messaging/localStorage/clipboard use the API
    // exists for — so the members are identity overrides like
    // runtime.connect: every call routes through the bridge.
    // createDocument resolves when the spawned document's shim
    // registers its relay context (the messaging-ready signal) and
    // fails typed on a load/boot error.  Reasons and justification
    // are recorded host-side (offscreenInfo) — 'getContexts' reports
    // the context OFFSCREEN_DOCUMENT.
    function offscreenError(api, code) {
        var message = (code === 'single-document')
            ? 'Only a single offscreen document may be created.'
            : api + ': ' + code;
        recordGap(api, code);
        var e = new Error(message);
        e.code = code;
        e.api = api;
        return e;
    }
    if (typeof chrome !== 'undefined') {
        var offNs = {
            __arktosShim: true,
            Reason: {
                TESTING: 'TESTING',
                AUDIO_PLAYBACK: 'AUDIO_PLAYBACK',
                IFRAME_SCRIPTING: 'IFRAME_SCRIPTING',
                DOM_SCRAPING: 'DOM_SCRAPING',
                BLOBS: 'BLOBS',
                DOM_PARSER: 'DOM_PARSER',
                USER_MEDIA: 'USER_MEDIA',
                DISPLAY_MEDIA: 'DISPLAY_MEDIA',
                WEB_RTC: 'WEB_RTC',
                CLIPBOARD: 'CLIPBOARD',
                LOCAL_STORAGE: 'LOCAL_STORAGE',
                WORKERS: 'WORKERS',
                BATTERY_STATUS: 'BATTERY_STATUS',
                MATCH_MEDIA: 'MATCH_MEDIA',
                GEOLOCATION: 'GEOLOCATION',
                COLLECTION: 'COLLECTION'
            },
            hasDocument: function (cb) {
                touch('chrome.offscreen.hasDocument');
                var p = call('offscreen', 'hasDocument', {});
                var q = p.then(function (v) { return !!v; });
                if (cb)
                    q.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return q;
            },
            closeDocument: function (cb) {
                touch('chrome.offscreen.closeDocument');
                var p = call('offscreen', 'closeDocument', {})
                    .then(function () {});
                if (cb)
                    p.then(function () { answerCb(cb); },
                           function (e) {
                               answerCb(cb, undefined, e.message);
                           });
                return p;
            },
            createDocument: function (parameters, cb) {
                touch('chrome.offscreen.createDocument');
                parameters = parameters || {};
                var docId = cfg.id + '#doc' + (++shim.seq) + '#'
                            + Date.now();
                return new Promise(function (resolve, reject) {
                    var entry = { cb: cb, resolve: resolve,
                                  reject: reject, done: false };
                    entry.fail = function (code) {
                        if (entry.done)
                            return;
                        entry.done = true;
                        delete docWait[docId];
                        var e = offscreenError(
                            'chrome.offscreen.createDocument', code);
                        answerCb(cb, undefined, e.message);
                        reject(e);
                    };
                    docWait[docId] = entry;
                    // Same safety net the other deferred answers
                    // carry — a delivery that never lands resolves
                    // typed rather than hanging the caller.
                    setTimeout(function () {
                        entry.fail('boot-timeout');
                    }, 30000);
                    call('offscreen', 'createDocument', {
                        docId: docId,
                        url: parameters.url,
                        reasons: parameters.reasons || null,
                        justification:
                            parameters.justification || null
                    }).then(function () {
                        // accepted — the answer lands on
                        // '__arktos.offscreenDone'.
                    }).catch(function (e) {
                        entry.fail((e && e.code)
                            ? String(e.code)
                            : 'offscreen-failed');
                    });
                });
            }
        };
        offscreenNsRef = offNs;
        try {
            if (chrome.offscreen
                    && typeof chrome.offscreen.createDocument
                           === 'function'
                    && chrome.offscreen.createDocument
                           !== offNs.createDocument)
                note('chrome.offscreen: native binding overridden — '
                     + 'an engine-spawned document would carry no '
                     + 'chromeBridge or relay context');
        } catch (e) {}
        installNs('offscreen', offNs);
        // installNs's member-fill keeps existing members — the
        // native trio exists, so force the identity overrides.
        if (chrome.offscreen && typeof chrome.offscreen === 'object') {
            for (var om in offNs) {
                if (chrome.offscreen[om] !== offNs[om])
                    safeSet(chrome.offscreen, om, offNs[om],
                            'chrome.offscreen.' + om);
            }
        }
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
                var q = p.then(function (v) {
                    if (v && v.note)
                        note('chrome.notifications.create: ' + v.note);
                    return (v && v.id) || '';
                });
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

    // --- chrome.idle ------------------------------------------------------------
    // ER13 — served by ExtensionIdleWatcher on the host: an
    // application-wide Qt input filter feeding a detection-interval
    // timer; transitions fan back in as idle.onStateChanged.  The
    // honest bounds ride a note, once per context: in-app input only
    // (input in other apps never resets the clock) and 'locked' is
    // never reported.  Chrome's 15s interval floor applies host-side.
    if (typeof chrome !== 'undefined') {
        var idleNoted = false;
        installNs('idle', {
            onStateChanged: makeEvent('idle.onStateChanged', {
                onAdd: function () {
                    // Registering a listener arms the watcher —
                    // subscribe goes through the same 'idle'
                    // permission gate the methods do.
                    call('idle', 'subscribe', {})
                        .catch(function () {});
                    if (!idleNoted) {
                        idleNoted = true;
                        note('chrome.idle: input is measured inside '
                             + 'Arktos windows only; OS-level idle '
                             + 'and the locked state are never '
                             + 'reported');
                    }
                }
            }),
            queryState: function (interval, cb) {
                var p = call('idle', 'queryState',
                             { interval: interval });
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message); });
                return p;
            },
            setDetectionInterval: function (interval, cb) {
                var p = call('idle', 'setDetectionInterval',
                             { interval: interval });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined, e.message); });
                return p.then(function () {});
            },
            // ChromeOS-only in Chrome — typed unsupported everywhere
            // else, never a fake number.
            getAutoLockDelay: function (cb) {
                return failCall('idle.getAutoLockDelay',
                                'unsupported', cb);
            }
        });
    }

    // --- chrome.commands ---------------------------------------------------------
    if (typeof chrome !== 'undefined') {
        installNs('commands', {
            onCommand: makeEvent('commands.onCommand'),
            getAll: function (cb) {
                touch('chrome.commands.getAll');
                // ER14: the C++ registry knows which suggested_key
                // actually bound — an absent/unbindable suggestion
                // reports '' like Chrome's unassigned rows, not the
                // manifest text.
                var p = call('commands', 'getAll', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined, e.message); });
                return p;
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
                     + 'host-side; Arktos has no side-panel surface');
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

    // --- chrome.management -------------------------------------------------
    // ER15: getSelf is the one member Chrome serves without the
    // 'management' permission — the backend answers the real
    // id/name/version/enabled from the manifest + manager records.
    // The other members ride the bridge so the backend can answer
    // Chrome's own failure vocabulary: no-permission when the
    // manifest never declared 'management', unsupported when it did
    // and we still cannot serve it.  fillNs() below stubs anything
    // this list missed with the same typed shape.
    if (typeof chrome !== 'undefined') {
        var mgmtNs = {
            getSelf: function (cb) {
                var p = call('management', 'getSelf', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                           });
                return p;
            }
        };
        var mgmtRest = ['get', 'getAll', 'setEnabled', 'uninstall',
            'uninstallSelf', 'launchApp', 'generateAppForLink',
            'getPermissionWarningsById',
            'getPermissionWarningsByManifest'];
        for (var mm = 0; mm < mgmtRest.length; ++mm) {
            (function (m) {
                mgmtNs[m] = function () {
                    var a = arguments;
                    var cb = (a.length && typeof a[a.length - 1]
                              === 'function')
                        ? a[a.length - 1] : null;
                    var p = call('management', m, {});
                    if (cb)
                        p.then(function (v) { answerCb(cb, v); },
                               function (e) {
                                   answerCb(cb, undefined,
                                            e && e.message
                                            || String(e));
                               });
                    return p;
                };
            })(mgmtRest[mm]);
        }
        installNs('management', mgmtNs);
        mgmtNsRef = mgmtNs;
        // Identity assert now, not only in repairInstalls: Qt
        // injects a native chrome.management whose getSelf answers
        // Chromium's records (no manifestVersion), and installNs
        // only fills missing members.
        assertManagement();
    }

    // --- chrome.sidebarAction ---------------------------------------------
    // ER15: the Firefox-shape sugar, installed only when the manifest
    // declares sidebar_action — an extension that never declared a
    // sidebar must keep seeing `chrome.sidebarAction` undefined so
    // its feature-detect stays honest (Bitwarden checks exactly
    // this).  Panel/title/icon are recorded host-side like
    // sidePanel's options; open/close/toggle answer typed
    // 'unsupported' — there is no sidebar surface to open.
    // The gate reads the handshake manifest, so the install DEFERS
    // to onBridgeUp — at DocumentCreation bridgeReply has not landed
    // and manifest() is still {}.
    function installSidebarAction() {
        if (typeof chrome === 'undefined'
                || !manifest().sidebar_action)
            return;
        var haveSidebar = null;
        try { haveSidebar = chrome.sidebarAction; } catch (e) {}
        if (haveSidebar)
            return;
        installNs('sidebarAction', {
            setPanel: function (details, cb) {
                var p = call('sidebarAction', 'setPanel',
                             { panel: details && details.panel });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                               });
                return p;
            },
            getPanel: function (details, cb) {
                var p = call('sidebarAction', 'getPanel', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                               });
                return p;
            },
            setTitle: function (details, cb) {
                var p = call('sidebarAction', 'setTitle',
                             { title: details && details.title });
                if (cb)
                    p.then(function () { cb(); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                               });
                return p;
            },
            getTitle: function (details, cb) {
                var p = call('sidebarAction', 'getTitle', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function (e) {
                               answerCb(cb, undefined,
                                        e && e.message || String(e));
                               });
                return p;
            },
            setIcon: function (details, cb) {
                var p = call('sidebarAction', 'setIcon',
                             { icon: details && (details.path
                                                 || details.imageData) });
                if (cb)
                    p.then(function () { cb(); }, function () { cb(); });
                return p;
            },
            isOpen: function (details, cb) {
                var p = call('sidebarAction', 'isOpen', {});
                if (cb)
                    p.then(function (v) { answerCb(cb, v); },
                           function () { cb(false); });
                return p;
            },
            open: function (cb) {
                return sidePanelOpen('open', cb);
            },
            close: function (cb) {
                return sidePanelOpen('close', cb);
            },
            toggle: function (cb) {
                return sidePanelOpen('toggle', cb);
            },
            onUpdated: makeEvent('sidebarAction.onUpdated')
        });
        function sidePanelOpen(op, cb) {
            var p = call('sidebarAction', op, {});
            if (cb)
                p.then(function () { cb(); },
                       function (e) {
                           answerCb(cb, undefined,
                                    e && e.message || String(e));
                       });
            return p;
        }
    }
    installSidebarAction();

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
        // lack its own onChanged event object — or a redefined
        // native area can have silently unwound the ER17 wraps.
        if (chrome.storage && typeof chrome.storage === 'object') {
            adoptRealArea('local');
            adoptRealArea('session');
            adoptRealArea('sync');
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
            // ER15: getViews/getBackgroundPage are identity
            // overrides like runtime.connect — a native binding can
            // only enumerate Chromium-registered extension views,
            // which the hosted background page is not.  Ours compose
            // from the runtime's context registry (own Window +
            // sibling handles) and answer the honest MV3 null.
            forceMember(ext, 'getViews', extensionGetViews,
                        'chrome.extension.getViews');
            forceMember(ext, 'getBackgroundPage',
                        extensionGetBackgroundPage,
                        'chrome.extension.getBackgroundPage');
            fillNs('extension', ['onRequest', 'onRequestExternal']);
            // MD15: the isAllowed* verdicts are identity overrides
            // like getViews — a native member answers from
            // Chromium's extension registry, which the hosted
            // context is not part of; the bridge call reaches
            // invokeExtension's authoritative verdict instead.
            forceMember(ext, 'isAllowedIncognitoAccess',
                        extAllowedIncognitoRef,
                        'chrome.extension.isAllowedIncognitoAccess');
            forceMember(ext, 'isAllowedFileSchemeAccess',
                        extAllowedFileSchemeRef,
                        'chrome.extension.isAllowedFileSchemeAccess');
            if (ext.inIncognitoContext === undefined) {
                try { ext.inIncognitoContext = false; }
                catch (e) {}
            }
        }
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
        'fontSettings', 'tabGroups', 'userScripts',
        'declarativeNetRequest', 'declarativeContent',
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
                         + ' has no Arktos surface');
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
        'chrome.scripting.registerContentScripts',
        'chrome.scripting.unregisterContentScripts',
        'chrome.contentScripts.register',
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
