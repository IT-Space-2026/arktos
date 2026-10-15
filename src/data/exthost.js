/*
 * EXT14 hosted-background preamble — embedded inside the generated
 * per-extension injection that ExtensionBackgroundHost arms at
 * DocumentCreation (MainWorld) on the host page at the extension's
 * own chrome-extension:// origin.  __ARORA_HOST_CONFIG is set by the
 * generated prelude: {id, name, scripts:[...]}.  This file provides:
 *
 *   - the __aroraHost state object the C++ host polls
 *     (booting -> ready | failed:<reason>),
 *   - error capture (window error + unhandledrejection) forwarded to
 *     the host side once the bridge is up,
 *   - service-worker globals a background bundle may touch
 *     (importScripts maps to a same-origin <script> append — eval()
 *     is CSP-blocked on extension pages),
 *   - the qwebchannel handshake against the registered 'chromeBridge'
 *     object (EXT15 fills the API surface behind it),
 *   - the background script loader — the extension's own files in
 *     manifest order, each appended as a same-origin <script>.
 *
 * Keep this file dependency-free and ES5-ish: it is inlined into a
 * QWebEngineScript source, so no modules and no Qt types here.
 */
(function () {
    'use strict';
    var cfg = window.__ARORA_HOST_CONFIG;
    if (!cfg || !cfg.id || window.__aroraHost)
        return;

    var host = window.__aroraHost = {
        id: cfg.id,
        name: cfg.name || '',
        // EXT17: 'background' is the hidden host page; 'page' is an
        // injected sibling document (popup/options/offscreen) — it
        // gets the shim but never the background script loader.
        kind: cfg.kind || 'background',
        state: 'booting',
        bridgeUp: false,
        scriptsLoaded: false,
        errors: [],
        ticks: 0,
        lastNudge: 0
    };

    function record(what) {
        var text = String(what && what.message || what);
        // A minified bundle's bare "Cannot read properties of
        // undefined" is undebuggable — keep the first frames.
        var stack = what && what.stack;
        if (stack)
            text += ' @' + String(stack).split('\n')
                    .slice(1, 4).join(' <-');
        if (host.errors.length < 200)
            host.errors.push(text);
        if (host.bridgeUp) {
            try { host.bridge.reportError(text); } catch (e) {}
        }
    }
    window.addEventListener('error', function (e) {
        record(e.error || e.message);
    });
    window.addEventListener('unhandledrejection', function (e) {
        record(e.reason);
    });

    // A hosted page has a DOM; the SW-only entry points a manifest's
    // background bundle may touch get page-world mappings rather than
    // throwing.  importScripts cannot be made synchronous under the
    // MV3 script-src 'self' CSP (no eval, no inline) — the C++ host
    // covers webpack's lazy chunks by merging them into the code
    // injection; this stub is a last-resort async append for chunk
    // URLs the scan did not know about.
    if (typeof self !== 'undefined' && !self.importScripts) {
        self.importScripts = function () {
            for (var i = 0; i < arguments.length; ++i) {
                var s = document.createElement('script');
                s.src = arguments[i];
                (document.head || document.documentElement)
                    .appendChild(s);
            }
        };
    }

    // Keepalive readout — ExtensionBackgroundHost nudges __aroraWake
    // on a timer; the interval ticks make residual throttling
    // observable from the host side.
    setInterval(function () { host.ticks++; }, 500);
    window.__aroraWake = function () {
        host.lastNudge = Date.now();
        return host.ticks;
    };

    function maybeReady() {
        if (host.scriptsLoaded && host.bridgeUp && host.state === 'booting')
            host.state = 'ready';
    }

    // --- chromeBridge handshake -----------------------------------
    // qt.webChannelTransport appears once the page's QWebChannel is
    // live; poll briefly rather than racing it.
    var channelTries = 0;
    var channelRetries = 0;

    // EXT19: a sibling document that never gets its channel used to
    // render a white nothing — surface the honest failure instead.
    function showBridgeFailure() {
        if (host.kind === 'background')
            return;     // the hidden host page renders nothing anyway
        var render = function () {
            if (!document.documentElement
                    || document.getElementById('arora-ext-bridge-fail'))
                return;
            var el = document.createElement('div');
            el.id = 'arora-ext-bridge-fail';
            el.setAttribute('style',
                'position:fixed;top:0;left:0;right:0;'
                + 'z-index:2147483647;background:#7f1d1d;color:#fff;'
                + 'padding:8px 10px;font:13px/1.4 sans-serif;');
            el.textContent = (host.name || host.id)
                + ': extension background not running — '
                + 'restart the browser or re-enable the extension.';
            (document.body || document.documentElement)
                .appendChild(el);
        };
        if (document.documentElement)
            render();
        else
            document.addEventListener('DOMContentLoaded', render);
    }

    // The failure is honest but not final — a channel arriving late
    // (the view watch attaches on url commit; a host may still be
    // spawning) can still make this document live.  Slow retries keep
    // the door open without hot-looping a dead path.
    function retryBridge() {
        if (host.bridgeUp || ++channelRetries > 60)
            return;
        if (typeof QWebChannel !== 'undefined'
                && typeof qt !== 'undefined'
                && qt.webChannelTransport) {
            channelTries = 0;
            host.state = 'booting';
            initBridge();
            return;
        }
        setTimeout(retryBridge, 1000);
    }

    function bridgeFailed(reason) {
        host.state = 'failed:' + reason;
        showBridgeFailure();
        setTimeout(retryBridge, 1000);
    }

    function initBridge() {
        if (host.bridgeUp || host.state !== 'booting')
            return;
        if (typeof QWebChannel === 'undefined'
                || typeof qt === 'undefined'
                || !qt.webChannelTransport) {
            if (++channelTries < 100) {
                setTimeout(initBridge, 50);
                return;
            }
            bridgeFailed('no-webchannel');
            return;
        }
        try {
            new QWebChannel(qt.webChannelTransport, function (channel) {
                var bridge = channel.objects.chromeBridge;
                if (!bridge) {
                    bridgeFailed('no-chromeBridge');
                    return;
                }
                host.bridge = bridge;
                bridge.handshake(
                    { id: cfg.id, url: String(location.href),
                      kind: host.kind,
                      shimVersion: 3 },
                    function (reply) {
                        host.bridgeReply = reply;
                        host.bridgeUp = true;
                        // EXT19: a self-healed channel drops the
                        // failure panel.
                        try {
                            var failEl = document.getElementById(
                                'arora-ext-bridge-fail');
                            if (failEl && failEl.parentNode)
                                failEl.parentNode.removeChild(failEl);
                        } catch (e) {}
                        // EXT15: the gapfill shim registers a hook so
                        // it can wire hostEvent/relay eagerly — a
                        // setTimeout poll would lose the race once
                        // the hidden page's timers get clamped.
                        try {
                            if (host.onBridgeUp)
                                host.onBridgeUp();
                        } catch (e) {
                            record('onBridgeUp hook: ' + e);
                        }
                        maybeReady();
                    });
            });
        } catch (e) {
            bridgeFailed('channel:' + e);
        }
    }

    // --- the extension's own background scripts, manifest order ---
    // cfg.inlineCode means the C++ host delivered the manifest
    // entries plus any webpack lazy chunks as a merged injection —
    // the only channel that can satisfy webpack's synchronous
    // importScripts() lazy-require under the extension CSP.  That
    // injection marks scriptsLoaded itself; nothing to do here.
    function loadScripts() {
        // EXT17: a sibling page's own markup loads its scripts —
        // injecting the background bundle there would fork the
        // extension's state per context.
        if (host.kind !== 'background') {
            host.scriptsLoaded = true;
            maybeReady();
            return;
        }
        if (cfg.inlineCode)
            return;
        var files = cfg.scripts || [];
        (function next() {
            var f = files.shift();
            if (!f) {
                // Chunks pushed before the entry's runtime patched
                // webpackChunk.push sit in the array unprocessed —
                // this webpack build does not replay the queue.
                // Re-pushing them now runs the patched push and
                // registers their modules in o.m.
                for (var k in self) {
                    if (k.indexOf('webpackChunk') === 0
                            && Array.isArray(self[k])) {
                        var arr = self[k];
                        var n = arr.length;
                        for (var i = 0; i < n; ++i) {
                            try { arr.push(arr[i]); } catch (e) {}
                        }
                    }
                }
                host.scriptsLoaded = true;
                maybeReady();
                return;
            }
            var s = document.createElement('script');
            s.src = f;
            s.onload = next;
            s.onerror = function () {
                record('script failed to load: ' + f);
                host.state = 'failed:script:' + f;
            };
            (document.head || document.documentElement).appendChild(s);
        })();
    }

    initBridge();
    if (document.documentElement)
        loadScripts();
    else
        document.addEventListener('DOMContentLoaded', loadScripts);
})();
