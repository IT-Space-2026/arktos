// PIP01: page-side helpers for the pop-out Picture-in-Picture path.
// PictureInPicture (C++) concatenates this file ahead of each call —
// like fetchLinks.js — so the namespace installs itself once per
// document and stays available for the follow-up calls.
//
//   __aroraPipCtl.tag(nonce, spec)   -> video info or {ok:false}
//   __aroraPipCtl.detach(nonce)      -> pause + hide + placeholder
//   __aroraPipCtl.restore(nonce, st) -> undo detach, apply play state
//
// spec = { tag | x, y | mediaUrl } — the context-menu path passes the
// click point (CSS px) and the request's media url; either can miss,
// falling back to the largest visible video.
(function () {
    if (window.__aroraPipCtl)
        return;
    var TAG = 'data-arora-pip';
    var ctl = {};

    function videos() {
        return Array.prototype.slice.call(
            document.querySelectorAll('video'));
    }
    function visible(v) {
        var r = v.getBoundingClientRect();
        return r.width > 0 && r.height > 0;
    }
    function best() {
        var vs = videos().filter(visible);
        // Prefer a playing video, then the largest by area.
        vs.sort(function (a, b) {
            var ap = !a.paused && !a.ended ? 1 : 0;
            var bp = !b.paused && !b.ended ? 1 : 0;
            if (ap !== bp)
                return bp - ap;
            var ra = a.getBoundingClientRect();
            var rb = b.getBoundingClientRect();
            return rb.width * rb.height - ra.width * ra.height;
        });
        return vs[0] || videos()[0] || null;
    }
    function byTag(nonce) {
        return document.querySelector(
            'video[' + TAG + '="' + nonce + '"]');
    }

    ctl.find = function (spec) {
        spec = spec || {};
        if (spec.tag) {
            var tagged = byTag(spec.tag);
            if (tagged)
                return tagged;
        }
        if (typeof spec.x === 'number' && typeof spec.y === 'number') {
            var el = document.elementFromPoint(spec.x, spec.y);
            while (el && el.tagName !== 'VIDEO')
                el = el.parentElement;
            if (el)
                return el;
        }
        if (spec.mediaUrl) {
            var list = videos();
            for (var i = 0; i < list.length; ++i) {
                if (list[i].currentSrc === spec.mediaUrl
                    || list[i].src === spec.mediaUrl)
                    return list[i];
            }
        }
        return best();
    };

    ctl.info = function (v) {
        return {
            ok: true,
            src: v.currentSrc || v.src || '',
            time: v.currentTime || 0,
            paused: v.paused,
            ended: v.ended,
            rate: v.playbackRate || 1,
            volume: v.volume,
            muted: v.muted,
            loop: v.loop,
            poster: v.poster || '',
            width: v.videoWidth || 0,
            height: v.videoHeight || 0
        };
    };

    ctl.tag = function (nonce, spec) {
        var v = ctl.find(spec);
        if (!v)
            return { ok: false, reason: 'novideo' };
        if (v.disablePictureInPicture)
            return { ok: false, reason: 'disabled' };
        v.setAttribute(TAG, nonce);
        var i = ctl.info(v);
        i.nonce = nonce;
        return i;
    };

    ctl.detach = function (nonce) {
        var v = byTag(nonce);
        if (!v)
            return { ok: false, reason: 'gone' };
        if (v.__aroraPipDetached)
            return { ok: true };
        var r = v.getBoundingClientRect();
        var ph = document.createElement('div');
        ph.className = 'arora-pip-placeholder';
        ph.setAttribute(TAG, nonce);
        ph.textContent = 'This video is playing in Picture-in-Picture';
        ph.style.cssText =
            'display:flex;align-items:center;justify-content:center;'
            + 'box-sizing:border-box;background:#111;color:#bbb;'
            + 'font:13px sans-serif;min-width:160px;min-height:90px;'
            + 'width:' + Math.max(r.width, 160) + 'px;'
            + 'height:' + Math.max(r.height, 90) + 'px;';
        v.__aroraPipPrevDisplay = v.style.display;
        if (v.parentNode)
            v.parentNode.insertBefore(ph, v);
        v.style.setProperty('display', 'none', 'important');
        v.pause();
        v.__aroraPipDetached = true;
        // Spec event — lets page PiP buttons flip their state on the
        // manual (context-menu/menu) path too, not just the shim's.
        try {
            v.dispatchEvent(new Event('enterpictureinpicture'));
        } catch (e) {}
        return { ok: true };
    };

    ctl.restore = function (nonce, state) {
        var v = byTag(nonce);
        if (!v)
            return { ok: false, reason: 'gone' };
        var ph = document.querySelector(
            '.arora-pip-placeholder[' + TAG + '="' + nonce + '"]');
        if (ph && ph.parentNode)
            ph.parentNode.removeChild(ph);
        v.style.removeProperty('display');
        if (v.__aroraPipPrevDisplay)
            v.style.display = v.__aroraPipPrevDisplay;
        v.__aroraPipDetached = false;
        v.removeAttribute(TAG);
        if (state) {
            try { v.currentTime = state.time; } catch (e) {}
            if (!state.paused && !state.ended)
                v.play().catch(function () {});
        }
        try {
            v.dispatchEvent(new Event('leavepictureinpicture'));
        } catch (e) {}
        return { ok: true };
    };

    window.__aroraPipCtl = ctl;
}())
