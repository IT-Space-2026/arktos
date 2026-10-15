// SRCH01: resolves the search form behind an editable context-menu
// click for 'Add to the toolbar search' — the synchronous QWebElement
// introspection the WebKit build did is an in-page probe now.
// WebView concatenates this file ahead of each call (the
// contextimage.js / fetchLinks.js pattern), so the namespace installs
// itself once per document and stays available.
//
//   __aroraCtxSearch.formInfo(x, y) -> { ok:true, action, fields,
//                                        submits, label }
//                                    | { ok:false, reason, method? }
//
// (x,y) are CSS pixels in the viewport — the caller converts the
// request's view-pixel position by the page zoom factor.
//
// fields is the flat name/value list of a GET form in DOM order:
// text-like inputs (the clicked one flagged t:true — its slot becomes
// {searchTerms}), checked checkbox/radio, hidden inputs, and each
// select's selected option text.  Passwords are deliberately absent —
// a stored engine must never snapshot credentials.  submits collects
// the submit buttons' {label, n} so the caller can ask which one
// carries the query when a form has several.
(function () {
    if (window.__aroraCtxSearch)
        return;
    var ctl = {};

    function at(x, y) {
        try { return document.elementFromPoint(x, y); }
        catch (e) { return null; }
    }

    // Types whose keystrokes can stand in for the query — the WebKit
    // gate was type=="text" exactly; type="search" is its modern
    // spelling.  Password never qualifies.
    function queryLike(el) {
        if (!el || el.tagName !== 'INPUT')
            return false;
        var type = (el.getAttribute('type') || 'text').toLowerCase();
        return type === 'text' || type === 'search';
    }

    // Wider set whose current value a GET form actually submits.
    function valueLike(type) {
        return type === 'text' || type === 'search' || type === 'url'
            || type === 'email' || type === 'tel' || type === 'number';
    }

    function inputAt(x, y) {
        var el = at(x, y);
        while (el && el.tagName !== 'INPUT')
            el = el.parentElement;
        if (queryLike(el))
            return el;
        // The click can land on an overlaying label or a styled
        // wrapper — a right-click focuses the field first, so the
        // active element is the honest next guess.
        if (queryLike(document.activeElement))
            return document.activeElement;
        // Last resort: the bounding-rect sweep (contextimage.js's
        // byRect) catches inputs hidden behind decorative siblings.
        var els = document.querySelectorAll('input');
        for (var i = els.length - 1; i >= 0; --i) {
            if (!queryLike(els[i]))
                continue;
            var r = els[i].getBoundingClientRect();
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
                return els[i];
        }
        return null;
    }

    ctl.formInfo = function (x, y) {
        var el = inputAt(x, y);
        if (!el)
            return { ok: false, reason: 'noinput' };
        // el.form covers the ancestor walk AND form="-associated
        // controls — a strict superset of the old parent climb.
        var form = el.form;
        if (!form)
            return { ok: false, reason: 'noform' };
        if (!form.getAttribute('action'))
            return { ok: false, reason: 'noaction' };
        var method = (form.getAttribute('method') || 'get').toLowerCase();
        if (method !== 'get')
            return { ok: false, reason: 'method',
                     method: method.toUpperCase() };

        var fields = [];
        var submits = [];
        // form.elements also lists controls bound by form= outside
        // the subtree — what the browser would really submit.
        var controls = form.elements;
        for (var i = 0; i < controls.length; ++i) {
            var c = controls[i];
            if (c.tagName === 'INPUT') {
                var type = (c.getAttribute('type') || 'text').toLowerCase();
                if (type === 'submit') {
                    submits.push({ label: c.value || '', n: c.name || '' });
                } else if (valueLike(type)) {
                    fields.push({ n: c.name || '',
                                  v: c === el ? '' : (c.value || ''),
                                  t: c === el });
                } else if (type === 'checkbox' || type === 'radio') {
                    if (c.checked)
                        fields.push({ n: c.name || '', v: c.value || '' });
                } else if (type === 'hidden') {
                    fields.push({ n: c.name || '', v: c.value || '' });
                }
                // password/button/file/image/reset and friends submit
                // nothing reusable for a template — skip.
            } else if (c.tagName === 'SELECT') {
                var idx = c.selectedIndex;
                if (idx >= 0 && c.options[idx])
                    fields.push({ n: c.name || '',
                                  v: c.options[idx].text });
            }
        }

        // A <label for=...> (or a wrapping label) names the field —
        // it seeds the engine-name prompt.
        var label = '';
        if (el.labels && el.labels.length)
            label = el.labels[0].textContent;
        if (!label && el.name) {
            try {
                var l = form.querySelector(
                    'label[for="' + CSS.escape(el.name) + '"]');
                if (l)
                    label = l.textContent;
            } catch (e) { /* malformed selector — no label */ }
        }

        // The action PROPERTY resolves the attribute against the
        // document url — the absolute endpoint the template needs.
        return { ok: true,
                 action: form.action,
                 fields: fields,
                 submits: submits,
                 label: label ? label.trim() : '' };
    };

    window.__aroraCtxSearch = ctl;
}())
