// Shared start-page script (startpage.html, startpage-tor.html).
// The "arora" channel object is only registered for qrc pages
// (webpage.cpp); every call is asynchronous — results come
// back through callbacks.
var arora = null;

function update()
{
    if (!arora)
        return;

    arora.translate('Welcome to Arora!', function (t) {
        document.title = t;
    });
    arora.translate('Arora Start', function (t) {
        document.getElementById('headerTitle').innerHTML = t;
    });
    arora.translate('Search!', function (t) {
        document.getElementById('searchButton').value = t;
    });
    document.getElementById('lineEdit').placeholder = arora.currentEngineName;

    // Try to change the direction of the page:
    //   This string is defined in QApplication
    //   src/gui/kernel/qapplication.cpp#qt_detectRTLLanguage()
    //   it should contain LTR or RTL
    arora.translate('QT_LAYOUT_DIRECTION', function (newDir) {
        newDir = newDir.toLowerCase();
        if ((newDir != 'ltr') && (newDir != 'rtl'))
            newDir = 'ltr';
        document.getElementsByTagName('body')[0].setAttribute('dir', newDir);
    });
}

function formSubmitted()
{
    var string = document.getElementById('lineEdit').value;

    if (string.length == 0 || !arora)
        return;

    arora.searchUrl(string, function (url) {
        window.location.href = url;
    });
}

function init()
{
    document.forms[0].lineEdit.select();
    // CHAN01: share the page's one channel client via the
    // bootstrap WebPage arms at DocumentCreation — a second
    // client would steal qt.webChannelTransport's single
    // onmessage slot from in-flight calls.  (Tor pages skip the
    // bootstrap — no co-tenant — so the direct client below is
    // their normal path, not a fallback.)
    var connect = window.__aroraChannel;
    if (!connect) {
        if (typeof QWebChannel == 'undefined' || !window.qt
            || !qt.webChannelTransport)
            return;
        connect = function (ready) {
            new QWebChannel(qt.webChannelTransport, ready);
        };
    }
    connect(function (channel) {
        arora = channel.objects.arora;
        // THEME01: prefers-color-scheme tracks the OS, not
        // the app's forced palette — the channel carries the
        // chrome scheme instead, live via darkChromeChanged.
        if (arora) {
            var applyScheme = function (dark) {
                document.documentElement.classList
                    .toggle('arora-dark', !!dark);
            };
            applyScheme(arora.darkChrome);
            arora.darkChromeChanged.connect(applyScheme);
        }
        update();
    });
}
