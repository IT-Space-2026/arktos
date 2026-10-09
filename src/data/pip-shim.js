// PIP01: page-side Picture-in-Picture shim, armed per page by
// WebPage::init() as a QWebEngineScript at DocumentCreation (main
// world, name "arora:pip-shim"), next to the "arora:channel"
// bootstrap it talks through.
//
// Qt WebEngine surfaces no PiP negotiation: QWebEnginePermission's
// PermissionType has no PiP entry and Chromium's PiP window manager
// lives in //chrome/browser — outside the content layer Qt ships —
// so a video's requestPictureInPicture() would otherwise die
// unimplemented.  This shim keeps the page-side contract working:
// the call is gated on transient user activation like Chromium's,
// then routed over the shared channel to the C++ PictureInPicture
// controller, which pops the video into an app-owned floating window.
//
// No channel (tor pages carry none, SEC08): the shim still installs
// but rejects with NotSupportedError, the same failure a browser
// without PiP produces — the manual pop-out paths don't need the
// channel at all.
(function () {
    if (!window.HTMLVideoElement || window.__aroraPipShim)
        return;
    window.__aroraPipShim = true;

    var TAG = 'data-arora-pip';
    var pending = {};        // nonce -> {resolve, reject, video}
    var exitResolvers = [];  // document.exitPictureInPicture() waits
    var seq = 0;
    var wired = false;

    function fakePipWindow() {
        // Enough of the PictureInPictureWindow surface for sites that
        // read width/height or hang a resize listener on it.
        var listeners = {};
        return {
            width: 400,
            height: 225,
            onresize: null,
            addEventListener: function (type, fn) {
                (listeners[type] = listeners[type] || []).push(fn);
            },
            removeEventListener: function (type, fn) {
                var l = listeners[type] || [];
                var i = l.indexOf(fn);
                if (i >= 0)
                    l.splice(i, 1);
            }
        };
    }

    function wire(bridge) {
        if (!bridge)
            return null;
        if (!wired) {
            wired = true;
            bridge.popOutResolved.connect(
                function (nonce, ok, reason) {
                    var p = pending[nonce];
                    if (!p)
                        return;
                    delete pending[nonce];
                    if (ok) {
                        // detach() in pip.js already dispatched
                        // enterpictureinpicture on the element.
                        p.resolve(fakePipWindow());
                    } else {
                        p.video.removeAttribute(TAG);
                        p.reject(new DOMException(
                            reason || 'Picture-in-Picture failed',
                            'NotSupportedError'));
                    }
                });
            bridge.popOutClosed.connect(function (nonce) {
                // restore() in pip.js already dispatched
                // leavepictureinpicture and cleared the tag; this
                // resolves any pending exitPictureInPicture().
                var r = exitResolvers;
                exitResolvers = [];
                for (var i = 0; i < r.length; ++i)
                    r[i]();
            });
        }
        return bridge;
    }

    function withBridge(cb) {
        if (window.__aroraChannel) {
            window.__aroraChannel(function (channel) {
                cb(wire(channel.objects.aroraPip));
            });
        } else {
            cb(null);
        }
    }

    try {
        // Chromium reports pictureInPictureEnabled per the engine;
        // where the engine ships no PiP it stays false — advertise
        // ours so pages expose their PiP button instead of hiding it.
        if (!document.pictureInPictureEnabled) {
            Object.defineProperty(document, 'pictureInPictureEnabled', {
                configurable: true,
                get: function () { return true; }
            });
        }
        Object.defineProperty(document, 'pictureInPictureElement', {
            configurable: true,
            get: function () {
                return document.querySelector('video[' + TAG + ']');
            }
        });
    } catch (e) {}

    HTMLVideoElement.prototype.requestPictureInPicture = function () {
        var video = this;
        return new Promise(function (resolve, reject) {
            if (video.disablePictureInPicture
                || video.hasAttribute('disablepictureinpicture')) {
                reject(new DOMException(
                    'Picture-in-Picture is disabled for this video',
                    'InvalidStateError'));
                return;
            }
            // Chromium requires transient activation for PiP — keep
            // the same gate so script cannot pop windows unprompted.
            var ua = navigator.userActivation;
            if (ua && !ua.isActive) {
                reject(new DOMException(
                    'requestPictureInPicture() requires a user gesture',
                    'NotAllowedError'));
                return;
            }
            var nonce = 'pip' + (++seq) + '_' + Date.now();
            video.setAttribute(TAG, nonce);
            pending[nonce] = { resolve: resolve, reject: reject,
                               video: video };
            withBridge(function (bridge) {
                if (!bridge) {
                    delete pending[nonce];
                    video.removeAttribute(TAG);
                    reject(new DOMException(
                        'Picture-in-Picture is not available',
                        'NotSupportedError'));
                    return;
                }
                bridge.requestPopOut(nonce);
            });
        });
    };

    var nativeExit = document.exitPictureInPicture;
    document.exitPictureInPicture = function () {
        return new Promise(function (resolve, reject) {
            var video = document.querySelector('video[' + TAG + ']');
            if (!video) {
                if (nativeExit) {
                    nativeExit.call(document).then(resolve, reject);
                    return;
                }
                reject(new DOMException(
                    'No Picture-in-Picture window is open',
                    'InvalidStateError'));
                return;
            }
            exitResolvers.push(resolve);
            withBridge(function (bridge) {
                if (!bridge) {
                    exitResolvers.pop();
                    reject(new DOMException(
                        'Picture-in-Picture is not available',
                        'NotSupportedError'));
                    return;
                }
                bridge.exitPopOut(video.getAttribute(TAG));
            });
        });
    };
}())
