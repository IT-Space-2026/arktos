// READ01/READ05: Reader Mode driver — injected by ReaderMode through
// runJavaScript into a dedicated ISOLATED script world (ReaderWorldId,
// not the page's main world), so page JavaScript can neither see nor
// redefine __aroraReader/Readability/isProbablyReaderable.  The DOM is
// shared across worlds, so extraction and overlay construction work on
// the live document exactly as before.  SEC24 moved the page's
// QWebChannel into a different isolated world (AroraChannelWorld) —
// the driver needs no channel access at all: its chrome reports through
// DOM CustomEvents on the shared document, and reader-forward.js —
// running in AroraChannelWorld where the transport lives — relays them
// to the "aroraReader" bridge object.  App-side injection is not a page
// script, so it is not subject to the page's own script-block rules;
// ReaderMode lifts JavascriptEnabled for the injection window on
// JSCTL-blocked pages.
//
// The article is extracted by the vendored Mozilla Readability
// (Apache-2.0, src/data/Readability.js + Readability-readerable.js,
// release 0.6.0).  The pipeline is now split at the trust boundary:
//
//   probe()    -> bool    "does this look like an article"
//   extract()  -> object  {ok, title, byline, siteName, excerpt,
//                          length, content}  — raw Readability output
//   << C++ sanitizes content (ReaderSanitizer — page JS cannot reach
//      or rewrite the policy) >>
//   show(opts) -> object  builds the overlay around the sanitized
//                          fragment
//   exit()     -> bool    removes the overlay
//   setFontSize(px) / setTheme(name)  live-apply + persist
//   isActive() -> bool
//
// The overlay is a fixed Shadow-DOM host holding a trusted chrome bar
// (font size, light/dark, exit) and a full-viewport SANDBOXED IFRAME
// for the article — sandboxed without allow-scripts, so nothing in
// the extracted content can execute even if the C++ sanitizer missed
// it.  allow-same-origin keeps srcdoc same-origin with the page so
// this driver can live-style the content (font/theme) and tests can
// read it; allow-top-navigation-by-user-activation + allow-popups
// keep article links behaving like they did in the old shadow-DOM
// overlay (a real click navigates the tab / opens a popup through
// the normal gate).  Page scripts could reach the same-origin frame
// too, but they could already reach the old overlay DOM — nothing
// gained there, and they cannot make its scripts run.
//
// window.__aroraReader still exits the overlay locally on tor pages
// (no forwarder is installed there), and the chrome reports preference
// changes and exits to C++ by dispatching "arora:reader:*" CustomEvents
// on the shared document — DOM events cross worlds, and
// reader-forward.js relays them to the "aroraReader" channel object in
// AroraChannelWorld, where SEC24 put the transport.
(function () {
    if (window.__aroraReader)
        return;

    var HOST_ID = "arora-reader-host";
    var MIN_FONT = 12;
    var MAX_FONT = 30;

    var state = { fontSize: 19, theme: "light" };

    function host() {
        return document.getElementById(HOST_ID);
    }

    function frame() {
        var el = host();
        return el ? el.shadowRoot.querySelector("iframe.frame") : null;
    }

    function frameRoot() {
        var f = frame();
        return f && f.contentDocument
            ? f.contentDocument.documentElement : null;
    }

    function report(name, detail) {
        try {
            document.dispatchEvent(
                new CustomEvent(name, { detail: detail }));
        } catch (e) {
        }
    }

    function savePreference(key, value) {
        report("arora:reader:pref", { key: key, value: value });
    }

    // Shared theme palette — the chrome bar consumes the --bar-*
    // vars, the article the rest.
    function themeVars() {
        return "\
.theme-light { --bg: #f8f6f1; --fg: #23231f; --link: #0b57a4; \
--bar-bg: #efece4; --bar-fg: #4a463d; --byline: #6d675c; }\
.theme-dark { --bg: #1c1c1e; --fg: #e6e2da; --link: #8ab4f8; \
--bar-bg: #262628; --bar-fg: #b9b4aa; --byline: #9a958b; }\
";
    }

    // Styles for the shadow root — the fixed host, the chrome bar and
    // the frame box.  The article itself styles inside the iframe.
    function css() {
        return "\
:host { all: initial; display: block; position: fixed; inset: 0; \
z-index: 2147483647; contain: content; }\
" + themeVars() + "\
.wrap { position: absolute; inset: 0; display: flex; \
flex-direction: column; background: var(--bg); color: var(--fg); }\
.bar { display: flex; align-items: center; \
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
iframe.frame { flex: 1; width: 100%; border: 0; display: block; \
background: var(--bg); }\
";
    }

    // Styles for the iframe document — the same article rules the old
    // overlay's .content carried, now scoped to the frame's <html>.
    function frameCss() {
        return "\
" + themeVars() + "\
html { background: var(--bg); }\
body { margin: 0; background: var(--bg); color: var(--fg); \
font-family: Georgia, 'Times New Roman', serif; \
font-size: var(--fs); line-height: 1.65; }\
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

    function applyFontSize(px) {
        var root = frameRoot();
        if (root)
            root.style.setProperty("--fs", px + "px");
    }

    function applyTheme(theme) {
        var el = host();
        var wrap = el && el.shadowRoot.querySelector(".wrap");
        var root = frameRoot();
        if (wrap) {
            wrap.classList.remove("theme-light", "theme-dark");
            wrap.classList.add("theme-" + theme);
        }
        if (root) {
            root.classList.remove("theme-light", "theme-dark");
            root.classList.add("theme-" + theme);
        }
        var themeBtn = el && el.shadowRoot.querySelector("button.theme");
        if (themeBtn)
            themeBtn.textContent = theme == "dark" ? "☀" : "☾";
    }

    // Escape text for interpolation into the srcdoc markup.
    function esc(text) {
        return String(text).replace(/&/g, "&amp;")
            .replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function frameDoc(bylineHtml) {
        return "<!DOCTYPE html><html class=\"theme-" + state.theme
            + "\" style=\"--fs:" + state.fontSize + "px\"><head>"
            + "<meta charset=\"utf-8\"><style>" + frameCss()
            + "</style></head><body><article>"
            + "<h1 class=\"title\">" + esc(state.title) + "</h1>"
            + bylineHtml
            + "<div class=\"content\">" + state.content + "</div>"
            + "</article></body></html>";
    }

    function enter(opts) {
        var existing = host();
        if (existing)
            return { ok: true, already: true };

        if (!document.body || !document.documentElement)
            return { ok: false };

        state.fontSize = Math.min(MAX_FONT,
                                  Math.max(MIN_FONT, opts.fontSize || 19));
        state.theme = opts.theme == "dark" ? "dark" : "light";
        state.title = opts.title || "";
        state.content = opts.content || "";

        var labels = opts.labels || {};
        var bylineParts = [];
        if (opts.byline)
            bylineParts.push(opts.byline);
        if (opts.siteName)
            bylineParts.push(opts.siteName);

        var hostEl = document.createElement("div");
        hostEl.id = HOST_ID;
        var shadow = hostEl.attachShadow({ mode: "open" });
        var sheet = new CSSStyleSheet();
        sheet.replaceSync(css());
        shadow.adoptedStyleSheets = [sheet];

        var wrap = document.createElement("div");
        wrap.className = "wrap theme-" + state.theme;

        var bar = document.createElement("div");
        bar.className = "bar";

        var tag = document.createElement("span");
        tag.className = "title-tag";
        tag.textContent = labels.tag || "Reader";
        bar.appendChild(tag);

        var fontDec = document.createElement("button");
        fontDec.className = "font-dec";
        fontDec.textContent = "A−";
        fontDec.title = labels.fontDec || "Smaller text";
        var fontInc = document.createElement("button");
        fontInc.className = "font-inc";
        fontInc.textContent = "A+";
        fontInc.title = labels.fontInc || "Larger text";
        var themeBtn = document.createElement("button");
        themeBtn.className = "theme";
        themeBtn.textContent = state.theme == "dark" ? "☀" : "☾";
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

        // The article renders in a sandboxed iframe — no scripts for
        // the content, same-origin so the chrome can restyle it, and
        // top navigation by real user activation so links behave the
        // way they did when the content was inline.
        var frameEl = document.createElement("iframe");
        frameEl.className = "frame";
        frameEl.setAttribute(
            "sandbox",
            "allow-same-origin"
            + " allow-top-navigation-by-user-activation"
            + " allow-popups allow-popups-to-escape-sandbox");

        var bylineHtml = "";
        if (bylineParts.length)
            bylineHtml = "<div class=\"byline\">"
                + esc(bylineParts.join(" — ")) + "</div>";
        frameEl.srcdoc = frameDoc(bylineHtml);
        // srcdoc loads asynchronously — a font/theme change fired in
        // the gap would land on the placeholder document; re-apply
        // the state once the real document arrives.
        frameEl.addEventListener("load", function () {
            applyFontSize(state.fontSize);
            applyTheme(state.theme);
        });

        wrap.appendChild(bar);
        wrap.appendChild(frameEl);
        shadow.appendChild(wrap);

        fontDec.addEventListener("click", function () {
            state.fontSize = Math.max(MIN_FONT, state.fontSize - 1);
            applyFontSize(state.fontSize);
            savePreference("fontSize", state.fontSize);
        });
        fontInc.addEventListener("click", function () {
            state.fontSize = Math.min(MAX_FONT, state.fontSize + 1);
            applyFontSize(state.fontSize);
            savePreference("fontSize", state.fontSize);
        });
        themeBtn.addEventListener("click", function () {
            state.theme = state.theme == "dark" ? "light" : "dark";
            applyTheme(state.theme);
            savePreference("theme", state.theme);
        });
        exitBtn.addEventListener("click", function () {
            window.__aroraReader.exit();
        });

        document.documentElement.appendChild(hostEl);
        return { ok: true };
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
                if (typeof isProbablyReaderable == "function") {
                    if (isProbablyReaderable(document))
                        return true;
                    // READ04: relaxed second pass — the strict
                    // thresholds miss real articles with short lead
                    // paragraphs or div-shaped copy (telex.hu).
                    if (isProbablyReaderable(document,
                            { minContentLength: 100, minScore: 10 }))
                        return true;
                }
            } catch (e) {
            }
            // Fallback heuristic: the page is article-like when it
            // holds enough paragraph text…
            var total = 0;
            var paragraphs = document.querySelectorAll("p");
            for (var i = 0; i < paragraphs.length; ++i) {
                var text = paragraphs[i].textContent.trim();
                if (text.length > 120)
                    total += text.length;
                if (total > 600)
                    return true;
            }
            // …or when a semantic article container carries
            // substantial copy (SPA markup that skips <p>).
            var blocks = document.querySelectorAll(
                "article, main, [role='main'], [itemprop='articleBody']");
            for (var j = 0; j < blocks.length; ++j) {
                if (blocks[j].textContent.trim().length > 1200)
                    return true;
            }
            return false;
        },

        // Raw extraction — sanitize happens C++-side so the policy
        // cannot be tampered with from the page.
        extract: function () {
            if (!document.body || typeof Readability != "function")
                return { ok: false };

            // Readability.parse() itself is lenient and will lift a
            // bare paragraph or two out of almost anything, so refuse
            // pages the heuristic does not consider article-like
            // before extracting.
            if (!window.__aroraReader.probe())
                return { ok: false };

            var article;
            try {
                var clone = document.cloneNode(true);
                // Cloning plugin elements makes Blink destroy and
                // rebuild their widgets in the detached document,
                // which has been seen to take the whole renderer down
                // a beat after the call returns.  Readability strips
                // these from the article content regardless, so
                // dropping them from the clone up front loses nothing.
                var plugins = clone.querySelectorAll(
                    "object, embed, applet, iframe, frame, frameset");
                for (var i = plugins.length - 1; i >= 0; --i)
                    plugins[i].remove();
                article = new Readability(clone).parse();
            } catch (e) {
                article = null;
            }
            if (!article || !article.content)
                return { ok: false };

            return {
                ok: true,
                title: article.title || "",
                byline: article.byline || "",
                siteName: article.siteName || "",
                excerpt: article.excerpt || "",
                length: article.length || 0,
                content: article.content,
            };
        },

        // Build the overlay around already-sanitized content.
        enter: enter,

        exit: function () {
            var el = host();
            if (!el)
                return false;
            el.remove();
            report("arora:reader:exit");
            return true;
        },

        setFontSize: function (px) {
            var el = host();
            if (!el)
                return;
            state.fontSize = Math.min(MAX_FONT,
                                      Math.max(MIN_FONT, px));
            applyFontSize(state.fontSize);
        },

        setTheme: function (theme) {
            var el = host();
            if (!el)
                return;
            state.theme = theme == "dark" ? "dark" : "light";
            applyTheme(state.theme);
        },
    };
}());
