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
        state: 'booting',
        bridgeUp: false,
        scriptsLoaded: false,
        errors: [],
        ticks: 0,
        lastNudge: 0
    };

    function record(what) {
        var text = String(what && what.message || what);
        if (host.errors.length < 200)
            host.errors.push(text);
        if (host.bridgeUp) {
            try { host.bridge.reportError(text); } catch (e) {}
        }
    }
    window.addEventListener('error', function (e) {
        record(e.message || e.error);
    });
    window.addEventListener('unhandledrejection', function (e) {
        record(e.reason);
    });

    // A hosted page has a DOM; the SW-only entry points a manifest's
    // background bundle may touch get page-world mappings rather than
    // throwing (spike: a one-line importScripts stub carried
    // Bitwarden's webpack chunk loading).
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
            host.state = 'failed:no-webchannel';
            return;
        }
        try {
            new QWebChannel(qt.webChannelTransport, function (channel) {
                var bridge = channel.objects.chromeBridge;
                if (!bridge) {
                    host.state = 'failed:no-chromeBridge';
                    return;
                }
                host.bridge = bridge;
                bridge.handshake(
                    { id: cfg.id, url: String(location.href) },
                    function (reply) {
                        host.bridgeReply = reply;
                        host.bridgeUp = true;
                        maybeReady();
                    });
            });
        } catch (e) {
            host.state = 'failed:channel:' + e;
        }
    }

    // --- the extension's own background scripts, manifest order ---
    function loadScripts() {
        var files = (cfg.scripts || []).slice();
        (function next() {
            var f = files.shift();
            if (!f) {
                host.scriptsLoaded = true;
                maybeReady();
                return;
            }
            var s = document.createElement('script');
            s.src = f;
            s.onload = next;
            s.onerror = function () {
                host.state = 'failed:script:' + f;
                record('background script failed to load: ' + f);
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
