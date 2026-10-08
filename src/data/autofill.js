// Armed per navigation by AutoFillManager::scheduleOnPage() as a
// per-page QWebEngineScript at DocumentReady in the page's main
// world.  Fills stored form data immediately, and — when CAPTURE_FLAG
// is true — reports submitted forms back through the "aroraAutofill"
// QWebChannel object.  FORMS_JSON, CAPTURE_FLAG and REPORT_TOKEN are
// substituted C++-side; all three stay inside this closure, so page
// script cannot read the token that authenticates a report (SEC08).
//
// The client is opened lazily and shared: qt.webChannelTransport has
// a single onmessage slot per frame and every `new QWebChannel`
// replaces it, so a second client steals the first client's in-flight
// responses — the recurring "execCallbacks[message.id] is not a
// function" errors (CHAN01) came from an autofill client colliding
// with page-side channel users such as the start page.  WebPage arms
// arora-channel.js at DocumentCreation, which owns the transport's
// dispatch and hands out one shared client via __aroraChannel(); the
// fallback below keeps the attachToPage path working on pages that
// never ran the bootstrap.
(function () {
    var savedForms = FORMS_JSON;
    var capture = CAPTURE_FLAG;
    var reportToken = "REPORT_TOKEN";

    var pendingReports = [];
    var bridge = null;
    var connecting = false;

    function fillForms() {
        for (var i = 0; i < savedForms.length; ++i) {
            var saved = savedForms[i];
            // Same lookup as the Qt4 code: document.forms["name"],
            // falling back to the first form for unnamed entries.
            var form = saved.name ? document.forms[saved.name]
                                  : document.forms[0];
            if (!form || form.tagName != 'FORM')
                continue;
            for (var j = 0; j < saved.elements.length; ++j) {
                var pair = saved.elements[j];
                var el = form.elements[pair.name];
                if (!el || el.disabled || el.readOnly)
                    continue;
                var type = (el.type || '').toLowerCase();
                if (!type || type == 'hidden' || type == 'reset'
                    || type == 'submit')
                    continue;
                if (type == 'checkbox')
                    el.checked = pair.value != '' && pair.value != 'false';
                else
                    el.value = pair.value;
            }
        }
    }

    function serializeForm(form) {
        var data = { name: form.name || '', hasPassword: false,
                     elements: [] };
        for (var i = 0; i < form.elements.length; ++i) {
            var el = form.elements[i];
            if (!el.name || el.disabled)
                continue;
            var type = (el.type || '').toLowerCase();
            if ((type == 'checkbox' || type == 'radio') && !el.checked)
                continue;
            if (type == 'password')
                data.hasPassword = true;
            data.elements.push({
                name: el.name,
                value: el.value,
                autocomplete: el.getAttribute('autocomplete') || ''
            });
        }
        // Only forms carrying a password are ever stored (matches the
        // old passwordForms setting); don't pay the channel round trip
        // for anything else.
        return (data.hasPassword && data.elements.length) ? data : null;
    }

    function flushReports() {
        while (bridge && pendingReports.length)
            bridge.submitForm(reportToken, String(location.href),
                              pendingReports.shift());
    }

    function connectChannel() {
        // __aroraChannel (arora-channel.js) serves the one shared
        // client; it queues this callback if the handshake is still
        // in flight, so no second client ever competes for the
        // transport's onmessage slot.
        if (window.__aroraChannel) {
            window.__aroraChannel(function (channel) {
                bridge = channel.objects.aroraAutofill || null;
                flushReports();
            });
            return;
        }
        // Fallback for documents where the DocumentCreation bootstrap
        // was never armed (attachToPage on a bare page).
        if (!window.qt || !qt.webChannelTransport || !window.QWebChannel)
            return;
        connecting = true;
        new QWebChannel(qt.webChannelTransport, function (channel) {
            connecting = false;
            bridge = channel.objects.aroraAutofill || null;
            flushReports();
        });
    }

    function installCapture() {
        var report = function (form) {
            if (!form || form.tagName != 'FORM')
                return;
            var data = serializeForm(form);
            if (data) {
                pendingReports.push(data);
                if (!bridge)
                    connectChannel();
            }
        };
        document.addEventListener('submit', function (event) {
            report(event.target);
        }, true);
        // form.submit() bypasses the submit event entirely; wrap it so
        // scripted submissions are captured too.
        var nativeSubmit = HTMLFormElement.prototype.submit;
        HTMLFormElement.prototype.submit = function () {
            report(this);
            return nativeSubmit.call(this);
        };
    }

    function start() {
        fillForms();
        if (capture)
            installCapture();
    }

    if (window.QWebChannel || !capture) {
        // qwebchannel.js was embedded into this script C++-side when
        // the resource was reachable; without capture there is no need
        // for the channel at all.
        start();
        return;
    }
    // Fallback: pull the channel client from the builtin qrc scheme.
    var loader = document.createElement('script');
    loader.src = 'qrc:///qtwebchannel/qwebchannel.js';
    loader.onload = start;
    (document.head || document.documentElement).appendChild(loader);
}())
