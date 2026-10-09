// CTX01: resolves image content under a context-menu click point for
// the media shapes Chromium's context-menu request cannot express —
// <canvas> pixels, <video> poster art, and <img> elements reported
// with an empty mediaUrl (e.g. a broken-src placeholder).
// WebView concatenates this file ahead of each call (the fetchLinks.js
// / pip.js pattern), so the namespace installs itself once per
// document and stays available for the follow-up calls.
//
//   __aroraCtxImage.canvasData(x, y) -> { ok:true, dataUrl }
//                                     | { ok:false, reason }
//   __aroraCtxImage.imageUrl(x, y)   -> { ok:true, url } | { ok:false }
//   __aroraCtxImage.posterUrl(x, y)  -> { ok:true, url } | { ok:false }
//
// (x,y) are CSS pixels in the viewport — the caller converts the
// request's view-pixel position by the page zoom factor.
(function () {
    if (window.__aroraCtxImage)
        return;
    var ctl = {};

    function at(x, y) {
        try { return document.elementFromPoint(x, y); }
        catch (e) { return null; }
    }
    // elementFromPoint can land on an overlaying sibling that covers
    // the media element — the bounding-rect sweep catches those.
    function byRect(tag, x, y) {
        var els = document.querySelectorAll(tag);
        for (var i = els.length - 1; i >= 0; --i) {
            var r = els[i].getBoundingClientRect();
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
                return els[i];
        }
        return null;
    }
    function find(tag, x, y) {
        var el = at(x, y);
        while (el && el.tagName !== tag)
            el = el.parentElement;
        return el || byRect(tag, x, y);
    }

    // The canvas under (x,y) serialized as a png data url.  A canvas
    // tainted by cross-origin pixels throws on readback.
    ctl.canvasData = function (x, y) {
        var el = find('CANVAS', x, y);
        if (!el)
            return { ok: false, reason: 'nocanvas' };
        var dataUrl;
        try {
            dataUrl = el.toDataURL('image/png');
        } catch (e) {
            return { ok: false, reason: 'tainted' };
        }
        if (!dataUrl || dataUrl.indexOf('data:') !== 0)
            return { ok: false, reason: 'empty' };
        return { ok: true, dataUrl: dataUrl };
    };

    // The live source of the <img> under (x,y) — currentSrc covers
    // <picture> source selection and blob: sources.
    ctl.imageUrl = function (x, y) {
        var el = find('IMG', x, y);
        var url = el && (el.currentSrc || el.src);
        return url ? { ok: true, url: url }
                   : { ok: false, reason: 'noimg' };
    };

    // The poster of the <video> under (x,y) — the property resolves
    // the attribute to an absolute url ('' when there is none).
    ctl.posterUrl = function (x, y) {
        var el = find('VIDEO', x, y);
        var url = el && el.poster;
        return url ? { ok: true, url: url }
                   : { ok: false, reason: 'noposter' };
    };

    window.__aroraCtxImage = ctl;
}())
