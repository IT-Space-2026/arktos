// Armed once per page by WebPage::init() as a QWebEngineScript at
// DocumentCreation (name "arora:channel"; the C++ side prepends
// qwebchannel.js so QWebChannel is defined here).  SEC24: it runs in
// AroraChannelWorld — the isolated script world the page's
// QWebChannel is bound to — so qt.webChannelTransport and this whole
// file are invisible to page script in the main world.
//
// qt.webChannelTransport is one object per frame whose onmessage is
// a single slot: every `new QWebChannel(transport, ...)` replaces it
// and any responses still owed to the previous client then crash the
// new owner's dispatch with "execCallbacks[message.id] is not a
// function" — the recurring console noise this file eliminates
// (CHAN01; the injected autofill bundle's client raced the start
// page's own).
//
// Instead of merely avoiding a second client, this bootstrap turns
// the slot into a demultiplexer before any page script can run: it
// installs dispatch() as the transport's onmessage and intercepts
// every later assignment, keeping each client's handler in a
// rotation.  Inbound responses carry an id owned by exactly one
// client's execCallbacks — the upstream handler throws TypeError on
// a foreign id, so that is the demux key: each handler is tried and
// the one that accepts the message wins.  (Even the init handshake
// travels as a normal exec/response pair, so the same rule covers
// it; and since every client receives the identical object list, a
// response claimed by the "wrong" client still delivers the same
// payload.)  Signals and property updates carry no id and are
// broadcast; a client that owns the object but has no slot connected
// logs "Unhandled signal", which upstream already does for
// unconnected signals.
//
// Arora-internal consumers (autofill.js, reader.js) never construct
// a QWebChannel of their own: they call __aroraChannel(ready), which
// lazily opens the single shared client — pages that never submit a
// password form never pay a handshake.
(function () {
    if (window.__aroraChannel)
        return;

    var transport = window.qt && qt.webChannelTransport;
    var handlers = [];
    var channel = null;
    var connecting = false;
    var readyQueue = [];

    function dispatch(message) {
        var data = message.data;
        if (typeof data === 'string') {
            try { data = JSON.parse(data); }
            catch (e) { data = null; }
        }
        if (data && data.type == 10) {         // QWebChannelMessageTypes.response
            // An id belongs to exactly one client's execCallbacks —
            // the upstream handler throws TypeError on a foreign id,
            // so that is the demux key: first handler to accept wins.
            // An id no client owns (e.g. a stale push) is dropped
            // quietly instead of becoming console noise.
            for (var i = 0; i < handlers.length; ++i) {
                try {
                    handlers[i](message);
                    break;
                } catch (e) {
                    // Foreign id — offer it to the next client.
                }
            }
            return;
        }
        // Signals and property updates carry no callback id:
        // broadcast — each client resolves the object name itself and
        // ignores ones it does not own.
        for (var j = 0; j < handlers.length; ++j) {
            try { handlers[j](message); } catch (e) {}
        }
    }

    if (transport) {
        if (transport.onmessage)
            handlers.push(transport.onmessage);
        try {
            Object.defineProperty(transport, 'onmessage', {
                configurable: true,
                get: function () { return dispatch; },
                set: function (handler) {
                    // Each `new QWebChannel` claims the slot here;
                    // keep its handler in the rotation instead of
                    // losing the previous client.
                    if (handlers.indexOf(handler) == -1)
                        handlers.push(handler);
                }
            });
        } catch (e) {
            // Non-configurable transport — degrade to plain
            // single-client behaviour rather than break dispatch.
            handlers.length = 0;
        }
    }

    window.__aroraChannel = function (ready) {
        if (channel) {
            ready(channel);
            return;
        }
        readyQueue.push(ready);
        if (connecting
            || typeof QWebChannel == 'undefined'
            || !transport)
            return;
        connecting = true;
        new QWebChannel(transport, function (connected) {
            channel = connected;
            connecting = false;
            var pending = readyQueue;
            readyQueue = [];
            for (var i = 0; i < pending.length; ++i) {
                try { pending[i](channel); } catch (e) {}
            }
        });
    };
}());
// ^ keep the semicolon — a leading-( IIFE after this one otherwise
// parses as a CALL on this expression (ASI does not fire here).

