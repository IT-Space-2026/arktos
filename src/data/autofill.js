// Injected by AutoFillManager::attachToPage() into the main world of
// every loaded page (WebEngine's runJavaScript replaced WebKit's
// synchronous evaluateJavaScript, so fill + capture share one script).
// Fills stored form data, then — when CAPTURE_FLAG is true — reports
// submitted forms back through the "aroraAutofill" QWebChannel object.
// FORMS_JSON and CAPTURE_FLAG are substituted C++-side.
(function () {
    var savedForms = FORMS_JSON;
    var capture = CAPTURE_FLAG;

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

    function installCapture(bridge) {
        var report = function (form) {
            if (!form || form.tagName != 'FORM')
                return;
            var data = serializeForm(form);
            if (data)
                bridge.submitForm(String(location.href), data);
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
        if (capture && window.qt && qt.webChannelTransport) {
            new QWebChannel(qt.webChannelTransport, function (channel) {
                var bridge = channel.objects.aroraAutofill;
                if (bridge)
                    installCapture(bridge);
            });
        }
        fillForms();
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
