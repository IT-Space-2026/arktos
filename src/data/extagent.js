/*
 * ER01 page agent — embedded inside the per-extension injection that
 * ExtensionBackgroundHost arms at DocumentCreation in the extension's
 * own script world on every document of the hosted profile.
 * __ARKTOS_EXT_CONFIG is set by the generated prelude:
 * {id, name, manifest, i18n, hostPerms, grants}.
 *
 * This is the content-script half of the hosted runtime: a minimal
 * chrome.runtime surface for the extension's declared content scripts
 * (and later ER07's scripting.executeScript payloads) whose real
 * bindings would only reach the dead service worker:
 *
 *   chrome.runtime.id / getURL / getManifest / lastError
 *   chrome.runtime.sendMessage — marshaled over the page's own
 *       QWebChannel (SEC24 postMessage forwarder -> 'arktosExt'
 *       object -> ExtensionPageBridge) into the hosted background,
 *       replies routed back through its extEvent signal
 *   chrome.runtime.onMessage / onConnect — event objects ER02's
 *       host->page deliveries fire
 *   chrome.runtime.connect — long-lived port over the same
 *       transport (content-message-handler.js opens one purely to
 *       observe disconnects)
 *   chrome.extension.getURL, chrome.i18n.getMessage — the two
 *       aliases content scripts commonly touch
 *   chrome.dom.openOrClosedShadowRoot — BW01: the content-script
 *       namespace Chromium binds natively; answers the element's
 *       reachable shadow root (open roots only — closed roots are
 *       unreachable from any JS realm)
 *
 * Per-host scoping: the agent installs chrome.* only when the
 * extension's host_permissions or declared content-script matches
 * cover this frame's url; the C++ bridge re-verifies coverage on
 * every call (the postMessage forwarder is reachable by main-world
 * script, so the check, not the transport, is the boundary).
 * Originless documents (data:, about:) are never covered — Chrome
 * semantics, and WebPage's page-level channel bootstrap does not
 * inject there, so no bridge could answer anyway.
 *
 * Keep this file dependency-free and ES5-plus-Promise: it is inlined
 * into a QWebEngineScript source.
 */
