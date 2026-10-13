// READ05: reader-mode event relay — armed once per page by ReaderMode
// as a QWebEngineScript at DocumentCreation in the channel world
// (Engine::AroraChannelWorld, name "arora:reader-forward") — SEC24
// moved the transport there, so this relay must live there too.
//
// The reader driver runs in its own isolated world (ReaderWorldId)
// where the shared QWebChannel transport does not exist, so its
// overlay chrome reports back by dispatching "arora:reader:*"
// CustomEvents on the document — DOM events cross worlds.  This relay
// forwards them to the lazy shared channel client (arora-channel.js's
// window.__aroraChannel, armed in this world) and the "aroraReader"
// bridge object.  A page forging these events gains nothing:
// notifyExited is a cosmetic state hint and setPreference writes two
// clamped settings.
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
