// READ05: reader-mode event relay — armed once per page by ReaderMode
// as a QWebEngineScript at DocumentCreation (main world, name
// "arora:reader-forward").
//
// The reader driver runs in an isolated world where the shared
// QWebChannel transport does not exist, so its overlay chrome reports
// back by dispatching "arora:reader:*" CustomEvents on the document —
// DOM events cross worlds.  This relay forwards them to the lazy
// shared channel client (arora-channel.js's window.__aroraChannel)
// and the "aroraReader" bridge object.  The bridge has always been
// page-reachable (SEC08), so a page forging these events gains
// nothing it did not already have: notifyExited is a cosmetic state
// hint and setPreference writes two clamped settings.
(function () {
    if (window.__aroraReaderForward)
        return;
    window.__aroraReaderForward = true;

    function bridge(fn) {
        if (!window.__aroraChannel)
            return;
        try {
            window.__aroraChannel(function (channel) {
                var object = channel && channel.objects
                    && channel.objects.aroraReader;
                if (object)
                    fn(object);
            });
        } catch (e) {
        }
    }

    document.addEventListener("arora:reader:exit", function () {
        bridge(function (object) { object.notifyExited(); });
    });

    document.addEventListener("arora:reader:pref", function (event) {
        var detail = event && event.detail;
        if (!detail || typeof detail.key != "string")
            return;
        var key = detail.key;
        var value = detail.value;
        bridge(function (object) { object.setPreference(key, value); });
    });
}());
