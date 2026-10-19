// ER11 — the subresource-completion probe for observe-only
// chrome.webRequest.  The profile interceptor sees requests leave
// but never their outcomes; this DocumentCreation-point script in
// the channel world watches Resource Timing for settled subresource
// loads and reports {url, type, end, transfer} through the same
// arktosExt channel op extnavdcl.js uses.  ExtensionPageBridge
// ('resourceTiming') pairs each entry against the watcher's
// in-flight ledger by url — entries it never saw (pre-subscription
// traffic, forged posts) are ignored.
//
// Honesty: a fetch()/XHR REJECTION leaves no resource-timing entry,
// so failed subresource requests are unobservable from here — the
// watcher's onErrorOccurred answers only main-frame load failures
// and policy blocks.
(function () {
    var pending = [];
    var timer = null;
    var origin = (typeof performance !== 'undefined'
                  && performance.timeOrigin) || 0;
    function flush() {
        if (!pending.length)
            return;
        var batch = pending;
        pending = [];
        try {
            window.__arktosChannel(function (channel) {
                var ext = channel && channel.objects
                    && channel.objects.arktosExt;
                if (ext && typeof ext.invoke === 'function')
                    ext.invoke('resourceTiming',
                               {entries: batch, origin: origin},
                               function () {});
            });
        } catch (e) {}
    }
    function queue(entry) {
        if (pending.length > 400)
            pending.shift();
        pending.push({
            url: entry.name,
            type: entry.initiatorType || '',
            end: entry.responseEnd || 0,
            transfer: entry.transferSize || 0});
        if (!timer)
            timer = setTimeout(function () {
                timer = null;
                flush();
            }, 1500);
    }
    try {
        // Warm the lazy channel client now — a burst flushed during
        // teardown must not be the handshake's first message.
        window.__arktosChannel(function () {});
        var po = new PerformanceObserver(function (list) {
            var es = list.getEntries();
            for (var i = 0; i < es.length; ++i) {
                if (es[i].entryType !== 'resource')
                    continue;
                queue(es[i]);
            }
        });
        po.observe({type: 'resource', buffered: true});
        window.addEventListener('pagehide', flush);
    } catch (e) {}
}());
