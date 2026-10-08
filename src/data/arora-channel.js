// Armed once per page by WebPage::init() as a QWebEngineScript at
// DocumentCreation (main world, name "arora:channel"; the C++ side
// prepends qwebchannel.js so QWebChannel is defined here).
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
// Arora-internal consumers (autofill.js, startpage.html) never
// construct a QWebChannel of their own: they call
// __aroraChannel(ready), which lazily opens the single shared client
// — pages that never submit a password form never pay a handshake.
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
}())
