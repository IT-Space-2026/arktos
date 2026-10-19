// ER04 — the webNavigation.onDOMContentLoaded probe.  Qt exposes no
// engine signal for the DOMContentLoaded milestone, so the hub
// (ExtensionEventHub::wireView) installs this as a DocumentCreation-
// point QWebEngineScript in the isolated ArktosChannelWorld on every
// tracked browsing page: it warms the world's lazy channel client
// NOW (the handshake must be done before the milestone or the report
// lands after loadFinished and the hub's load gate drops it) and
// registers the listener that reports the milestone when it fires.
//
// The report rides the page's own channel client — this world's
// window.__arktosChannel, the same internal transport autofill.js
// and reader.js use — to ExtensionPageBridge::invoke('navDomReady').
// The C++ side re-verifies a genuine load is in flight before
// emitting, so a forged postMessage reaching the same op can claim
// the milestone early inside a real navigation but can never invent
// one.
//
// Originless documents (data:, about:) run no page-level scripts —
// measured in ER01 — so they never report here; the hub emits the
// event at load completion there instead, keeping the
// committed -> domContentLoaded -> completed ordering honest.
(function () {
    try {
        // Warms the lazy QWebChannel: __arktosChannel queues callbacks
        // until the client handshake finishes.
        window.__arktosChannel(function () {});
        document.addEventListener('DOMContentLoaded', function () {
            try {
                window.__arktosChannel(function (channel) {
                    var ext = channel && channel.objects
                        && channel.objects.arktosExt;
                    if (ext && typeof ext.invoke === 'function')
                        ext.invoke('navDomReady', {},
                                   function () {});
                });
            } catch (e) {}
        });
    } catch (e) {}
}());
