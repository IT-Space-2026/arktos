// Shared start-page script (startpage.html, startpage-tor.html).
// The "arktos" channel object is only registered for qrc pages
// (webpage.cpp); every call is asynchronous — results come
// back through callbacks.
//
// SEC24: the page's QWebChannel lives in an isolated script world —
// no transport is reachable here — so 'arktos' is called through the
// whitelisted postMessage forwarder arktos-channel.js arms in the
// channel world (its whitelist admits 'arktos' only on qrc: pages).
// This file carries its own tiny copy of the request/response client
// rather than leaning on arktos-mainworld.js: the tor profile arms no
// main-world shim at all, and the start page must work there too.
var arktosBridge = (function () {
    var seq = 0;
    var pending = {};     // id -> {resolve, reject}
    var signalSubs = {};  // 'object.signal' -> [handlers]

    window.addEventListener('message', function (event) {
        if (event.source !== window)
            return;
        var data = event.data;
        if (!data)
            return;
        if (data.arktosBridge == 'result') {
            var entry = pending[data.id];
            if (!entry)
                return;
            delete pending[data.id];
            if (data.ok)
                entry.resolve(data.value);
            else
                entry.reject(new Error('arktos bridge refused'));
            return;
        }
        if (data.arktosBridge == 'signal') {
            var handlers = signalSubs[data.object + '.' + data.signal]
                || [];
            var args = data.args || [];
            for (var i = 0; i < handlers.length; ++i) {
                try { handlers[i].apply(null, args); } catch (e) {}
            }
        }
    });

    function send(kind, object, member, args) {
        return new Promise(function (resolve, reject) {
            var id = ++seq;
            pending[id] = { resolve: resolve, reject: reject };
            try {
                window.postMessage({
                    arktosBridge: 'call',
                    id: id,
                    kind: kind,
                    object: object,
                    member: member,
                    args: args || []
                }, '*');
            } catch (e) {
                delete pending[id];
                reject(e);
                return;
            }
            setTimeout(function () {
                if (pending[id]) {
                    delete pending[id];
                    reject(new Error('arktos bridge unreachable'));
                }
            }, 10000);
        });
    }

    return {
        call: function (object, method) {
            return send('method', object, method,
                        Array.prototype.slice.call(arguments, 2));
        },
        get: function (object, property) {
            return send('property', object, property);
        },
        signal: function (object, signal, handler) {
            var key = object + '.' + signal;
            (signalSubs[key] = signalSubs[key] || []).push(handler);
            return send('subscribe', object, signal);
        }
    };
}());

var arktos = null;

function update()
{
    if (!arktos)
        return;

    arktos.translate('Welcome to Arktos!', function (t) {
        document.title = t;
    });
    arktos.translate('Arktos Start', function (t) {
        document.getElementById('headerTitle').innerHTML = t;
    });
    arktos.translate('Search!', function (t) {
        document.getElementById('searchButton').value = t;
    });
    document.getElementById('lineEdit').placeholder = arktos.currentEngineName;

    // Try to change the direction of the page:
    //   This string is defined in QApplication
    //   src/gui/kernel/qapplication.cpp#qt_detectRTLLanguage()
    //   it should contain LTR or RTL
    arktos.translate('QT_LAYOUT_DIRECTION', function (newDir) {
        newDir = newDir.toLowerCase();
        if ((newDir != 'ltr') && (newDir != 'rtl'))
            newDir = 'ltr';
        document.getElementsByTagName('body')[0].setAttribute('dir', newDir);
    });
}

function formSubmitted()
{
    var string = document.getElementById('lineEdit').value;

    if (string.length == 0 || !arktos)
        return;

    arktos.searchUrl(string, function (url) {
        if (url)
            window.location.href = url;
    });
}

function init()
{
    document.forms[0].lineEdit.select();
    // Fetch the two properties before publishing the facade — the
    // page reads arktos.currentEngineName synchronously once `arktos`
    // is non-null.  A refused hop leaves arktos null and the page in
    // its untranslated fallback state, same as a missing channel.
    Promise.all([
        arktosBridge.get('arktos', 'currentEngineName'),
        arktosBridge.get('arktos', 'darkChrome')
    ]).then(function (values) {
        arktos = {
            currentEngineName: values[0] || '',
            darkChrome: !!values[1],
            translate: function (string, ready) {
                arktosBridge.call('arktos', 'translate', string)
                    .then(function (translated) { ready(translated); },
                          function () { ready(string); });
            },
            searchUrl: function (string, ready) {
                arktosBridge.call('arktos', 'searchUrl', string)
                    .then(ready, function () { ready(''); });
            },
            // Mirror the QWebChannel object shape callers (and
            // --startpage-smoke's probe) expect: signal objects with
            // a connect() method.
            darkChromeChanged: {
                connect: function (handler) {
                    arktosBridge.signal('arktos', 'darkChromeChanged',
                                       handler);
                }
            },
            currentEngineNameChanged: {
                connect: function (handler) {
                    arktosBridge.signal('arktos',
                                       'currentEngineNameChanged',
                                       handler);
                }
            }
        };

        // THEME01: prefers-color-scheme tracks the OS, not the app's
        // forced palette — the channel carries the chrome scheme
        // instead, live via darkChromeChanged.
        var applyScheme = function (dark) {
            document.documentElement.classList
                .toggle('arktos-dark', !!dark);
        };
        applyScheme(arktos.darkChrome);
        // darkChromeChanged carries no value — re-read the property
        // when it fires.
        arktos.darkChromeChanged.connect(function () {
            arktosBridge.get('arktos', 'darkChrome')
                .then(function (dark) {
                    arktos.darkChrome = !!dark;
                    applyScheme(arktos.darkChrome);
                }, function () {});
        });

        // The engine can switch under a live start page — re-read the
        // property and refresh the labels when it changes.
        arktos.currentEngineNameChanged.connect(function () {
            arktosBridge.get('arktos', 'currentEngineName')
                .then(function (name) {
                    arktos.currentEngineName = name || '';
                    update();
                }, function () {});
        });

        update();
    }, function () {
        // No forwarder — keep the untranslated page.
    });
}
