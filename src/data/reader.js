// READ01: Reader Mode driver — injected into the page's main world by
// ReaderMode through QWebEnginePage::runJavaScript.  App-side
// injection is not a page script, so it is not subject to the page's
// own script-block rules; ReaderMode lifts JavascriptEnabled for the
// injection window on JSCTL-blocked pages (WebEngine never runs
// runJavaScript otherwise).
//
// The article is extracted by the vendored Mozilla Readability
// (Apache-2.0, src/data/Readability.js + Readability-readerable.js,
// release 0.6.0) and rendered into a full-viewport Shadow DOM overlay
// rather than replacing the document: the original DOM, scroll
// position and form state all survive exit, and "Exit reader view" is
// just host.remove().
//
// window.__aroraReader exposes:
//   probe()          -> bool   heuristic "does this look like an article"
//   enter(opts)      -> object {ok, title, byline, excerpt, length, already}
//   exit()           -> bool   removes the overlay, tells C++ via the bridge
//   setFontSize(px)  -> void   live-apply a base font size
//   setTheme(name)   -> void   live-apply "light" or "dark"
//   isActive()       -> bool
//
// The overlay chrome (font size, theme, exit) lives inside the shadow
// root so it works even without a QWebChannel; when the shared channel
// bootstrap is available the buttons also report to the "aroraReader"
// bridge object so preferences persist and C++ learns about JS-side
// exits (tor pages have no channel — exit then just closes the
// overlay locally).
(function () {
    if (window.__aroraReader)
        return;

    var HOST_ID = "arora-reader-host";
    var MIN_FONT = 12;
    var MAX_FONT = 30;

    function host() {
        return document.getElementById(HOST_ID);
    }

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

    function savePreference(key, value) {
        bridge(function (object) { object.setPreference(key, value); });
    }

    // Readability output is already cleaned, but the fragment is
    // inserted into the live document — strip anything interactive or
    // navigation-capable defensively.
    function sanitize(html) {
        var doc = new DOMParser().parseFromString(html, "text/html");
        var dead = doc.querySelectorAll(
            "script, iframe, frame, frameset, object, embed, applet," +
            " form, input, button, select, textarea, link, meta, base," +
            " template, noscript");
        for (var i = dead.length - 1; i >= 0; --i)
            dead[i].remove();
        var all = doc.body.querySelectorAll("*");
        for (var j = 0; j < all.length; ++j) {
            var el = all[j];
            var attrs = el.attributes;
            for (var k = attrs.length - 1; k >= 0; --k) {
                var name = attrs[k].name.toLowerCase();
                var value = attrs[k].value.trim().toLowerCase();
                if (name.indexOf("on") === 0
                    || ((name == "href" || name == "src" || name == "xlink:href")
                        && value.indexOf("javascript:") === 0))
                    el.removeAttribute(attrs[k].name);
            }
        }
        return doc.body.innerHTML;
    }

    function css() {
        return "\
:host { all: initial; display: block; position: fixed; inset: 0; \
z-index: 2147483647; contain: content; }\
.wrap { position: absolute; inset: 0; overflow-y: auto; \
background: var(--bg); color: var(--fg); \
font-family: Georgia, 'Times New Roman', serif; \
font-size: var(--fs); line-height: 1.65; }\
.wrap.theme-light { --bg: #f8f6f1; --fg: #23231f; --link: #0b57a4; \
--bar-bg: #efece4; --bar-fg: #4a463d; --byline: #6d675c; }\
.wrap.theme-dark { --bg: #1c1c1e; --fg: #e6e2da; --link: #8ab4f8; \
--bar-bg: #262628; --bar-fg: #b9b4aa; --byline: #9a958b; }\
.bar { position: sticky; top: 0; display: flex; align-items: center; \
gap: 4px; padding: 6px 16px; background: var(--bar-bg); \
color: var(--bar-fg); \
font-family: system-ui, sans-serif; font-size: 13px; \
box-shadow: 0 1px 4px rgba(0,0,0,.25); z-index: 1; }\
.bar button { font: inherit; color: inherit; background: transparent; \
border: 1px solid transparent; border-radius: 4px; \
padding: 3px 8px; cursor: pointer; }\
.bar button:hover { background: rgba(127,127,127,.18); \
border-color: rgba(127,127,127,.35); }\
.bar .spacer { flex: 1; }\
.bar .title-tag { margin-right: 8px; opacity: .7; \
white-space: nowrap; }\
article { max-width: 42em; margin: 0 auto; padding: 1.5em 1em 4em; }\
h1.title { font-size: 1.7em; line-height: 1.25; margin: 0 0 .4em; }\
.byline { color: var(--byline); font-family: system-ui, sans-serif; \
font-size: .78em; margin-bottom: 1.6em; }\
.content a { color: var(--link); }\
.content img, .content video, .content svg { max-width: 100%; \
height: auto; }\
.content pre, .content code { font-family: monospace; \
font-size: .85em; white-space: pre-wrap; }\
.content blockquote { margin: 1em 0; padding-left: 1em; \
border-left: 3px solid var(--bar-fg); opacity: .85; }\
.content table { border-collapse: collapse; max-width: 100%; \
display: block; overflow-x: auto; }\
.content td, .content th { border: 1px solid var(--bar-fg); \
padding: 4px 8px; }\
.content figcaption, .content caption { font-size: .8em; \
color: var(--byline); }\
.content hr { border: 0; border-top: 1px solid var(--bar-fg); }\
";
    }

    function applyFontSize(root, px) {
        root.style.setProperty("--fs", px + "px");
    }

    function applyTheme(wrap, theme) {
        wrap.classList.remove("theme-light", "theme-dark");
        wrap.classList.add("theme-" + theme);
    }

    function enter(opts) {
        var existing = host();
        if (existing)
            return { ok: true, already: true };

        if (!document.body || typeof Readability != "function")
            return { ok: false };

        // Readability.parse() itself is lenient and will lift a bare
        // paragraph or two out of almost anything, so refuse pages the
        // heuristic does not consider article-like before extracting.
        if (!window.__aroraReader.probe())
            return { ok: false };

        var article;
        try {
            article = new Readability(document.cloneNode(true)).parse();
        } catch (e) {
            article = null;
        }
        if (!article || !article.content)
            return { ok: false };

        var state = {
            fontSize: Math.min(MAX_FONT,
                               Math.max(MIN_FONT, opts.fontSize || 19)),
            theme: opts.theme == "dark" ? "dark" : "light",
        };
        var labels = opts.labels || {};

        var hostEl = document.createElement("div");
        hostEl.id = HOST_ID;
        var shadow = hostEl.attachShadow({ mode: "open" });
        var sheet = new CSSStyleSheet();
        sheet.replaceSync(css());
        shadow.adoptedStyleSheets = [sheet];

        var wrap = document.createElement("div");
        wrap.className = "wrap theme-" + state.theme;
        applyFontSize(wrap, state.fontSize);

        var bar = document.createElement("div");
        bar.className = "bar";

        var tag = document.createElement("span");
        tag.className = "title-tag";
        tag.textContent = labels.tag || "Reader";
        bar.appendChild(tag);

        var fontDec = document.createElement("button");
        fontDec.className = "font-dec";
        fontDec.textContent = "A\u2212";
        fontDec.title = labels.fontDec || "Smaller text";
        var fontInc = document.createElement("button");
        fontInc.className = "font-inc";
        fontInc.textContent = "A+";
        fontInc.title = labels.fontInc || "Larger text";
        var themeBtn = document.createElement("button");
        themeBtn.className = "theme";
        themeBtn.textContent = state.theme == "dark" ? "\u2600" : "\u263E";
        themeBtn.title = labels.theme || "Toggle light/dark";
        var spacer = document.createElement("span");
        spacer.className = "spacer";
        var exitBtn = document.createElement("button");
        exitBtn.className = "exit";
        exitBtn.textContent = labels.exit || "Exit reader view";
        bar.appendChild(fontDec);
        bar.appendChild(fontInc);
        bar.appendChild(themeBtn);
        bar.appendChild(spacer);
        bar.appendChild(exitBtn);

        var body = document.createElement("article");
        var title = document.createElement("h1");
        title.className = "title";
        title.textContent = article.title || "";
        var byline = document.createElement("div");
        byline.className = "byline";
        var bylineParts = [];
        if (article.byline)
            bylineParts.push(article.byline);
        if (article.siteName)
            bylineParts.push(article.siteName);
        byline.textContent = bylineParts.join(" \u2014 ");
        var content = document.createElement("div");
        content.className = "content";
        content.innerHTML = sanitize(article.content);
        body.appendChild(title);
        if (byline.textContent)
            body.appendChild(byline);
        body.appendChild(content);

        wrap.appendChild(bar);
        wrap.appendChild(body);
        shadow.appendChild(wrap);

        fontDec.addEventListener("click", function () {
            state.fontSize = Math.max(MIN_FONT, state.fontSize - 1);
            applyFontSize(wrap, state.fontSize);
            savePreference("fontSize", state.fontSize);
        });
        fontInc.addEventListener("click", function () {
            state.fontSize = Math.min(MAX_FONT, state.fontSize + 1);
            applyFontSize(wrap, state.fontSize);
            savePreference("fontSize", state.fontSize);
        });
        themeBtn.addEventListener("click", function () {
            state.theme = state.theme == "dark" ? "light" : "dark";
            applyTheme(wrap, state.theme);
            themeBtn.textContent =
                state.theme == "dark" ? "\u2600" : "\u263E";
            savePreference("theme", state.theme);
        });
        exitBtn.addEventListener("click", function () {
            window.__aroraReader.exit();
        });

        document.documentElement.appendChild(hostEl);

        return {
            ok: true,
            title: article.title || "",
            byline: article.byline || "",
            excerpt: article.excerpt || "",
            length: article.length || 0,
        };
    }

    window.__aroraReader = {
        isActive: function () {
            return !!host();
        },

        probe: function () {
            if (!document.body)
                return false;
            if (host())
                return true;
            try {
                if (typeof isProbablyReaderable == "function")
                    return !!isProbablyReaderable(document);
            } catch (e) {
            }
            // Fallback heuristic if the helper is missing: the page is
            // article-like when it holds enough paragraph text.
            var total = 0;
            var paragraphs = document.querySelectorAll("p");
            for (var i = 0; i < paragraphs.length; ++i) {
                var text = paragraphs[i].textContent.trim();
                if (text.length > 120)
                    total += text.length;
                if (total > 600)
                    return true;
            }
            return false;
        },

        enter: enter,

        exit: function () {
            var el = host();
            if (!el)
                return false;
            el.remove();
            bridge(function (object) { object.notifyExited(); });
            return true;
        },

        setFontSize: function (px) {
            var el = host();
            if (!el)
                return;
            var wrap = el.shadowRoot.querySelector(".wrap");
            applyFontSize(wrap,
                          Math.min(MAX_FONT, Math.max(MIN_FONT, px)));
        },

        setTheme: function (theme) {
            var el = host();
            if (!el)
                return;
            applyTheme(el.shadowRoot.querySelector(".wrap"),
                       theme == "dark" ? "dark" : "light");
        },
    };
}());