// ------------------------------------------------------------------
// SEC24: the main-world forwarder — the ONLY path page-side script
// has to the channel now that the transport lives in this isolated
// world.  window.postMessage crosses worlds on the shared DOMWindow,
// so arora-mainworld.js (and the start pages' inline client) post
// requests here; this side checks an explicit whitelist before any
// call is relayed to a channel object.  Anything not listed —
// aroraAutofill, aroraReader, and the whole objects table — stays
// unreachable from the main world no matter what the page forges.
//
// The locked surface, deliberately page-callable:
//   external.AddSearchProvider   window.external web-compat
//   aroraPip.requestPopOut       requestPictureInPicture shim backend
//   aroraPip.exitPopOut          document.exitPictureInPicture backend
//   aroraPip.popOutResolved/     pip-shim's async result signals
//         popOutClosed
// On qrc: pages only (the start pages — the object's sole legitimate
// consumer, and only registered there anyway):
//   arora.translate / arora.searchUrl        (methods)
//   arora.currentEngineName / arora.darkChrome (property reads)
//   arora.currentEngineNameChanged / arora.darkChromeChanged (signals)
// ------------------------------------------------------------------
(function () {
    var METHODS = {
        'external': { 'AddSearchProvider': true },
        'aroraPip': { 'requestPopOut': true, 'exitPopOut': true }
    };
    var PROPERTIES = {};
    var SIGNALS = {
        'aroraPip': { 'popOutResolved': true, 'popOutClosed': true }
    };
    if (location.protocol == 'qrc:') {
        METHODS['arora'] = { 'translate': true, 'searchUrl': true };
        PROPERTIES['arora'] = {
            'currentEngineName': true, 'darkChrome': true
        };
        SIGNALS['arora'] = {
            'currentEngineNameChanged': true, 'darkChromeChanged': true
        };
    }

    var subscribed = {};   // 'object.signal' -> true once connected

    function allowed(table, object, member) {
        return !!(table[object] && table[object][member]);
    }

    window.addEventListener('message', function (event) {
        if (event.source !== window)
            return;
        var request = event.data;
        if (!request || request.aroraBridge != 'call')
            return;

        var respond = function (ok, value) {
            try {
                window.postMessage({
                    aroraBridge: 'result',
                    id: request.id,
                    ok: !!ok,
                    value: value === undefined ? null : value
                }, '*');
            } catch (e) {}
        };

        var object = request.object;
        var member = request.member;
        var kind = request.kind;
        var table = kind == 'method' ? METHODS
                  : kind == 'property' ? PROPERTIES
                  : kind == 'subscribe' ? SIGNALS
                  : null;
        if (!table || !allowed(table, object, member)) {
            respond(false);
            return;
        }

        __aroraChannel(function (channel) {
            var target = channel && channel.objects
                && channel.objects[object];
            if (!target) {
                respond(false);
                return;
            }
            if (kind == 'property') {
                respond(true, target[member]);
                return;
            }
            if (kind == 'subscribe') {
                var signal = target[member];
                if (!signal || typeof signal.connect != 'function') {
                    respond(false);
                    return;
                }
                var key = object + '.' + member;
                if (!subscribed[key]) {
                    subscribed[key] = true;
                    signal.connect(function () {
                        var args =
                            Array.prototype.slice.call(arguments);
                        try {
                            window.postMessage({
                                aroraBridge: 'signal',
                                object: object,
                                signal: member,
                                args: args
                            }, '*');
                        } catch (e) {}
                    });
                }
                respond(true);
                return;
            }
            // kind == 'method'
            var fn = target[member];
            if (typeof fn != 'function') {
                respond(false);
                return;
            }
            var args = Array.isArray(request.args)
                ? request.args.slice() : [];
            args.push(function (value) { respond(true, value); });
            try {
                fn.apply(target, args);
            } catch (e) {
                respond(false);
            }
        });
    });
}())
