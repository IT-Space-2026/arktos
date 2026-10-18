// READ05: reader-mode event relay — armed once per page by ReaderMode
// as a QWebEngineScript at DocumentCreation in the channel world
// (Engine::ArktosChannelWorld, name "arktos:reader-forward") — SEC24
// moved the transport there, so this relay must live there too.
//
// The reader driver runs in its own isolated world (ReaderWorldId)
// where the shared QWebChannel transport does not exist, so its
// overlay chrome reports back by dispatching "arktos:reader:*"
// CustomEvents on the document — DOM events cross worlds.  This relay
// forwards them to the lazy shared channel client (arktos-channel.js's
// window.__arktosChannel, armed in this world) and the "arktosReader"
// bridge object.  A page forging these events gains nothing:
// notifyExited is a cosmetic state hint and setPreference writes two
// clamped settings.
(function () {
    if (window.__arktosReaderForward)
        return;
    window.__arktosReaderForward = true;

    function bridge(fn) {
        if (!window.__arktosChannel)
            return;
        try {
            window.__arktosChannel(function (channel) {
                var object = channel && channel.objects
                    && channel.objects.arktosReader;
                if (object)
                    fn(object);
            });
        } catch (e) {
        }
    }

    document.addEventListener("arktos:reader:exit", function () {
        bridge(function (object) { object.notifyExited(); });
    });

    document.addEventListener("arktos:reader:pref", function (event) {
        var detail = event && event.detail;
        if (!detail || typeof detail.key != "string")
            return;
        var key = detail.key;
        var value = detail.value;
        bridge(function (object) { object.setPreference(key, value); });
    });
}());
