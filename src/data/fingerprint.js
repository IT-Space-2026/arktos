// SAFE06: anti-fingerprinting countermeasures, installed on the
// profile as a QWebEngineScript (DocumentCreation, MainWorld,
// subframes included, name "arora:fingerprint") by
// BrowserProfile::installFingerprintProtection when
// privacy/fingerprintProtection is on — always on for the tor
// profile.  MainWorld is required: the spoofed globals must be the
// objects page script actually touches, an isolated world's
// prototypes are invisible to it.
//
// Two placeholders are filled by FingerprintProtector::scriptSource:
//   %EXEMPTIONS% — a JSON array literal of exempt host names
//   %SEED%       — the process-lifetime noise seed
//
// The noise is keyed to one seed per browser session: every page in
// the session reads the SAME perturbed canvas, so the spoof presents
// a stable (uniform) identity while tracking scripts get a value
// that differs from the real hardware and from other sessions.  The
// perturbation is ±1 on a sparse subset of pixels — invisible to the
// eye, destructive to any hash of the readout.
//
// Honest scope: this is noise-based mitigation, not anonymity.
// Canvas/WebGL/device hints are normalized, but timing, fonts and
// the hundred other signals still identify a machine — the tor
// window is the anonymity answer (see README).

(function () {
    'use strict';

    try {
        var host = String(location.hostname || '').toLowerCase();
        var exempt = %EXEMPTIONS%;
        for (var i = 0; i < exempt.length; ++i) {
            var h = exempt[i];
            if (host === h
                || (host.length > h.length
                    && host.substring(host.length - h.length - 1)
                       === '.' + h))
                return;
        }
    } catch (e) {
        return;
    }

    var SEED = %SEED% >>> 0;

    // --- stealth -------------------------------------------------
    // Fingerprinters compare Function.prototype.toString output
    // against "[native code]" to detect wrapped functions, so the
    // patched natives are registered in a map the toString override
    // consults.
    var disguised = typeof WeakMap === 'function' ? new WeakMap() : null;
    var origFnToString = Function.prototype.toString;
    Function.prototype.toString = function () {
        try {
            if (disguised && disguised.has(this))
                return 'function ' + disguised.get(this)
                    + '() { [native code] }';
        } catch (e) {}
        return origFnToString.call(this);
    };
    function disguise(fn, name) {
        try {
            if (disguised)
                disguised.set(fn, name);
        } catch (e) {}
        return fn;
    }
    disguise(Function.prototype.toString, 'toString');

    // --- deterministic noise -------------------------------------
    function noise32(n, salt) {
        var x = (SEED ^ Math.imul(n >>> 0, 2654435761)
                 ^ Math.imul(salt >>> 0, 97)) >>> 0;
        x ^= x >>> 15;
        x = Math.imul(x, 2246822519) >>> 0;
        x ^= x >>> 13;
        return x >>> 0;
    }

    // Perturb ~1/16 of the pixels of an RGBA buffer by ±1 — the same
    // pixel always gets the same delta within the session.
    function perturb(data) {
        if (!data || !data.length)
            return;
        for (var i = 0; i < data.length; i += 4) {
            var px = i >> 2;
            if (px % 16 !== 0)
                continue;
            var n = noise32(px, 1);
            data[i] = data[i] + ((n & 1) ? 1 : -1);
            n = noise32(px, 2);
            data[i + 1] = data[i + 1] + ((n & 1) ? 1 : -1);
            n = noise32(px, 3);
            data[i + 2] = data[i + 2] + ((n & 1) ? 1 : -1);
        }
    }

    // --- canvas readout ------------------------------------------
    try {
        var origGetImageData =
            CanvasRenderingContext2D.prototype.getImageData;
        CanvasRenderingContext2D.prototype.getImageData =
            disguise(function (x, y, w, h) {
                var image = origGetImageData.apply(this, arguments);
                try {
                    perturb(image.data);
                } catch (e) {}
                return image;
            }, 'getImageData');

        // toDataURL/toBlob export the bitmap without going through
        // getImageData, so the noise is applied to a scratch copy and
        // the original exporter runs on that.
        function noisyClone(canvas) {
            var clone = document.createElement('canvas');
            clone.width = canvas.width;
            clone.height = canvas.height;
            var ctx = clone.getContext('2d');
            if (!ctx)
                return null;
            ctx.drawImage(canvas, 0, 0);
            try {
                var image = origGetImageData.call(
                    ctx, 0, 0, clone.width, clone.height);
                perturb(image.data);
                ctx.putImageData(image, 0, 0);
            } catch (e) {}
            return clone;
        }

        var origToDataURL = HTMLCanvasElement.prototype.toDataURL;
        HTMLCanvasElement.prototype.toDataURL =
            disguise(function (type) {
                var clone = noisyClone(this);
                return clone
                    ? origToDataURL.apply(clone, arguments)
                    : origToDataURL.apply(this, arguments);
            }, 'toDataURL');

        var origToBlob = HTMLCanvasElement.prototype.toBlob;
        HTMLCanvasElement.prototype.toBlob =
            disguise(function (callback, type, quality) {
                var clone = noisyClone(this);
                return clone
                    ? origToBlob.apply(clone, arguments)
                    : origToBlob.apply(this, arguments);
            }, 'toBlob');
    } catch (e) {}

    // --- WebGL ----------------------------------------------------
    // A generic renderer identity every protected session reports —
    // the real GPU/driver string is one of the highest-entropy
    // fingerprint inputs.  UNMASKED_VENDOR/RENDERER are the
    // WEBGL_debug_renderer_info constants; the numeric forms are
    // matched so an enabled extension cannot sneak the real value
    // through a different object.
    var GENERIC_GL_VENDOR = 'Google Inc. (Intel)';
    var GENERIC_GL_RENDERER =
        'ANGLE (Intel, Intel(R) UHD Graphics 630 Direct3D11 '
        + 'vs_5_0 ps_5_0, D3D11)';
    try {
        var glCtors = [];
        if (typeof WebGLRenderingContext === 'function')
            glCtors.push(WebGLRenderingContext);
        if (typeof WebGL2RenderingContext === 'function')
            glCtors.push(WebGL2RenderingContext);
        for (var g = 0; g < glCtors.length; ++g) {
            (function (proto) {
                var origGetParameter = proto.getParameter;
                proto.getParameter = disguise(function (pname) {
                    if (pname === 0x9245)
                        return GENERIC_GL_VENDOR;
                    if (pname === 0x9246)
                        return GENERIC_GL_RENDERER;
                    return origGetParameter.apply(this, arguments);
                }, 'getParameter');

                var origReadPixels = proto.readPixels;
                proto.readPixels = disguise(
                    function (x, y, w, h, format, type, pixels) {
                        var result = origReadPixels.apply(
                            this, arguments);
                        try {
                            if (pixels && pixels.length)
                                perturb(pixels);
                        } catch (e) {}
                        return result;
                    }, 'readPixels');
            })(glCtors[g].prototype);
        }
    } catch (e) {}

    // --- navigator normalization ----------------------------------
    // Values shared by the largest plausible user cohort so a
    // protected session blends in instead of standing out.
    function spoofNavigator(name, value, getterName) {
        try {
            var getter = disguise(function () { return value; },
                                  getterName || ('get ' + name));
            Object.defineProperty(Navigator.prototype, name, {
                get: getter,
                configurable: true,
                enumerable: true
            });
        } catch (e) {
            try {
                Object.defineProperty(navigator, name, {
                    get: function () { return value; },
                    configurable: true
                });
            } catch (e2) {}
        }
    }
    spoofNavigator('hardwareConcurrency', 4);
    spoofNavigator('deviceMemory', 8);

    // Plugin enumeration: real Chrome still exposes the internal
    // PDF-viewer plugins while Arora has none — the uniform answer
    // is an empty list like Tor Browser's.
    try {
        var emptyPlugins = Object.create(PluginArray.prototype);
        Object.defineProperty(emptyPlugins, 'length', { value: 0 });
        spoofNavigator('plugins', emptyPlugins);
        var emptyMimes = Object.create(MimeTypeArray.prototype);
        Object.defineProperty(emptyMimes, 'length', { value: 0 });
        spoofNavigator('mimeTypes', emptyMimes);
        spoofNavigator('pdfViewerEnabled', false);
    } catch (e) {}
})();
