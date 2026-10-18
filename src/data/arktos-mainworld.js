// Armed once per page by WebPage::init() as a QWebEngineScript at
// DocumentCreation (main world, name "arktos:mainworld"; tor pages do
// not get it — the tor start page's inline client carries its own).
//
// SEC24: the page's QWebChannel lives in an isolated script world
// now — qt.webChannelTransport is not visible here, so no page-side
// code can enumerate or invoke the registered bridge objects.  This
// file is the main-world half of the one sanctioned path in: the
// whitelisted postMessage forwarder armed by arktos-channel.js in the
// channel world.  It exposes three helpers —
//
//   __arktosMainCall(object, method, ...args)  -> Promise(result)
//   __arktosMainGet(object, property)          -> Promise(value)
//   __arktosMainSignal(object, signal, fn)     -> Promise(subscribed)
//
// — which every main-world Arktos script (the PiP shim) and every
// web-compat caller (window.external below) goes through.  The
// whitelist is enforced forwarder-side; a forged request for an
// unlisted object or member gets an error response and reaches
// nothing.
//
// window.external.AddSearchProvider is the IE/Firefox-era API pages
// call to offer an OpenSearch descriptor — it is deliberately
// page-callable (the C++ side validates the url and asks for consent
// before fetching).  Chromium ships a vestigial window.external
// whose methods call into embedder plumbing Qt does not implement,
// so the real implementation is installed over it.
(function () {
    if (window.__arktosMainCall)
        return;

    var seq = 0;
    var pending = {};     // id -> {resolve, reject}
    var signalSubs = {};  // 'object.signal' -> [handlers]

    window.addEventListener('message', function (event) {
        if (event.source !== window)
            return;
        var data = event.data;
        if (!data)
            return;
        if (data.arktosBridge == 'result') {
            var entry = pending[data.id];
            if (!entry)
                return;
            delete pending[data.id];
            if (data.ok)
                entry.resolve(data.value);
            else
                entry.reject(new Error('arktos bridge refused'));
            return;
        }
        if (data.arktosBridge == 'signal') {
            var key = data.object + '.' + data.signal;
            var handlers = signalSubs[key] || [];
            var args = data.args || [];
            for (var i = 0; i < handlers.length; ++i) {
                try { handlers[i].apply(null, args); } catch (e) {}
            }
        }
    });

    function send(kind, object, member, args) {
        return new Promise(function (resolve, reject) {
            var id = ++seq;
            pending[id] = { resolve: resolve, reject: reject };
            try {
                window.postMessage({
                    arktosBridge: 'call',
                    id: id,
                    kind: kind,
                    object: object,
                    member: member,
                    args: args || []
                }, '*');
            } catch (e) {
                delete pending[id];
                reject(e);
                return;
            }
            // A page with no forwarder (channel-world bootstrap
            // disarmed) must reject, not hang the caller forever.
            setTimeout(function () {
                if (pending[id]) {
                    delete pending[id];
                    reject(new Error('arktos bridge unreachable'));
                }
            }, 10000);
        });
    }

    window.__arktosMainCall = function (object, method) {
        var args = Array.prototype.slice.call(arguments, 2);
        return send('method', object, method, args);
    };

    window.__arktosMainGet = function (object, property) {
        return send('property', object, property);
    };

    window.__arktosMainSignal = function (object, signal, handler) {
        var key = object + '.' + signal;
        (signalSubs[key] = signalSubs[key] || []).push(handler);
        return send('subscribe', object, signal);
    };

    // The web-compat surface — see the header comment.  Only
    // AddSearchProvider is (re)defined; that is the complete surface
    // 'external' exposes to page script.
    var external = window.external || (window.external = {});
    external.AddSearchProvider = function (url) {
        window.__arktosMainCall('external', 'AddSearchProvider',
                               String(url))
            .catch(function () {});
    };
}())