(function () {
    'use strict';
    var cfg = window.__ARKTOS_EXT_CONFIG;
    if (!cfg || !cfg.id)
        return;
    // One shared agent world hosts EVERY extension's agent (Qt caps
    // script worlds at 0..256 — per-extension worlds do not scale),
    // so the registry is keyed by extension id; each extension's
    // content scripts are wrapped in a closure receiving THEIR OWN
    // `chrome` binding (never the global) from this table.
    var agents = window.__arktosExtAgents =
        window.__arktosExtAgents || {};
    if (agents[cfg.id])
        return;

    var agent = agents[cfg.id] = {
        id: cfg.id,
        name: cfg.name || '',
        version: 1,
        active: false,      // covered page — chrome installed
        bridgeUp: false,    // transport subscribed + hello answered
        gaps: [],           // typed errors (health surface, like the shim)
        notes: [],
        touched: {},
        errors: [],
        deliveries: 0,      // __arktos.deliver pushes addressed here
        deliverLog: []      // bounded: msg.command of each delivery
    };
    var LOG_CAP = 500;

    function note(text) {
        if (agent.notes.length < LOG_CAP)
            agent.notes.push(String(text));
    }
    function recordGap(api, error) {
        var text = api + ': ' + error;
        for (var i = 0; i < agent.gaps.length; ++i)
            if (agent.gaps[i] === text)
                return;
        if (agent.gaps.length < LOG_CAP)
            agent.gaps.push(text);
    }
    function touch(api) {
        agent.touched[api] = (agent.touched[api] || 0) + 1;
    }
    function record(what) {
        var text = String(what && what.message || what);
        var stack = what && what.stack;
        if (stack)
            text += ' @' + String(stack).split('\n')
                    .slice(1, 4).join(' <-');
        if (agent.errors.length < 200)
            agent.errors.push(text);
    }
    agent.probe = function () {
        return {
            active: agent.active,
            bridgeUp: agent.bridgeUp,
            gaps: agent.gaps,
            notes: agent.notes,
            touched: agent.touched,
            deliveries: agent.deliveries,
            deliverLog: agent.deliverLog.slice(-20),
            errors: agent.errors.slice(-20)
        };
    };

    // --- match-pattern gate --------------------------------------------
    // Chrome match semantics: scheme://host/path; '*' scheme covers
    // http+https, '<all_urls>' http/https/file/ftp, host '*' any
    // host, '*.x' x plus subdomains, path is a glob.
    function globRe(glob) {
        var re = '';
        for (var i = 0; i < glob.length; ++i) {
            var c = glob.charAt(i);
            if (c === '*')
                re += '.*';
            else if (c === '?')
                re += '.';
            else
                re += c.replace(/[.+^${}()|[\]\\]/g, '\\$&');
        }
        return new RegExp('^' + re + '$');
    }
    function matchPattern(pattern, url) {
        try {
            var u = new URL(url);
            var scheme = u.protocol.replace(':', '');
            if (pattern === '<all_urls>') {
                return scheme === 'http' || scheme === 'https'
                    || scheme === 'file' || scheme === 'ftp';
            }
            var sep = pattern.indexOf('://');
            if (sep < 0)
                return false;
            var ps = pattern.substring(0, sep);
            var rest = pattern.substring(sep + 3);
            var slash = rest.indexOf('/');
            var phost = slash < 0 ? rest : rest.substring(0, slash);
            var ppath = slash < 0 ? '/*' : rest.substring(slash);
            if (ps === '*') {
                if (scheme !== 'http' && scheme !== 'https')
                    return false;
            } else if (ps !== scheme) {
                return false;
            }
            var host = u.hostname;
            if (phost !== '*') {
                if (phost.indexOf('*.') === 0) {
                    var base = phost.substring(2);
                    if (host !== base
                            && !(host.length > base.length
                                 && host.lastIndexOf('.' + base)
                                    === host.length - base.length - 1))
                        return false;
                } else if (phost !== host) {
                    return false;
                }
            }
            var path = u.pathname + u.search;
            return globRe(ppath).test(path);
        } catch (e) {
            return false;
        }
    }
    function matchAny(patterns, url) {
        for (var i = 0; i < patterns.length; ++i) {
            if (matchPattern(patterns[i], url))
                return true;
        }
        return false;
    }
    agent.matchPattern = matchPattern;
    agent.matchAny = matchAny;

    var url = String(location.href);
    var grants = (cfg.hostPerms || []).concat(cfg.grants || []);
    if (!matchAny(grants, url)) {
        note('page not covered by extension grants — no chrome.* '
             + 'installed: ' + url.slice(0, 200));
        return;             // agent.active stays false
    }
    agent.active = true;

    // --- transport ------------------------------------------------------
    // window.postMessage crosses script worlds on the shared
    // DOMWindow: the SEC24 channel-world forwarder (arktos-channel.js)
    // whitelists 'arktosExt'.invoke/extEvent for us.  Calls ride
    // {arktosBridge:'call',kind:'method'} envelopes, replies come back
    // as 'result'; host->page pushes arrive as 'signal' reposts of the
    // bridge's extEvent.  A page with no channel (a bare
    // QWebEnginePage, a qrc doc) simply never subscribes — calls
    // queue, then reject typed at their watchdog.
    var seq = 0;
    var pendings = {};          // request id -> {resolve,reject,timer}
    var sendWait = {};          // message mid -> {finish}
    var callQueue = [];         // rpcs issued before the handshake
    var transportUp = false;
    agent.ports = {};           // local port id -> Port

    function rid() {
        return 'ea' + (++seq) + '.' + Date.now().toString(36)
                + Math.random().toString(36).slice(2, 8);
    }
    function post(message) {
        try { window.postMessage(message, '*'); }
        catch (e) { record('postMessage: ' + e); }
    }
    function timedOut(id) {
        var entry = pendings[id];
        if (!entry)
            return;
        delete pendings[id];
        entry.reject('bridge-timeout');
    }
    function issueRpc(op, args) {
        return new Promise(function (resolve, reject) {
            var id = rid();
            var call = args || {};
            call.op = op;
            call.ext = cfg.id;
            pendings[id] = { resolve: resolve, reject: reject };
            setTimeout(function () { timedOut(id); }, 30000);
            // The forwarder spreads positional args onto the channel
            // method — invoke(op, args) — and appends its result
            // callback.
            post({ arktosBridge: 'call', kind: 'method',
                   object: 'arktosExt', member: 'invoke',
                   args: [op, call], id: id });
        });
    }
    function rpc(op, args) {
        touch('arktosExt.' + op);
        if (transportUp)
            return issueRpc(op, args);
        return new Promise(function (resolve, reject) {
            var entry = { op: op, args: args,
                          resolve: resolve, reject: reject };
            callQueue.push(entry);
            setTimeout(function () {
                var i = callQueue.indexOf(entry);
                if (i >= 0)
                    callQueue.splice(i, 1);
                reject(new Error('bridge-timeout'));
            }, 30000);
        });
    }
    function flushQueue() {
        var q = callQueue;
        callQueue = [];
        for (var i = 0; i < q.length; ++i) {
            (function (c) {
                issueRpc(c.op, c.args).then(c.resolve, c.reject);
            })(q[i]);
        }
    }

    // --- the page-facing surfaces ---------------------------------------
    function makeEvent() {
        var fns = [];
        var ev = {
            addListener: function (fn) {
                if (typeof fn === 'function' && fns.indexOf(fn) < 0)
                    fns.push(fn);
            },
            removeListener: function (fn) {
                var i = fns.indexOf(fn);
                if (i >= 0)
                    fns.splice(i, 1);
            },
            hasListener: function (fn) {
                return fns.indexOf(fn) >= 0;
            },
            hasListeners: function () {
                return fns.length > 0;
            },
            __fns: function () { return fns.slice(); },
            __count: function () { return fns.length; },
            __fire: function (args) {
                var replies = [];
                for (var i = 0; i < fns.length; ++i) {
                    try {
                        replies.push(fns[i].apply(null, args));
                    } catch (e) {
                        record('page listener: ' + e);
                    }
                }
                return replies;
            }
        };
        return ev;
    }

    var lastError = null;
    function answerCb(cb, value, message) {
        if (!cb)
            return;
        lastError = message ? { message: message } : null;
        try { cb(value); } catch (e) { record('callback: ' + e); }
        lastError = null;
    }

    function finishSend(mid, value, hadListeners) {
        var entry = sendWait[mid];
        if (!entry || entry.done)
            return;
        entry.finish(value, hadListeners);
    }

    // --- ports ----------------------------------------------------------
    function newPort(name, chan, localId, sender) {
        var port = {
            name: name || '',
            sender: sender,
            onMessage: makeEvent(),
            onDisconnect: makeEvent(),
            disconnected: false
        };
        port.postMessage = function (msg) {
            if (port.disconnected)
                return;
            rpc('portMessage', { chan: chan, portId: localId,
                                 msg: msg })
                .catch(function (e) {
                    recordGap('chrome.runtime.Port.postMessage',
                              String(e && e.message || e));
                });
        };
        port.disconnect = function () {
            if (port.disconnected)
                return;
            port.disconnected = true;
            rpc('portDisconnect', { chan: chan, portId: localId })
                .catch(function () {});
            dropPort(localId, port);
        };
        agent.ports[localId] = port;
        return port;
    }
    function dropPort(localId, port) {
        if (agent.ports[localId] !== port)
            port = agent.ports[localId] || port;
        delete agent.ports[localId];
        if (port && !port.__disconnectFired) {
            port.__disconnectFired = true;
            port.disconnected = true;
            port.onDisconnect.__fire([port]);
        }
    }

    // --- runtime.sendMessage ----------------------------------------------
    function shimSendMessage() {
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
        if (extId !== cfg.id) {
            // Cross-extension messaging would need
            // externally_connectable mediation — typed absent reply.
            recordGap('chrome.runtime.sendMessage', 'cross-extension');
            answerCb(cb, undefined,
                     'chrome.runtime.sendMessage: cross-extension '
                     + 'messaging unsupported');
            return Promise.reject(new Error(
                'chrome.runtime.sendMessage: cross-extension'));
        }
        touch('chrome.runtime.sendMessage');
        var mid = cfg.id + '#pg' + (++seq) + '#' + Date.now();
        return new Promise(function (resolve) {
            var entry = {
                done: false,
                finish: function (value) {
                    if (entry.done)
                        return;
                    entry.done = true;
                    delete sendWait[mid];
                    answerCb(cb, value,
                             value === undefined
                                 ? 'The message port closed before a '
                                   + 'response was received.'
                                 : null);
                    resolve(value);
                }
            };
            sendWait[mid] = entry;
            // Chrome parity: nobody answered -> undefined + lastError,
            // never a hang.
            setTimeout(function () { entry.finish(undefined); },
                       15000);
            rpc('send', { mid: mid, msg: msg,
                          senderUrl: String(location.href) })
                .then(function (info) {
                    if (info && info.result
                            && info.result.peers === 0)
                        entry.finish(undefined);
                })
                .catch(function (e) {
                    recordGap('chrome.runtime.sendMessage',
                              String(e && e.message || e));
                    entry.finish(undefined);
                });
        });
    }

    // --- inbound host->page pushes ----------------------------------------
    // Shared runtime.onMessage fan — both delivery paths fire the
    // same listener set: '__arktos.deliver' (the extEvent push the
    // main-frame path rides) and agent.__deliverOneShot (BW12's
    // per-frame path, invoked by runJavaScript straight into the
    // addressed frame).  respond() is the single-shot answer
    // channel; the return tells the caller whether a responder may
    // still answer asynchronously (`return true` or a promise keeps
    // Chrome's channel open; any other value is ignored).
    function fireMessageListeners(msg, sender, respond) {
        var wantAsync = false;
        var listeners = shimEvents['runtime.onMessage'];
        var count = listeners ? listeners.__count() : 0;
        if (listeners) {
            var fns = listeners.__fns();
            for (var i = 0; i < fns.length; ++i) {
                try {
                    var r = fns[i].call(null, msg, sender, respond);
                    if (r === true)
                        wantAsync = true;
                    else if (r && typeof r.then === 'function') {
                        wantAsync = true;
                        r.then(function (v) { respond(v); },
                               function () { respond(undefined); });
                    }
                } catch (e) {
                    record('onMessage listener: ' + e);
                }
            }
        }
        return { count: count, wantAsync: wantAsync };
    }

    // BW12 — a one-shot delivery addressed at THIS frame
    // (tabs.sendMessage's options.frameId).  The agent runs in every
    // frame's world, but the SEC24 postMessage forwarder only exists
    // on the main frame, so a subframe agent could never see an
    // extEvent push nor answer over the 'respond' rpc — the host
    // invokes this directly inside the frame's agent world instead.
    // A synchronous settle rides the call's own return envelope; an
    // async responder parks the answer on
    // window.__arktosDeliverResults[mid] where the host's poll
    // collects it.  Both shapes become the same
    // '__arktos.deliverResponse' the channel-routed respond op makes.
    agent.__deliverOneShot = function (payload) {
        var mid = String(payload && payload.mid || '');
        var reg = window.__arktosDeliverResults =
            window.__arktosDeliverResults || {};
        var parked = false;     // async pending — reg[mid] owns it
        var result = null;      // sync settle rides the return
        var answered = false;
        function respond(value) {
            if (answered)
                return;
            answered = true;
            var entry = {
                response: value === undefined ? null : value,
                hadListeners: true
            };
            if (parked)
                reg[mid] = entry;
            else
                result = entry;
        }
        var st = fireMessageListeners(
            payload && payload.msg,
            (payload && payload.sender) || { id: cfg.id },
            respond);
        if (answered)
            return { __deliver: 1, settled: true,
                     response: result.response,
                     hadListeners: true };
        if (!st.wantAsync)
            return { __deliver: 1, settled: true,
                     response: null,
                     hadListeners: st.count > 0 };
        // The pending marker distinguishes 'still outstanding' from
        // 'no entry' for the host's poll.
        parked = true;
        reg[mid] = null;
        return { __deliver: 1, pending: true,
                 hadListeners: st.count > 0 };
    };

    function onExtEvent(name, payload) {
        if (name === '__arktos.extResponse') {
            if (payload && payload.mid)
                finishSend(payload.mid,
                           (payload.response === null
                            || payload.response === undefined)
                               ? undefined : payload.response);
            return;
        }
        if (name === '__arktos.deliver') {
            // ER02's host->page message (tabs.sendMessage /
            // runtime.sendMessage targeted at a tab): fire the
            // page-side onMessage listeners and route the response
            // back through 'respond'.
            if (!payload || !payload.mid)
                return;
            // The push reposts to EVERY agent on the document —
            // 'ext' scopes it to the addressed extension so a
            // sibling extension's listeners never fire on traffic
            // that is not theirs.
            if (payload.ext !== cfg.id)
                return;
            ++agent.deliveries;
            if (agent.deliverLog.length < 40)
                agent.deliverLog.push(
                    (payload.msg && payload.msg.command)
                        ? String(payload.msg.command)
                        : '(no-command)');
            var mid = payload.mid;
            var sender = payload.sender
                || { id: cfg.id };
            var responded = false;
            function respond(value) {
                if (responded)
                    return;
                responded = true;
                rpc('respond', { mid: mid,
                                 response: value === undefined
                                     ? null : value })
                    .catch(function (e) {
                        record('respond: ' + String(e && e.message || e));
                    });
            }
            var st = fireMessageListeners(payload.msg, sender,
                                          respond);
            if (!responded && !st.wantAsync)
                rpc('respond', { mid: mid, response: null,
                                 hadListeners: st.count > 0 })
                    .then(function (r) {
                        if (!r || r.ok !== true)
                            record('respond refused: '
                                   + JSON.stringify(r));
                    })
                    .catch(function (e) {
                        record('respond: ' + String(e && e.message || e));
                    });
            return;
        }
        if (name === '__arktos.portConnect') {
            // A host-side context connected INTO the page
            // (chrome.tabs.connect) — register a local end and fire
            // runtime.onConnect.  'ext' scopes the push like
            // __arktos.deliver's: the repost reaches every agent on
            // the document, and only the addressed extension's
            // listeners may fire.
            if (!payload || !payload.chan || payload.ext !== cfg.id)
                return;
            var localId = 'p' + (++seq) + '#' + Date.now();
            var port = newPort(payload.name || '', payload.chan,
                               localId, payload.sender);
            rpc('portAccept', { chan: payload.chan,
                                portId: localId })
                .catch(function () {});
            var connEv = shimEvents['runtime.onConnect'];
            if (connEv)
                connEv.__fire([port]);
            return;
        }
        if (name === '__arktos.portMessage') {
            if (payload && agent.ports[payload.portId])
                agent.ports[payload.portId].onMessage
                    .__fire([payload.msg,
                             agent.ports[payload.portId]]);
            return;
        }
        if (name === '__arktos.portDisconnect') {
            if (payload && agent.ports[payload.portId])
                dropPort(payload.portId,
                         agent.ports[payload.portId]);
            return;
        }
        if (name === '__arktos.storageChanged') {
            // A mutation this page made echoes back so onChanged
            // fires here too (other contexts' writes don't reach
            // the page yet — noted gap, ER05 territory).
            if (payload) {
                var area = agent.chrome.storage[payload.areaName];
                if (area && area.onChanged)
                    area.onChanged.__fire([payload.changes,
                                           payload.areaName]);
                agent.chrome.storage.onChanged
                    .__fire([payload.changes, payload.areaName]);
            }
            return;
        }
    }

    // --- transport wiring ---------------------------------------------------
    var subscribed = false;
    var subTries = 0;
    function trySubscribe() {
        if (subscribed)
            return;
        if (++subTries > 60) {
            note('arktosExt transport never came up — page has no '
                 + 'channel-world forwarder');
            return;
        }
        var id = rid();
        pendings[id] = {
            resolve: function () {
                subscribed = true;
                transportUp = true;
                flushQueue();
            },
            reject: function () {}
        };
        setTimeout(function () {
            if (pendings[id])
                delete pendings[id];
        }, 30000);
        post({ arktosBridge: 'call', kind: 'subscribe',
               object: 'arktosExt', member: 'extEvent', id: id });
        if (!subscribed)
            setTimeout(trySubscribe, 200);
    }

    window.addEventListener('message', function (event) {
        if (event.source !== window)
            return;
        var data = event.data;
        if (!data)
            return;
        if (data.arktosBridge === 'result' && data.id
                && !pendings[data.id]) {
            // A result nobody waits for is a lost call — worth a
            // note on the diagnostics surface.
            note('result with no pending call: ' + data.id);
        }
        if (data.arktosBridge === 'result' && data.id
                && pendings[data.id]) {
            var entry = pendings[data.id];
            delete pendings[data.id];
            if (data.ok)
                entry.resolve(data.value);
            else
                entry.reject(new Error('bridge-refused'));
            return;
        }
        if (data.arktosBridge === 'signal'
                && data.object === 'arktosExt'
                && data.signal === 'extEvent'
                && Array.isArray(data.args))
            onExtEvent(data.args[0], data.args[1]);
    });

    // 'hello' doubles as the per-page scoping handshake: the bridge
    // re-verifies this extension's grants against the page url and
    // answers no-host-access when they do not cover it — the typed
    // absent-API reply, surfaced here as a gap.
    function hello() {
        rpc('hello', { url: String(location.href) })
            .then(function (info) {
                agent.bridgeUp = true;
                if (info && info.name)
                    agent.name = info.name;
            })
            .catch(function (e) {
                recordGap('arktosExt.hello',
                          String(e && e.message || e));
                // A late channel can still make the agent live —
                // retry slowly like the host page does.
                setTimeout(hello, 3000);
            });
    }

    // --- the chrome.* surface ----------------------------------------------
    var shimEvents = {};
    shimEvents['runtime.onMessage'] = makeEvent();
    shimEvents['runtime.onConnect'] = makeEvent();

    var manifest = cfg.manifest || {};
    var runtime = {
        id: cfg.id,
        onMessage: shimEvents['runtime.onMessage'],
        onConnect: shimEvents['runtime.onConnect'],
        sendMessage: shimSendMessage,
        getURL: function (path) {
            return 'chrome-extension://' + cfg.id + '/'
                   + String(path || '');
        },
        getManifest: function () { return manifest; },
        connect: function (a1, a2) {
            var info = {};
            if (typeof a1 === 'string') {
                if (a1 !== cfg.id) {
                    recordGap('chrome.runtime.connect',
                              'cross-extension');
                    var dead = {
                        name: (a2 && a2.name) || '',
                        sender: { id: cfg.id },
                        onMessage: makeEvent(),
                        onDisconnect: makeEvent(),
                        disconnected: true,
                        postMessage: function () {
                            recordGap(
                                'chrome.runtime.Port.postMessage',
                                'cross-extension');
                        },
                        disconnect: function () {}
                    };
                    setTimeout(function () {
                        dead.onDisconnect.__fire([dead]);
                    }, 0);
                    return dead;
                }
                info = a2 || {};
            } else if (a1) {
                info = a1;
            }
            touch('chrome.runtime.connect');
            var localId = 'p' + (++seq) + '#' + Date.now();
            var chan = cfg.id + '#' + localId;
            var port = newPort(info.name || '', chan, localId);
            rpc('portConnect', { chan: chan, portId: localId,
                                 name: info.name || '',
                                 senderUrl:
                                     String(location.href) })
                .catch(function () {});
            return port;
        }
    };
    // lastError mirrors the callback-scoped value the answerCb sets.
    Object.defineProperty(runtime, 'lastError', {
        get: function () { return lastError; },
        set: function () {}
    });

    var extension = {
        getURL: runtime.getURL,
        inIncognitoContext: false
    };

    // BW01: chrome.dom — the one content-script namespace the agent
    // surface was missing (Bitwarden's injected autofill payloads call
    // openOrClosedShadowRoot to walk shadow DOM).  Chromium's native
    // binding returns closed roots too; no JS realm can reach those,
    // so the member answers element.shadowRoot — the real value for
    // open roots, null where the root is closed or absent.  The
    // limitation is noted once rather than silently believed.
    var domNoted = false;
    var dom = {
        openOrClosedShadowRoot: function (el) {
            touch('chrome.dom.openOrClosedShadowRoot');
            if (!domNoted) {
                domNoted = true;
                note('chrome.dom.openOrClosedShadowRoot: open '
                     + 'shadow roots only — closed roots are '
                     + 'unreachable from script');
            }
            try {
                return (el && el.shadowRoot) || null;
            } catch (e) {
                return null;
            }
        }
    };

    // chrome.storage — the full content-script surface, proxied to
    // the extension's own backend store over the 'storage' op
    // (gated C++-side on the manifest's 'storage' permission).
    function storageArea(areaName) {
        var callArea = function (op, args, cb) {
            touch('chrome.storage.' + areaName + '.' + op);
            return rpc('storage', { method: areaName + '.' + op,
                                    args: args || {} })
                .then(function (reply) {
                    var result = reply && reply.result !== undefined
                        ? reply.result : reply;
                    if (cb)
                        answerCb(cb, result);
                    return result;
                })
                .catch(function (e) {
                    var msg = String(e && e.message || e);
                    recordGap('chrome.storage.' + areaName + '.' + op,
                              msg);
                    if (cb)
                        answerCb(cb, undefined, msg);
                    else
                        return Promise.reject(e);
                });
        };
        return {
            get: function (keys, cb) {
                return callArea('get', { keys: keys }, cb);
            },
            getBytesInUse: function (keys, cb) {
                return callArea('getBytesInUse', { keys: keys }, cb);
            },
            set: function (items, cb) {
                return callArea('set', { items: items }, cb);
            },
            remove: function (keys, cb) {
                return callArea('remove', { keys: keys }, cb);
            },
            clear: function (cb) {
                return callArea('clear', {}, cb);
            },
            onChanged: makeEvent()
        };
    }
    var storage = {
        local: storageArea('local'),
        session: storageArea('session'),
        managed: storageArea('managed'),
        sync: storageArea('sync'),
        onChanged: makeEvent()
    };
    var i18nMap = cfg.i18n || {};
    var i18n = {
        getMessage: function (name, substitutions) {
            var text = i18nMap[name];
            if (!text)
                return '';
            if (substitutions !== undefined) {
                var subs = Array.isArray(substitutions)
                    ? substitutions : [substitutions];
                text = String(text).replace(
                    /\$(\d+)/g,
                    function (m, n) {
                        var v = subs[parseInt(n, 10) - 1];
                        return v === undefined ? m : String(v);
                    });
            }
            return text;
        }
    };

    // No global `chrome`: in the shared agent world a global binding
    // could only serve ONE extension honestly.  Content scripts get
    // their own extension's object through the closure the host's
    // content-script wrapper builds — it reads
    // __arktosExtAgents[<id>].chrome and calls
    // (function(chrome){ ...cs code... })(agent.chrome).
    agent.chrome = {
        runtime: runtime,
        extension: extension,
        i18n: i18n,
        storage: storage,
        dom: dom
    };
    // The page-facing alias content scripts also probe: code gated on
    // `typeof chrome !== 'undefined'` inside the wrapper resolves the
    // param, so a global here would only mislead.  ER07's
    // executeScript payloads use the same binding path.

    trySubscribe();
    hello();
})();
