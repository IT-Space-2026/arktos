/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "bidipanel.h"

#include "arorastyle.h"
#include "bidiclient.h"
#include "engineinterface.h"
#include "enginetab.h"
#include "sourcehighlighter.h"
#include "tabwidget.h"
#include "webview.h"
#include "network/tlsverifier.h"
#include "utils/aroraicon.h"
#include "utils/flowlayout.h"

#include <qabstractitemmodel.h>
#include <qaction.h>
#include <qapplication.h>
#include <qboxlayout.h>
#include <qbuttongroup.h>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qevent.h>
#include <qheaderview.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlistwidget.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpolygon.h>
#include <qpushbutton.h>
#include <qscrollbar.h>
#include <qshortcut.h>
#include <qsortfilterproxymodel.h>
#include <qspinbox.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qstandarditemmodel.h>
#include <qstyle.h>
#include <qstyleditemdelegate.h>
#include <qstyleoption.h>
#include <qtableview.h>
#include <qtablewidget.h>
#include <qtabwidget.h>
#include <qtextcursor.h>
#include <qtextedit.h>
#include <qtextformat.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qtreewidget.h>
#include <qurl.h>
#include <qvector.h>

#include <algorithm>
#include <cmath>

// eval tags
static const quint64 TagConsole = 1;
static const quint64 TagDomInject = 2;
static const quint64 TagDomChildren = 3;
static const quint64 TagLocalStorage = 4;
static const quint64 TagCookies = 5;
// DEVT13: sessionStorage / IndexedDB / the no-CSS-Domain styles
// fallback / the inspect-overlay installer.
static const quint64 TagSessionStorage = 6;
static const quint64 TagIdb = 7;
static const quint64 TagDomProps = 8;
static const quint64 TagInspect = 9;
// DEVT19: the Elements find bar's page-side search, and the
// computed-style pick list used when the CSS domain is absent but
// DOM still resolves node ids.
static const quint64 TagDomSearch = 10;
static const quint64 TagDomStylesPick = 11;

// DEVT16: deferred getTree retries a "…attaching" web tab gets
// before the row settles on the honest "no debug target" verdict —
// 600ms apart, so ~4s of attach window.
static const int kMaxAttachRetries = 6;

/*
 * DEVT13 — arora.cdp passthrough kinds.
 *
 * Everything past the BiDi surface rides the vendor extension: raw
 * CDP commands on a page session (or the browser session), answers
 * returned verbatim so "method not found" stays a real verdict.
 * Own id->purpose map (m_cdpReqs), so these tags share no numbers
 * with the eval tags above.
 */
enum CdpKind {
    CdpProbe = 1,
    CdpResourceTree,
    CdpResourceContent,
    CdpEvalNode,
    CdpRequestNode,
    CdpComputed,
    CdpMetrics,
    CdpProfStart,
    CdpProfStop,
    CdpHeapStart,
    CdpHeapStop,
    CdpBpSet,
    CdpBpRemove,
    CdpGeneric,   // quiet housekeeping (e.g. Runtime.releaseObjectGroup)
    // DEVT19 — Elements parity reads/writes on the resolved nodeId.
    CdpAttributes,
    CdpBoxModel,
    CdpAttrSet,
    // DEVT20 — Debugger.getScriptSource bodies; n carries the
    // paused line to highlight (1-based, 0 = plain selection).
    CdpScriptSource,
    // DEVT21 — Runtime.getHeapUsage: the heap summary's used/total
    // pair, paired with a sampling pass for the top sites.
    CdpHeapUsage
};

// The cheapest REAL method per domain — enable calls are stateless,
// the read calls prove the domain serves data.  A CDP "not found /
// not implemented" error marks the feature honestly down.
static const struct {
    const char *domain;
    const char *method;
    const char *params;
} kDomainProbes[] = {
    {"Runtime", "Runtime.evaluate", "{\"expression\":\"1\"}"},
    {"DOM", "DOM.getDocument", "{\"depth\":0}"},
    {"CSS", "CSS.enable", "{}"},
    {"Page", "Page.getResourceTree", "{}"},
    {"Debugger", "Debugger.enable", "{}"},
    {"Security", "Security.enable", "{}"},
    // DEVT21: must be enable, not getMetrics — getMetrics answers
    // clean-but-EMPTY until the domain is enabled on the session,
    // so probing the read call marked Performance "up" while the
    // metrics table stayed blank forever.
    {"Performance", "Performance.enable", "{}"},
    {"Profiler", "Profiler.enable", "{}"},
    {"HeapProfiler", "HeapProfiler.enable", "{}"},
};
static const int kDomainProbeCount =
    int(sizeof(kDomainProbes) / sizeof(kDomainProbes[0]));

// DOM walking lives in the page — BiDi (and the CDP backend we
// translate through) stays out of the layout details.  The helper
// resolves an element by an index path ("0/4/2") and returns its
// element children as JSON.  DEVT22: the reply is an object —
// {label, markup, children:[{label, markup, children}]} — where
// `label` is the compact selector form (tag#id.class) the crumbs
// and expansion matching use, and `markup` is Chrome's
// <tag attr="v"> line the tree paints.  Older {label, children}
// objects and bare arrays still parse (the canned test payloads
// drive the legacy shape).
static const char kDomHelper[] =
    "window.__arora_dom_label = function(c) {"
    "  var s = c.tagName.toLowerCase();"
    "  if (c.id) s += '#' + c.id;"
    "  var cn = (typeof c.className === 'string') ? c.className.trim() : '';"
    "  if (cn) s += '.' + cn.split(/\\s+/).join('.');"
    "  return s;"
    "};"
    "window.__arora_dom_markup = function(e) {"
    "  var s = '<' + e.tagName.toLowerCase();"
    "  var n = e.attributes ? e.attributes.length : 0;"
    "  for (var i = 0; i < n && i < 8; i++) {"
    "    var v = e.attributes[i].value;"
    "    if (v.length > 40) v = v.slice(0, 39) + '\\u2026';"
    "    s += ' ' + e.attributes[i].name + '=\"' + v + '\"';"
    "  }"
    "  if (n > 8) s += ' \\u2026';"
    "  return s + '>';"
    "};"
    "window.__arora_dom_children = function(path) {"
    "  var e = document.documentElement;"
    "  if (path) { var parts = path.split('/');"
    "    for (var i = 0; i < parts.length; i++) {"
    "      var idx = +parts[i], kids = [];"
    "      for (var c = e ? e.firstElementChild : null; c; c = c.nextElementSibling) kids.push(c);"
    "      e = kids[idx];"
    "    }"
    "  }"
    "  if (!e) return 'null';"
    "  var out = [], n = 0;"
    "  for (var c = e.firstElementChild; c && n < 200; c = c.nextElementSibling) {"
    "    var cc = 0; for (var k = c.firstElementChild; k; k = k.nextElementSibling) cc++;"
    "    out.push({label: window.__arora_dom_label(c),"
    "              markup: window.__arora_dom_markup(c),"
    "              children: cc}); n++;"
    "  }"
    "  return JSON.stringify({label: window.__arora_dom_label(e),"
    "      markup: window.__arora_dom_markup(e), children: out});"
    "};"
    // DEVT19: a MutationObserver keeps the Elements tree live — the
    // bridge relays no CDP DOM.* events, so the page reports batches
    // on the same console.log channel the inspect pick uses (the
    // panel intercepts the marker before the console sees it).
    // Throttled so a mutation storm becomes one refresh.
    "if (!window.__arora_dom_obs) {"
    "  var __mt = null;"
    "  window.__arora_dom_obs = new MutationObserver(function(ms) {"
    "    for (var i = 0; i < ms.length; i++) {"
    "      var m = ms[i], own = false, j;"
    "      if (m.target && m.target.id === '__arora_overlay') continue;"
    "      for (j = 0; j < m.addedNodes.length; j++)"
    "        if (m.addedNodes[j].id === '__arora_overlay') own = true;"
    "      for (j = 0; j < m.removedNodes.length; j++)"
    "        if (m.removedNodes[j].id === '__arora_overlay') own = true;"
    "      if (own) continue;"
    "      if (__mt) clearTimeout(__mt);"
    "      __mt = setTimeout(function() { __mt = null;"
    "        console.log('__arora_dom_mutated'); }, 250);"
    "      break;"
    "    }"
    "  });"
    "  window.__arora_dom_obs.observe("
    "    document.documentElement || document,"
    "    {subtree: true, childList: true, attributes: true,"
    "     characterData: true});"
    "}"
    "'installed'";

// DEVT13: resolve an index path to the element itself — the same
// walk the children helper performs, used by the computed-styles
// chain (Runtime.evaluate returnByValue:false hands back an
// objectId) and by the attribute fallback.
static const char kDomResolve[] =
    "(function(p){var e=document.documentElement;"
    "if(p){var parts=p.split('/');"
    "for(var i=0;i<parts.length;i++){var idx=+parts[i],kids=[];"
    "for(var c=e?e.firstElementChild:null;c;c=c.nextElementSibling)"
    "kids.push(c);e=kids[idx];}}return e;})";

// DEVT13: when the CDP CSS domain is absent the styles pane falls
// back to what the DOM alone can honestly report — attributes,
// inline style, and a short getComputedStyle pick list.
static const char kDomProps[] =
    "(function(e){if(!e)return '[]';var out=[];"
    "for(var i=0;i<e.attributes.length;i++)"
    "out.push([e.attributes[i].name,e.attributes[i].value]);"
    "var cs=getComputedStyle(e),pick=['display','position','box-sizing',"
    "'color','background-color','font-family','font-size','font-weight',"
    "'line-height','margin','padding','border','width','height'];"
    "for(var k=0;k<pick.length;k++)"
    "out.push(['computed: '+pick[k],cs.getPropertyValue(pick[k])]);"
    "return JSON.stringify(out);})";

// DEVT19: same pick list WITHOUT the attribute pairs — the CDP DOM
// domain already answered attributes/box-model for this node, so
// the Computed group gets only real computed values.
static const char kDomStylesPick[] =
    "(function(e){if(!e)return '[]';var cs=getComputedStyle(e),pick=['display',"
    "'position','box-sizing','color','background-color','font-family',"
    "'font-size','font-weight','line-height','margin','padding','border',"
    "'width','height'];var out=[];for(var k=0;k<pick.length;k++)"
    "out.push([pick[k],cs.getPropertyValue(pick[k])]);"
    "return JSON.stringify(out);})";

// DEVT19: the Elements find bar's page-side search — Chrome's "find
// by string or selector".  A parseable query runs querySelectorAll;
// anything else is a case-insensitive text/tag/id/class match over
// every element.  Answers index paths in the tree's own coordinate
// space (the same walk __arora_dom_children uses), capped at 200.
// DEVT22: hits carry {path, label, markup} so the results list can
// show Chrome's <tag attr> lines; bare path strings still parse.
static const char kDomSearch[] =
    "window.__arora_dom_search = function(q) {"
    "  var out = [], i;"
    "  var lb = window.__arora_dom_label, mk = window.__arora_dom_markup;"
    "  if (!lb) lb = function(e) { return e.tagName.toLowerCase(); };"
    "  if (!mk) mk = function(e) { return '<' + lb(e) + '>'; };"
    "  var path = function(e) {"
    "    var parts = [];"
    "    while (e && e !== document.documentElement) {"
    "      var p = e.parentElement; if (!p) break; var i = 0;"
    "      for (var c = p.firstElementChild; c; c = c.nextElementSibling) {"
    "        if (c === e) break; i++;"
    "      }"
    "      parts.unshift(i); e = p;"
    "    }"
    "    return parts.join('/');"
    "  };"
    "  var hit = function(e) {"
    "    return {path: path(e), label: lb(e), markup: mk(e)};"
    "  };"
    "  try {"
    "    var nodes = document.querySelectorAll(q);"
    "    for (i = 0; i < nodes.length && out.length < 200; i++)"
    "      out.push(hit(nodes[i]));"
    "    return JSON.stringify(out);"
    "  } catch (err) {}"
    "  var low = q.toLowerCase();"
    "  var all = document.querySelectorAll('*');"
    "  for (i = 0; i < all.length && out.length < 200; i++) {"
    "    var e = all[i];"
    "    var cn = (typeof e.className === 'string') ? e.className : '';"
    "    if (e.tagName.toLowerCase().indexOf(low) >= 0"
    "        || (e.id && e.id.toLowerCase().indexOf(low) >= 0)"
    "        || cn.toLowerCase().indexOf(low) >= 0"
    "        || (e.children.length === 0 && e.textContent"
    "            && e.textContent.toLowerCase().indexOf(low) >= 0))"
    "      out.push(hit(e));"
    "  }"
    "  return JSON.stringify(out);"
    "}";

// DEVT13: click-to-inspect — a translucent overlay chases the
// hovered element; a click swallows the event, derives the same
// index path the tree uses, and announces it on console.log (the
// one channel page->panel the bridge already relays).  The entry
// is intercepted before it reaches the console list.
static const char kInspectOn[] =
    "(function(){var st=window.__arora_inspect;"
    "if(!st){st={};window.__arora_inspect=st;"
    "var ov=document.createElement('div');"
    // DEVT19: the DOM mutation observer skips changes to our own
    // inspect chrome by this id.
    "ov.id='__arora_overlay';"
    "ov.style.cssText='position:fixed;z-index:2147483647;"
    "pointer-events:none;border:2px solid #1a73e8;"
    "background:rgba(26,115,232,.2);display:none';"
    "st.ov=ov;"
    "(document.documentElement||document.body).appendChild(ov);"
    "st.move=function(ev){var t=ev.target;if(!t||t===ov)return;"
    "var r=t.getBoundingClientRect();ov.style.display='block';"
    "ov.style.left=r.left+'px';ov.style.top=r.top+'px';"
    "ov.style.width=r.width+'px';ov.style.height=r.height+'px';};"
    "st.click=function(ev){ev.preventDefault();ev.stopPropagation();"
    "var e=ev.target,parts=[];"
    "while(e&&e!==document.documentElement){var p=e.parentElement;"
    "if(!p)break;var i=0;"
    "for(var c=p.firstElementChild;c;c=c.nextElementSibling){"
    "if(c===e)break;i++;}parts.unshift(i);e=p;}"
    "console.log('__arora_inspect_pick:'+parts.join('/'));"
    "st.off();st.ov.style.display='none';};"
    "st.on=function(){document.addEventListener('mousemove',st.move,true);"
    "document.addEventListener('click',st.click,true);};"
    "st.off=function(){document.removeEventListener('mousemove',st.move,"
    "true);document.removeEventListener('click',st.click,true);};}"
    "st.on();st.ov.style.display='none';return 'inspect-on';})()";
static const char kInspectOff[] =
    "(function(){var s=window.__arora_inspect;"
    "if(s){s.off();s.ov.style.display='none';}return 'inspect-off';})()";

/*
 * DEVT06 — Chrome DevTools-style Network tab.
 *
 * The tab mirrors the Chrome layout: a toolbar row (DEVT11: record/
 * stop + clear leading, the filter field stretching, Invert /
 * Preserve log / Disable cache trailing, throttling + response
 * search honestly disabled — the bridge exposes neither), a row of
 * type-filter chips, and a table whose columns match Chrome's
 * ordering — Name, Status, Type, Initiator, Size, Time, Waterfall.
 * One row set lives in BidiNetworkModel; the filter field and chips
 * re-slice it through BidiNetworkProxy, so a chip is a filtered
 * view, not separate data.  The Waterfall column is painted by a
 * delegate against the whole recording window — the bridge emits CDP
 * MonotonicTime in ms, so the bars are real timings, not decoration.
 * DEVT11 colors each bar by resource type, runs a shared ms tick
 * ladder through the header ruler and the cell gridlines, and breaks
 * the raw CDP ResourceTiming block into per-phase hover lines.
 */

// Chrome's type chips — request.type (CDP ResourceType) buckets onto
// these; unmapped types land on Other.  NetAll is the no-filter chip.
// Values double as QButtonGroup ids, so NetAll is 0 not -1 (Qt
// reserves -1 for "no such button").
enum NetBucket {
    NetAll = 0,
    NetFetchXhr,
    NetDoc,
    NetCss,
    NetJs,
    NetFont,
    NetImg,
    NetMedia,
    NetManifest,
    NetSocket,
    NetWasm,
    NetOther
};

static int netBucketFor(const QString &cdpType)
{
    const QString t = cdpType.toLower();
    if (t == QLatin1String("xhr") || t == QLatin1String("fetch"))
        return NetFetchXhr;
    if (t == QLatin1String("document"))
        return NetDoc;
    if (t == QLatin1String("stylesheet"))
        return NetCss;
    if (t == QLatin1String("script"))
        return NetJs;
    if (t == QLatin1String("font"))
        return NetFont;
    if (t == QLatin1String("image"))
        return NetImg;
    if (t == QLatin1String("media"))
        return NetMedia;
    if (t == QLatin1String("manifest"))
        return NetManifest;
    if (t == QLatin1String("websocket"))
        return NetSocket;
    if (t == QLatin1String("wasm"))
        return NetWasm;
    return NetOther;
}

// Display name for a request row — the URL's last path segment like
// Chrome's Name column, falling back to host or a scheme stub.
static QString requestName(const QString &url)
{
    if (url.startsWith(QLatin1String("data:")))
        return url.left(32);
    const QUrl u(url);
    const QString path = u.path();
    QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    if (name.isEmpty())
        name = u.host().isEmpty() ? url : u.host() + QLatin1Char('/');
    return name;
}

// initiator {type,url,lineNumber} -> "script.js:12" / the bare type.
static QString initiatorText(const QJsonObject &initiator)
{
    const QString url = initiator[QLatin1String("url")].toString();
    if (!url.isEmpty()) {
        QString text = requestName(url);
        const int line = initiator[QLatin1String("lineNumber")].toInt(-1);
        if (line >= 0)
            text += QLatin1Char(':') + QString::number(line);
        return text;
    }
    return initiator[QLatin1String("type")].toString();
}

static QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 kB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB")
        .arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

static QString formatMillis(double ms)
{
    if (ms < 0.0)
        return QStringLiteral("-");
    if (ms < 1000.0)
        return QStringLiteral("%1 ms").arg(int(ms + 0.5));
    return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

// Failed/4xx+ rows tint red — lightened on dark palettes.
static QColor failColor()
{
    return qApp->palette().color(QPalette::Base).lightness() < 128
        ? QColor(0xff, 0x8a, 0x80)
        : QColor(0xc6, 0x28, 0x28);
}

/*
 * DEVT12 — Chromium-parity Console tab.
 *
 * The console is a structured list, not a text dump: every row is a
 * top-level item in a three-column tree — message (with the level's
 * icon and background wash), a narrow repeat-count badge, and the
 * source url:line flush right like Chrome.  The bridge normalizes
 * CDP console/Log levels onto BiDi's four (Level* below) and
 * forwards the RemoteObject args, so entries with object previews
 * expand into "name: value" children — entries without previews
 * stay honestly flat (there is no objectId round-trip to fake a
 * deeper read with).  Identical consecutive entries collapse into
 * one row with a count badge, Chrome's repeat marker.  The toolbar
 * carries the clear button, a live text filter, a checkable
 * "Default levels" dropdown (Verbose/Info/Warnings/Errors) and a
 * console-scoped Preserve log, and the REPL sits under a '>' prompt
 * row with Up/Down history recall.
 */

// The level buckets the dropdown filters on — bit index into
// m_consoleLevelMask.  The bridge emits BiDi level strings; the
// panel normalizes raw names too (tests hand events straight in).
enum ConsoleLevel {
    LevelVerbose = 0, // debug/verbose
    LevelInfo,        // log/info/dir/table/group/trace/count/…
    LevelWarning,     // warn/warning
    LevelError,       // error/assert + eval exception replies
    LevelCount
};

static int consoleLevelFor(const QString &raw)
{
    const QString l = raw.toLower();
    if (l == QLatin1String("error") || l == QLatin1String("assert"))
        return LevelError;
    if (l == QLatin1String("warn") || l == QLatin1String("warning"))
        return LevelWarning;
    if (l == QLatin1String("debug") || l == QLatin1String("verbose"))
        return LevelVerbose;
    return LevelInfo;
}

// Chrome's default set — Verbose off, the rest on.
static const int kDefaultConsoleLevels =
    (1 << LevelInfo) | (1 << LevelWarning) | (1 << LevelError);

// Item data roles on column 0.
enum ConsoleRole {
    ConsoleLevelRole = Qt::UserRole,        // ConsoleLevel
    ConsoleKeyRole = Qt::UserRole + 1,      // dedupe key, "" = never
    ConsoleCountRole = Qt::UserRole + 2     // repeat count (default 1)
};

// Per-level row wash — Chrome's subtle background tint (red for
// errors, amber for warnings); verbose dims the text instead.
static QColor consoleWash(int level, const QPalette &pal)
{
    switch (level) {
    case LevelError:
        return AroraStyle::blend(pal.color(QPalette::Base),
                                 failColor(), 0.16);
    case LevelWarning:
        return AroraStyle::blend(pal.color(QPalette::Base),
                                 QColor(0xf9, 0xab, 0x00), 0.14);
    }
    return QColor();
}

// One object preview becomes expandable "name: value" children —
// nested valuePreview recurses (bounded), preview.overflow gets
// Chrome's trailing "…" row.  Only args carrying a CDP preview
// produce children; anything else renders flat.
static void consolePreviewChildren(QTreeWidgetItem *parent,
                                   const QJsonArray &args,
                                   const QColor &wash, int depth)
{
    if (depth > 4)
        return;
    for (const QJsonValue &av : args) {
        const QJsonObject preview =
            av.toObject()[QLatin1String("preview")].toObject();
        const QJsonArray props =
            preview[QLatin1String("properties")].toArray();
        if (props.isEmpty())
            continue;
        for (const QJsonValue &pv : props) {
            const QJsonObject prop = pv.toObject();
            QString line =
                prop[QLatin1String("name")].toString()
                + QLatin1String(": ");
            const QString value =
                prop[QLatin1String("value")].toString();
            const QJsonObject nested =
                prop[QLatin1String("valuePreview")].toObject();
            if (!value.isEmpty())
                line += value;
            else
                line += nested[QLatin1String("description")].toString();
            QTreeWidgetItem *child = new QTreeWidgetItem(
                QStringList(line));
            if (wash.isValid())
                child->setBackground(0, wash);
            parent->addChild(child);
            const QJsonArray nestedProps =
                nested[QLatin1String("properties")].toArray();
            if (!nestedProps.isEmpty()) {
                consolePreviewChildren(
                    child,
                    QJsonArray{QJsonObject{
                        {QLatin1String("preview"), nested}}},
                    wash, depth + 1);
            }
        }
        if (preview[QLatin1String("overflow")].toBool()) {
            QTreeWidgetItem *more = new QTreeWidgetItem(
                QStringList(QStringLiteral("…")));
            if (wash.isValid())
                more->setBackground(0, wash);
            parent->addChild(more);
        }
    }
}

// DEVT11: the record toggle's dot — red while capturing, the
// disabled-text grey while paused (Chrome's circle semantics, the
// same icon at both states so the control reads as a toggle).
static QIcon recordDotIcon(bool recording, const QPalette &palette)
{
    QPixmap dot(16, 16);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(recording
        ? AroraStyle::recordDotColor()
        : palette.color(QPalette::Disabled, QPalette::ButtonText));
    painter.drawEllipse(QRectF(3.0, 3.0, 10.0, 10.0));
    return QIcon(dot);
}

// DEVT13: the Elements tab's inspect-picker glyph — a corner arrow
// into a box, Chrome's "select an element" cursor.  Same icon at
// both states; the button's checked tint carries the state.
static QIcon inspectPickerIcon(const QPalette &palette)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = palette.color(QPalette::ButtonText);
    p.setPen(QPen(c, 1.6));
    p.drawRect(QRectF(6.5, 6.5, 7.5, 7.5));
    QPolygonF arrow;
    arrow << QPointF(2, 2) << QPointF(8, 5) << QPointF(5, 8);
    p.setBrush(c);
    p.drawPolygon(arrow);
    return QIcon(pm);
}

// DEVT22 — Elements Chrome-UX parity.
//
// Column-0 roles on m_domTree items (Qt::UserRole itself is the
// index path, set by the lazy fetch):
//   DomCrumbRole — the compact selector label ("div#id.class") the
//                  breadcrumb bar and the live-refresh expansion
//                  snapshot match on; the display text is the wider
//                  <tag attr="v"> markup, which changes on every
//                  attribute mutation and would needlessly collapse
//                  branches if it keyed the snapshot.
//   DomBoxRole   — marks the styles pane's spanned box-model row.
static const int DomCrumbRole = Qt::UserRole + 6;
static const int DomBoxRole = Qt::UserRole + 7;

// Synthesize "<tag id="i" class="a b">" from the compact
// tag#id.class1.class2 label — the fallback when a children reply
// carries no markup field (canned payloads, an older helper).
static QString domMarkupFromLabel(const QString &label)
{
    const int hash = label.indexOf(QLatin1Char('#'));
    const int dot = label.indexOf(QLatin1Char('.'));
    int cut = label.size();
    if (hash >= 0 && hash < cut)
        cut = hash;
    if (dot >= 0 && dot < cut)
        cut = dot;
    const QString tag = label.left(cut);
    const QString rest = label.mid(cut);
    QString id, cls;
    if (rest.startsWith(QLatin1Char('#'))) {
        const QString tail = rest.mid(1);
        const int d = tail.indexOf(QLatin1Char('.'));
        id = d < 0 ? tail : tail.left(d);
        if (d >= 0)
            cls = tail.mid(d + 1);
    } else if (rest.startsWith(QLatin1Char('.'))) {
        cls = rest.mid(1);
    }
    cls.replace(QLatin1Char('.'), QLatin1Char(' '));
    QString out = QLatin1Char('<') + tag;
    if (!id.isEmpty())
        out += QStringLiteral(" id=\"%1\"").arg(id);
    if (!cls.isEmpty())
        out += QStringLiteral(" class=\"%1\"").arg(cls);
    return out + QLatin1Char('>');
}

// The stable key the expansion snapshot and the crumb bar match on:
// the compact selector label.  The markup display text changes on
// every attribute mutation and would collapse branches the label
// still identifies.
static QString domItemKey(const QTreeWidgetItem *item)
{
    const QString key = item->data(0, DomCrumbRole).toString();
    return key.isEmpty() ? item->text(0) : key;
}

// Chrome's Elements tree paints markup lines, not flat selector
// text: "<tag" and ">" in the accent color, attribute names in a
// muted ink, quoted values in the link color.  Rows that aren't
// markup (the loading placeholder, verdict rows) paint stock.
class BidiDomDelegate : public QStyledItemDelegate
{
public:
    explicit BidiDomDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
        // Test seam — the class is file-local, so the suite reads
        // this marker instead of naming the type.
        setProperty("aroraDomMarkup", true);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString text = index.data(Qt::DisplayRole).toString();
        if (!text.startsWith(QLatin1Char('<'))
            || !text.endsWith(QLatin1Char('>'))) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *w = opt.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette &pal = opt.palette;
        const QColor tagColor =
            selected ? AroraStyle::selectionTextColor(pal)
                     : AroraStyle::accentFillColor(pal);
        const QColor nameColor =
            selected ? AroraStyle::selectionTextColor(pal)
                     : AroraStyle::blend(pal.color(QPalette::Text),
                                         pal.color(QPalette::Link),
                                         0.55);
        const QColor valueColor =
            selected ? AroraStyle::selectionTextColor(pal)
                     : pal.color(QPalette::Link);

        // Split "<tag a="v" b="w" …>" into {text, color} spans.
        struct Span { QString text; QColor color; };
        QVector<Span> spans;
        int i = 1;
        int tagEnd = i;
        while (tagEnd < text.size()
               && !text.at(tagEnd).isSpace()
               && text.at(tagEnd) != QLatin1Char('>'))
            ++tagEnd;
        spans.append({text.left(tagEnd), tagColor});
        i = tagEnd;
        const int n = text.size();
        while (i < n) {
            const int ws = i;
            while (i < n && text.at(i).isSpace())
                ++i;
            if (i >= n || text.at(i) == QLatin1Char('>'))
                break;
            QString chunk = text.mid(ws, i - ws);
            // One attribute: name="value" | name='value' | …
            int nameEnd = i;
            while (nameEnd < n && text.at(nameEnd) != QLatin1Char('=')
                   && text.at(nameEnd) != QLatin1Char('>')
                   && !text.at(nameEnd).isSpace())
                ++nameEnd;
            const QString name = text.mid(i, nameEnd - i);
            QString value;
            i = nameEnd;
            if (i < n && text.at(i) == QLatin1Char('=')) {
                ++i;
                if (i < n && (text.at(i) == QLatin1Char('"')
                              || text.at(i) == QLatin1Char('\''))) {
                    const QChar q = text.at(i);
                    const int close = text.indexOf(q, i + 1);
                    const int end = close < 0 ? n : close + 1;
                    value = text.mid(i, end - i);
                    i = end;
                } else {
                    int vEnd = i;
                    while (vEnd < n && !text.at(vEnd).isSpace()
                           && text.at(vEnd) != QLatin1Char('>'))
                        ++vEnd;
                    value = text.mid(i, vEnd - i);
                    i = vEnd;
                }
            }
            if (name == QLatin1String("…")) {
                spans.append({chunk + name, tagColor});
            } else {
                spans.append({chunk + name
                              + (value.isEmpty() ? QString()
                                                 : QStringLiteral("=")),
                              nameColor});
                if (!value.isEmpty())
                    spans.append({value, valueColor});
            }
        }
        // ">" closes the line — keep it attached through elision.
        spans.append({QStringLiteral(">"), tagColor});

        QRect r = style->subElementRect(
            QStyle::SE_ItemViewItemText, &opt, w);
        const int avail = r.width();
        const QFontMetrics fm = opt.fontMetrics;
        painter->save();
        painter->setFont(opt.font);
        const int baseY = r.y() + fm.ascent()
            + (r.height() - fm.height()) / 2;
        int x = r.x();
        for (int s = 0; s < spans.size(); ++s) {
            const int wdt = fm.horizontalAdvance(spans[s].text);
            if (x + wdt > r.x() + avail && s > 0) {
                painter->setPen(tagColor);
                painter->drawText(x, baseY,
                                  QStringLiteral("…"));
                x += fm.horizontalAdvance(QStringLiteral("…"));
                painter->drawText(x, baseY, QStringLiteral(">"));
                break;
            }
            painter->setPen(spans[s].color);
            painter->drawText(x, baseY, spans[s].text);
            x += wdt;
        }
        painter->restore();
    }
};

// Chrome's Computed box-model diagram: nested margin / border /
// padding / content boxes with the DOM.getBoxModel numbers painted
// on each edge (numbers annotate — the boxes aren't to scale, same
// as Chrome).  State and every edge value ride on dynamic
// properties so tests and smokes read them without naming this
// file-local class.
class BidiBoxModelView : public QWidget
{
public:
    // Edges are CSS order: top, right, bottom, left.
    explicit BidiBoxModelView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QLatin1String("bidiElementsBoxModel"));
        setProperty("state", QStringLiteral("empty"));
        setToolTip(QStringLiteral(
            "margin / border / padding / content — DOM.getBoxModel"));
    }

    void setModel(const QVector<double> &margin,
                  const QVector<double> &border,
                  const QVector<double> &padding,
                  const QSizeF &content)
    {
        m_margin = margin;
        m_border = border;
        m_padding = padding;
        m_content = content;
        m_error.clear();
        static const char *props[] = {"mt", "mr", "mb", "ml",
                                      "bt", "br", "bb", "bl",
                                      "pt", "pr", "pb", "pl"};
        const QVector<double> *rings[] = {&m_margin, &m_border,
                                          &m_padding};
        for (int ring = 0; ring < 3; ++ring)
            for (int e = 0; e < 4; ++e)
                setProperty(props[ring * 4 + e],
                            rings[ring]->value(e, 0.0));
        setProperty("cw", content.width());
        setProperty("ch", content.height());
        setProperty("state", QStringLiteral("model"));
        setProperty("error", QString());
        update();
    }

    void setError(const QString &why)
    {
        m_margin.clear();
        m_border.clear();
        m_padding.clear();
        m_content = QSizeF();
        m_error = why;
        setProperty("state", QStringLiteral("error"));
        setProperty("error", why);
        update();
    }

    bool hasModel() const
    {
        return m_error.isEmpty() && m_content.isValid();
    }

    QSize sizeHint() const override { return QSize(0, 148); }
    QSize minimumSizeHint() const override { return QSize(0, 96); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette &pal = palette();
        const QRect area = rect().adjusted(6, 4, -6, -4);
        QFont f = font();
        f.setPointSizeF(qMax(7.5, f.pointSizeF() * 0.82));
        p.setFont(f);
        const QFontMetrics fm(f);

        if (!hasModel()) {
            p.setPen(pal.color(QPalette::Disabled, QPalette::Text));
            p.drawText(area, Qt::AlignCenter | Qt::TextWordWrap,
                       m_error.isEmpty() ? QString() : m_error);
            return;
        }

        // Ring thickness: room for the caption + the edge numbers.
        int t = qBound(16, qMin(area.width(), area.height()) / 7, 30);
        while (t > 12
               && area.adjusted(3 * t, 3 * t, -3 * t, -3 * t)
                          .height() < fm.height() + 8)
            --t;
        const QRect marginR = area;
        const QRect borderR = marginR.adjusted(t, t, -t, -t);
        const QRect paddingR = borderR.adjusted(t, t, -t, -t);
        const QRect contentR = paddingR.adjusted(t, t, -t, -t);

        // Chrome's pastel set, blended halfway toward the field's
        // Base so dark palettes darken the fills instead of glowing.
        const auto ringColor = [&pal](const QColor &chrome) {
            return AroraStyle::blend(pal.color(QPalette::Base),
                                     chrome, 0.55);
        };
        const struct { QRect r; QColor c; const char *name;
                       const QVector<double> *edges; } rings[] = {
            {marginR, ringColor(QColor(0xf6, 0xb1, 0x7c)), "margin",
             &m_margin},
            {borderR, ringColor(QColor(0xff, 0xe0, 0xa0)), "border",
             &m_border},
            {paddingR, ringColor(QColor(0xc8, 0xdf, 0xa4)), "padding",
             &m_padding},
        };
        for (const auto &ring : rings) {
            p.fillRect(ring.r, ring.c);
            const QColor ink = AroraStyle::contrastTextColor(ring.c);
            p.setPen(ink);
            // caption in the ring's top-left corner
            p.drawText(ring.r.adjusted(3, 1, 0, 0),
                       Qt::AlignLeft | Qt::AlignTop,
                       QLatin1String(ring.name));
            const QVector<double> &e = *ring.edges;
            const auto num = [](double v) {
                return v == std::floor(v)
                    ? QString::number(int(v))
                    : QString::number(v, 'f', 1);
            };
            // Edge numbers centered in each ring band — Chrome's
            // layout; the side bands draw only when the text fits.
            if (ring.edges->size() >= 4 && fm.height() <= t - 2) {
                const int cy = ring.r.center().y();
                p.drawText(QRect(ring.r.left() + 4, ring.r.top(),
                                 ring.r.width() - 8, t),
                           Qt::AlignCenter, num(e.value(0)));
                p.drawText(QRect(ring.r.left() + 4,
                                 ring.r.bottom() - t,
                                 ring.r.width() - 8, t),
                           Qt::AlignCenter, num(e.value(2)));
                if (fm.horizontalAdvance(num(e.value(1))) <= t - 4)
                    p.drawText(QRect(ring.r.right() - t,
                                     cy - fm.height() / 2,
                                     t, fm.height()),
                               Qt::AlignCenter, num(e.value(1)));
                if (fm.horizontalAdvance(num(e.value(3))) <= t - 4)
                    p.drawText(QRect(ring.r.left(),
                                     cy - fm.height() / 2,
                                     t, fm.height()),
                               Qt::AlignCenter, num(e.value(3)));
            }
        }
        p.fillRect(contentR,
                   ringColor(QColor(0x8b, 0xb3, 0xd9)));
        p.setPen(AroraStyle::contrastTextColor(
            ringColor(QColor(0x8b, 0xb3, 0xd9))));
        p.drawText(contentR, Qt::AlignCenter,
                   QStringLiteral("%1 × %2")
                       .arg(m_content.width(), 0, 'f',
                            m_content.width() == std::floor(
                                m_content.width()) ? 0 : 1)
                       .arg(m_content.height(), 0, 'f',
                            m_content.height() == std::floor(
                                m_content.height()) ? 0 : 1));
    }

private:
    QVector<double> m_margin, m_border, m_padding;
    QSizeF m_content;
    QString m_error;
};

class BidiNetworkModel : public QAbstractTableModel
{
public:
    enum Column {
        NameColumn = 0,
        StatusColumn,
        TypeColumn,
        InitiatorColumn,
        SizeColumn,
        TimeColumn,
        WaterfallColumn,
        ColumnCount
    };
    enum RowState { Pending = 0, Receiving, Finished, Failed };
    // DEVT07: how a response was served — the Size column shows the
    // "(disk cache)"-style label instead of a bogus 0-byte count.
    enum CacheKind {
        CacheNone = 0,
        CacheDisk,
        CacheMemory,
        CacheServiceWorker,
        CachePrefetch
    };
    // Raw-value roles — the proxy sorts/filters on these and the
    // waterfall delegate reads the timings.
    enum Role {
        StartRole = Qt::UserRole + 1,
        EndRole,
        SizeRole,
        StatusRole,
        BucketRole,
        StateRole,
        UrlRole,
        CacheRole,
        TtfbRole,
        DecodedRole,
        // DEVT11: the raw CDP type for the waterfall's per-type
        // swatch (TypeRole) — appended last so the earlier role
        // numbers the suite mirrors stay put.
        TypeRole
    };
    struct Row {
        QString requestId, url, name, type, initiator, errorText;
        // DEVT11: the raw CDP ResourceTiming object — the waterfall
        // tooltip's per-phase breakdown (dns/connect/ssl/send/
        // waiting/download) reads it; empty when the engine sent no
        // timing block.
        QJsonObject timing;
        int bucket = NetOther;
        int status = -1;
        int state = Pending;
        int cacheKind = CacheNone;
        qint64 size = -1;
        // DEVT07: uncompressed body bytes (the footer's "resources"
        // figure) and the cache-served flag the Size column marks
        // "(cached)" for instead of a misleading "0 B".
        qint64 decodedSize = -1;
        bool cached = false;
        double startMs = -1.0;
        double endMs = -1.0;
        // Time-to-first-byte as an offset from startMs (CDP
        // timing.receiveHeadersEnd) — splits the waterfall bar into
        // waiting/receiving segments; -1 when the engine didn't send
        // timing for the request.
        double ttfbMs = -1.0;
    };

    explicit BidiNetworkModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }
    int columnCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return QVariant();
        switch (section) {
        case NameColumn: return tr("Name");
        case StatusColumn: return tr("Status");
        case TypeColumn: return tr("Type");
        case InitiatorColumn: return tr("Initiator");
        case SizeColumn: return tr("Size");
        case TimeColumn: return tr("Time");
        case WaterfallColumn: return tr("Waterfall");
        }
        return QVariant();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return QVariant();
        const Row &row = m_rows.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case NameColumn:
                return row.name;
            case StatusColumn:
                if (row.status > 0)
                    return QString::number(row.status);
                if (row.state == Failed)
                    return row.errorText.isEmpty()
                        ? QStringLiteral("(failed)") : row.errorText;
                return QString();
            case TypeColumn:
                return row.type.isEmpty()
                    ? QStringLiteral("other") : row.type.toLower();
            case InitiatorColumn:
                return row.initiator;
            case SizeColumn:
                switch (row.cacheKind) {
                case CacheDisk: return tr("(disk cache)");
                case CacheMemory: return tr("(memory cache)");
                case CacheServiceWorker: return tr("(service worker)");
                case CachePrefetch: return tr("(prefetch cache)");
                default: break;
                }
                // Cache-served rows report 0 wire bytes — show the
                // honest marker like Chrome instead of "0 B".
                if (row.cached && row.size <= 0)
                    return tr("(cached)");
                return formatBytes(row.size);
            case TimeColumn:
                return row.endMs >= 0.0 && row.startMs >= 0.0
                    ? formatMillis(row.endMs - row.startMs)
                    : QStringLiteral("-");
            }
            return QString();
        case Qt::ToolTipRole:
            if (index.column() == NameColumn)
                return row.url;
            if (index.column() == StatusColumn && row.state == Failed)
                return row.errorText;
            if (index.column() == SizeColumn && row.size >= 0) {
                // Chrome's cell carries the wire size; the tooltip
                // adds the decoded resource size when the bridge
                // reported it.
                QString tip = tr("%1 transferred over network")
                    .arg(formatBytes(row.size));
                if (row.decodedSize >= 0 && row.decodedSize != row.size)
                    tip += tr(", resource size %1")
                        .arg(formatBytes(row.decodedSize));
                return tip;
            }
            if (index.column() == WaterfallColumn
                && row.startMs >= 0.0 && row.endMs >= 0.0) {
                const double total = row.endMs - row.startMs;
                // DEVT11: Chrome's hover breakdown — one line per
                // phase the raw CDP ResourceTiming actually carried.
                // "Queueing/Stalled" is honestly absent: it needs the
                // request's wall-clock issue time, which the
                // monotonic waterfall clock cannot reproduce.
                QStringList lines;
                const auto seg = [&lines, this](const QString &label,
                                                double start,
                                                double end) {
                    if (start >= 0.0 && end > start)
                        lines << label + QLatin1String(": ")
                                + formatMillis(end - start);
                };
                const QJsonObject &t = row.timing;
                seg(tr("Proxy negotiation"),
                    t[QStringLiteral("proxyStart")].toDouble(-1.0),
                    t[QStringLiteral("proxyEnd")].toDouble(-1.0));
                seg(tr("DNS Lookup"),
                    t[QStringLiteral("dnsStart")].toDouble(-1.0),
                    t[QStringLiteral("dnsEnd")].toDouble(-1.0));
                seg(tr("Initial connection"),
                    t[QStringLiteral("connectStart")].toDouble(-1.0),
                    t[QStringLiteral("connectEnd")].toDouble(-1.0));
                seg(tr("SSL"),
                    t[QStringLiteral("sslStart")].toDouble(-1.0),
                    t[QStringLiteral("sslEnd")].toDouble(-1.0));
                seg(tr("Request sent"),
                    t[QStringLiteral("sendStart")].toDouble(-1.0),
                    t[QStringLiteral("sendEnd")].toDouble(-1.0));
                const double ttfb =
                    t[QStringLiteral("receiveHeadersEnd")]
                        .toDouble(-1.0);
                const double sendEnd =
                    t[QStringLiteral("sendEnd")].toDouble(-1.0);
                if (ttfb >= 0.0) {
                    lines << tr("Waiting (TTFB): %1")
                        .arg(formatMillis(
                            sendEnd >= 0.0 ? ttfb - sendEnd : ttfb));
                    if (ttfb < total)
                        lines << tr("Content Download: %1")
                            .arg(formatMillis(total - ttfb));
                }
                if (!lines.isEmpty())
                    return lines.join(QLatin1Char('\n'));
                if (row.ttfbMs >= 0.0 && row.ttfbMs < total)
                    return tr("Wait %1 · Download %2")
                        .arg(formatMillis(row.ttfbMs),
                             formatMillis(total - row.ttfbMs));
                return formatMillis(total);
            }
            return QVariant();
        case Qt::ForegroundRole:
            if (row.state == Failed || row.status >= 400)
                return failColor();
            // DEVT11: cache-served rows read grayed like Chrome's —
            // the request hit no wire.
            if (row.cacheKind != CacheNone || row.cached)
                return qApp->palette().color(
                    QPalette::Disabled, QPalette::Text);
            return QVariant();
        case Qt::TextAlignmentRole:
            if (index.column() == StatusColumn
                || index.column() == SizeColumn
                || index.column() == TimeColumn)
                return int(Qt::AlignRight | Qt::AlignVCenter);
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        case StartRole: return row.startMs;
        case EndRole: return row.endMs;
        case SizeRole: return row.size;
        case StatusRole: return row.status;
        case BucketRole: return row.bucket;
        case StateRole: return row.state;
        case UrlRole: return row.url;
        case CacheRole: return row.cacheKind;
        case TtfbRole: return row.ttfbMs;
        case DecodedRole: return row.decodedSize;
        case TypeRole: return row.type;
        }
        return QVariant();
    }

    int addRequest(const Row &row)
    {
        // Same 1000-row cap as the old widget, enforced model-side —
        // rowsRemoved lets the panel rebase its requestId index.
        if (m_rows.size() >= 1000) {
            const int drop = m_rows.size() - 999;
            beginRemoveRows(QModelIndex(), 0, drop - 1);
            m_rows.remove(0, drop);
            endRemoveRows();
        }
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size());
        m_rows.append(row);
        endInsertRows();
        return m_rows.size() - 1;
    }

    Row *rowAt(int row)
    {
        return row >= 0 && row < m_rows.size() ? &m_rows[row] : nullptr;
    }

    void rowChanged(int row)
    {
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    }

    void clear()
    {
        m_lastTs = -1.0;
        if (m_rows.isEmpty())
            return;
        beginRemoveRows(QModelIndex(), 0, m_rows.size() - 1);
        m_rows.clear();
        endRemoveRows();
    }

    // Freshest CDP timestamp seen — lets a still-pending row's
    // waterfall bar keep growing as later events arrive (DEVT07).
    void noteTimestamp(double ts)
    {
        if (ts > m_lastTs)
            m_lastTs = ts;
    }

    // Waterfall window: earliest start .. latest end (pending rows
    // count at their start so the scale never runs backwards; the
    // latest event timestamp keeps in-flight bars honest).
    double windowStart() const
    {
        double v = -1.0;
        for (const Row &row : m_rows) {
            if (row.startMs >= 0.0)
                v = v < 0.0 ? row.startMs : qMin(v, row.startMs);
        }
        return v;
    }
    double windowEnd() const
    {
        double v = m_lastTs;
        for (const Row &row : m_rows)
            v = qMax(v, row.endMs >= 0.0 ? row.endMs : row.startMs);
        return v;
    }
    // DEVT09: the paint axis zooms into the dense part of the
    // recording.  The true window still drives the footer's Finish,
    // but the delegate scales to roughly the p90 completed end —
    // applied only when the true window is more than twice the dense
    // span, so an even spread keeps the honest full axis while a lone
    // slow request can no longer shrink every other bar to a
    // hairline.  (The spec's alternative — a fit-dense toggle — would
    // add a toolbar control; the percentile cap gives Chrome's
    // "readable by default" behavior with no UI and no state.)
    double displayWindowEnd() const
    {
        const double w0 = windowStart();
        const double full = windowEnd();
        if (w0 < 0.0 || full <= w0)
            return full;
        QVector<double> ends;
        ends.reserve(m_rows.size());
        for (const Row &row : m_rows) {
            if (row.endMs >= 0.0)
                ends.append(row.endMs);
        }
        if (ends.size() < 2)
            return full;
        std::sort(ends.begin(), ends.end());
        // Lower-rank p90: for tiny sets this deliberately lands below
        // the max so a single slow request still reads as an outlier.
        const double dense = ends.at((ends.size() - 1) * 9 / 10);
        if (dense > w0 && full - w0 > 2.0 * (dense - w0))
            return dense;
        return full;
    }

    // DEVT09: page-lifecycle markers on the request clock — the
    // delegate draws them as column-wide DCL/Load lines.  -1 unset.
    // They deliberately survive clear() like the panel's own fields
    // (the Clear button wipes rows, not the page's load timings).
    void setPageTimings(double dclMs, double loadMs)
    {
        if (m_dclMs == dclMs && m_loadMs == loadMs)
            return;
        m_dclMs = dclMs;
        m_loadMs = loadMs;
        if (!m_rows.isEmpty())
            emit dataChanged(index(0, WaterfallColumn),
                             index(m_rows.size() - 1, WaterfallColumn));
    }
    double domContentLoadedMs() const { return m_dclMs; }
    double loadMs() const { return m_loadMs; }

private:
    QVector<Row> m_rows;
    double m_lastTs = -1.0;
    double m_dclMs = -1.0;
    double m_loadMs = -1.0;
};

// DEVT07 — per-column floors: a narrow dock squeezes the elastic
// Name/Waterfall split but can never crush a column to zero; below
// the floors the table scrolls horizontally instead.
static const int kNetColumnMin[BidiNetworkModel::ColumnCount] = {
    90,   // Name
    48,   // Status
    56,   // Type
    80,   // Initiator
    64,   // Size
    56,   // Time
    130,  // Waterfall
};

class BidiNetworkProxy : public QSortFilterProxyModel
{
public:
    explicit BidiNetworkProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
    }

    void setNeedle(const QString &needle)
    {
        if (m_needle == needle)
            return;
        beginFilterChange();
        m_needle = needle;
        endFilterChange();
    }
    void setInverted(bool inverted)
    {
        if (m_inverted == inverted)
            return;
        beginFilterChange();
        m_inverted = inverted;
        endFilterChange();
    }
    void setBucket(int bucket)
    {
        if (m_bucket == bucket)
            return;
        beginFilterChange();
        m_bucket = bucket;
        endFilterChange();
    }

protected:
    bool filterAcceptsRow(int row,
                          const QModelIndex &parent) const override
    {
        const QModelIndex i = sourceModel()->index(
            row, BidiNetworkModel::NameColumn, parent);
        if (m_bucket != NetAll
            && i.data(BidiNetworkModel::BucketRole).toInt() != m_bucket)
            return false;
        if (m_needle.isEmpty())
            return true;
        const bool hit =
            i.data(BidiNetworkModel::UrlRole).toString()
                .contains(m_needle, Qt::CaseInsensitive)
            || i.data(Qt::DisplayRole).toString()
                   .contains(m_needle, Qt::CaseInsensitive);
        return m_inverted ? !hit : hit;
    }

    bool lessThan(const QModelIndex &left,
                  const QModelIndex &right) const override
    {
        switch (left.column()) {
        case BidiNetworkModel::StatusColumn:
            return left.data(BidiNetworkModel::StatusRole).toInt()
                < right.data(BidiNetworkModel::StatusRole).toInt();
        case BidiNetworkModel::SizeColumn:
            return left.data(BidiNetworkModel::SizeRole).toLongLong()
                < right.data(BidiNetworkModel::SizeRole).toLongLong();
        case BidiNetworkModel::TimeColumn:
            return duration(left) < duration(right);
        case BidiNetworkModel::WaterfallColumn:
            return left.data(BidiNetworkModel::StartRole).toDouble()
                < right.data(BidiNetworkModel::StartRole).toDouble();
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    static double duration(const QModelIndex &i)
    {
        const double s = i.data(BidiNetworkModel::StartRole).toDouble();
        const double e = i.data(BidiNetworkModel::EndRole).toDouble();
        return e >= 0.0 && s >= 0.0 ? e - s : -1.0;
    }

    QString m_needle;
    bool m_inverted = false;
    int m_bucket = NetAll;
};

// DEVT11: Chrome's waterfall ruler — a "nice" tick step on the
// 1/2/5 ladder picked so labels land roughly every 90 px of axis.
static double waterfallTickStep(double spanMs, int widthPx)
{
    if (spanMs <= 0.0 || widthPx <= 0)
        return -1.0;
    const double rawStep = spanMs * 90.0 / widthPx;
    static const double kSteps[] = {
        1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000,
        10000, 30000, 60000, 300000, 600000
    };
    for (double step : kSteps) {
        if (step >= rawStep)
            return step;
    }
    return rawStep;
}

// Tick positions (absolute ms) the ruler and the cell gridlines
// share — one ladder so a header tick always lands on a cell rule.
static QVector<double> waterfallTicks(double w0, double w1,
                                      int widthPx)
{
    QVector<double> ticks;
    const double step = waterfallTickStep(w1 - w0, widthPx);
    if (step <= 0.0)
        return ticks;
    for (double t = std::ceil(w0 / step) * step; t <= w1; t += step)
        ticks.append(t);
    return ticks;
}

// Paints the Chrome-style start+duration bar scaled to the dense
// display window (DEVT09 — one outlier request can no longer flatten
// every bar to a hairline); pending rows run to the window end,
// translucent, and bars clipped by the zoomed axis carry an overflow
// notch at the cell edge.
// DEVT07: when the response timing arrived the bar splits at
// time-to-first-byte — light segment waiting, solid receiving.
// DEVT11: the bar takes its resource type's swatch like Chrome
// (failed keeps the red), and faint tick gridlines run behind the
// bars on the same ladder as the header ruler.
class BidiWaterfallDelegate : public QStyledItemDelegate
{
public:
    // DEVT09: sub-pixel durations still get a readable swatch — the
    // bar may honestly overlap its neighbors like Chrome's does.
    static const int MinBarWidth = 5;

    explicit BidiWaterfallDelegate(BidiNetworkModel *model,
                                   QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_model(model)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!m_model)
            return;
        const double w0 = m_model->windowStart();
        const double w1 = m_model->displayWindowEnd();
        if (w0 < 0.0 || w1 <= w0)
            return;

        const QRect area = option.rect.adjusted(2, 3, -2, -3);
        const double span = w1 - w0;
        const int axisW = area.width() - 1;

        // DEVT11: the ruler's faint column-wide gridlines — the same
        // tick ladder the header paints, behind the bar.
        const QColor grid = AroraStyle::blend(
            option.palette.color(QPalette::Base),
            option.palette.color(QPalette::Text), 0.14);
        painter->setPen(grid);
        for (double t : waterfallTicks(w0, w1, area.width())) {
            const int gx = area.x() + int((t - w0) / span * axisW);
            painter->drawLine(gx, area.top(), gx, area.bottom());
        }

        // The bar.  A start mapped past the dense window's edge means
        // the request lives entirely outside the zoomed axis — paint
        // nothing rather than a phantom sliver at x 0.
        const double start =
            index.data(BidiNetworkModel::StartRole).toDouble();
        if (start >= w0 && start <= w1) {
            double end =
                index.data(BidiNetworkModel::EndRole).toDouble();
            const bool pending = end < 0.0;
            if (pending)
                end = w1;
            const bool clipped = !pending && end > w1;

            const int x = area.x()
                + int((start - w0) / span * axisW);
            const int width = qMin(
                qMax(MinBarWidth, int((end - start) / span * axisW)),
                area.right() - x + 1);
            if (width > 0) {
                const bool failed =
                    index.data(BidiNetworkModel::StateRole).toInt()
                    == BidiNetworkModel::Failed;
                // DEVT11: Chrome colors the bar by resource type;
                // failed rows stay red.  Typeless rows (the event
                // never named one) fall back to the neutral swatch.
                QColor bar = failed
                    ? failColor()
                    : AroraStyle::networkTypeSwatch(
                        index.data(BidiNetworkModel::TypeRole)
                            .toString(),
                        option.palette);
                if (pending)
                    bar.setAlpha(120);

                // Wait/receive split at TTFB, as a fraction of the
                // bar (DEVT07).
                const double ttfb =
                    index.data(BidiNetworkModel::TtfbRole).toDouble();
                int waitW = 0;
                if (ttfb > 0.0 && end - start > 0.0
                    && ttfb < end - start)
                    waitW = qMin(width - 1,
                                 qMax(1, int(ttfb / (end - start)
                                                 * width)));
                if (waitW > 0) {
                    QColor wait = bar;
                    wait.setAlpha(pending ? 60 : 80);
                    painter->fillRect(
                        QRect(x, area.y(), waitW, area.height()),
                        wait);
                }
                painter->fillRect(
                    QRect(x + waitW, area.y(), width - waitW,
                          area.height()),
                    bar);
                // A 1px darker edge keeps the swatch legible on light
                // themes where Highlight sits close to Base.
                const QColor edge = bar.darker(140);
                painter->setPen(edge);
                painter->drawRect(
                    QRect(x, area.y(), width, area.height())
                        .adjusted(0, 0, -1, -1));
                if (clipped) {
                    // Overflow notch: a right-pointing tick at the
                    // cell edge — the request continues past the
                    // zoomed window (the footer's Finish still
                    // reports the true full span).
                    const int cy = area.center().y();
                    QPolygonF notch;
                    notch << QPointF(area.right() - 5, cy - 4)
                          << QPointF(area.right(), cy)
                          << QPointF(area.right() - 5, cy + 4);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(edge);
                    painter->drawPolygon(notch);
                }
            }
        }

        // DEVT09: page-lifecycle markers — every cell draws its slice
        // of the line so they read as Chrome's column-wide
        // DOMContentLoaded/Load rules, without a second paint pass.
        const auto markerAt = [&](double ms, const QColor &color) {
            if (ms < w0 || ms > w1)
                return;
            const int mx =
                area.x() + int((ms - w0) / span * axisW);
            painter->fillRect(QRect(mx, area.y(), 1, area.height()),
                              color);
        };
        markerAt(m_model->domContentLoadedMs(),
                 option.palette.color(QPalette::Link));
        markerAt(m_model->loadMs(), failColor());
    }

private:
    BidiNetworkModel *m_model;
};

// DEVT11: Chrome's waterfall ruler — the Waterfall column's header
// section carries the ms tick ladder ("200 ms", "1.00 s") instead of
// a text label, drawn on the same scale the delegate uses so a tick
// always lands on a cell gridline.  Every other section paints
// stock.
class BidiNetworkHeader : public QHeaderView
{
public:
    explicit BidiNetworkHeader(BidiNetworkModel *model,
                               QWidget *parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent)
        , m_model(model)
    {
    }

protected:
    void paintSection(QPainter *painter, const QRect &rect,
                      int logicalIndex) const override
    {
        if (logicalIndex != BidiNetworkModel::WaterfallColumn) {
            QHeaderView::paintSection(painter, rect, logicalIndex);
            return;
        }
        // Stock section chrome, minus the "Waterfall" label — the
        // ruler owns the space like Chrome's does.
        QStyleOptionHeader opt;
        initStyleOptionForIndex(&opt, logicalIndex);
        opt.text.clear();
        style()->drawControl(QStyle::CE_Header, &opt, painter, this);
        if (!m_model)
            return;
        const double w0 = m_model->windowStart();
        const double w1 = m_model->displayWindowEnd();
        if (w0 < 0.0 || w1 <= w0)
            return;
        const double span = w1 - w0;
        const QRect area = rect.adjusted(2, 0, -2, 0);
        const int axisW = area.width() - 1;
        painter->save();
        painter->setClipRect(rect.adjusted(0, 0, -1, 0));
        QFont small = font();
        small.setPointSizeF(qMax(6.0, small.pointSizeF() - 1.0));
        painter->setFont(small);
        painter->setPen(palette().color(
            QPalette::Disabled, QPalette::WindowText));
        for (double t : waterfallTicks(w0, w1, area.width())) {
            const int x = area.x() + int((t - w0) / span * axisW);
            painter->drawLine(x, area.bottom() - 3, x, area.bottom());
            painter->drawText(
                QRect(x + 3, rect.top(), 70, rect.height()),
                int(Qt::AlignVCenter | Qt::AlignLeft),
                formatMillis(t - w0));
        }
        painter->restore();
    }

private:
    BidiNetworkModel *m_model;
};

BidiPanel::BidiPanel(QWidget *parent)
    : QWidget(parent)
    , m_client(new BidiClient(this))
    , m_nextTag(10)
    , m_loadMs(-1.0)
{
    // SIDE05: plain dock content — the surrounding m_bidiPanelDock
    // owns the "bidiPanelDock" objectName, the title bar, and the
    // allowed areas.
    setObjectName(QLatin1String("bidiPanel"));

    m_stack = new QStackedWidget(this);

    // --- unavailable page ------------------------------------------
    QWidget *unavail = new QWidget;
    QVBoxLayout *uv = new QVBoxLayout(unavail);
    m_unavailableLabel = new QLabel;
    m_unavailableLabel->setWordWrap(true);
    m_unavailableLabel->setAlignment(Qt::AlignCenter);
    uv->addStretch();
    uv->addWidget(m_unavailableLabel);
    uv->addStretch();
    m_stack->addWidget(unavail);

    // --- tabs page --------------------------------------------------
    QWidget *tabsPage = new QWidget;
    QVBoxLayout *tv = new QVBoxLayout(tabsPage);
    tv->setContentsMargins(2, 2, 2, 2);
    m_contextCombo = new QComboBox;
    m_contextCombo->setObjectName(QLatin1String("bidiContextCombo"));
    tv->addWidget(m_contextCombo);
    m_tabs = new QTabWidget;
    QTabWidget *tabs = m_tabs;
    tv->addWidget(tabs);
    m_stack->addWidget(tabsPage);

    // DEVT13 — Elements: the old DOM tab grew Chrome's chrome.  The
    // toolbar carries click-to-inspect and refresh; the right pane
    // shows computed styles (or the DOM-derived fallback); Chrome's
    // breadcrumb trail runs along the bottom edge.  DEVT22 reshaped
    // the page to Chrome's silhouette: a compact bordered toolbar
    // (buttons left, live element count right), a markup-painting
    // DOM tree, the grouped styles rail with a real box-model
    // diagram, and a find dock with a results list.
    QWidget *elementsPage = new QWidget;
    QVBoxLayout *elv = new QVBoxLayout(elementsPage);
    elv->setContentsMargins(0, 0, 0, 0);
    elv->setSpacing(0);
    QWidget *elBar = new QWidget;
    elBar->setObjectName(QLatin1String("bidiElementsToolbar"));
    // DEVT22: a visibly delimited compact bar, not buttons floating
    // in an unbounded strip — fixed height, hairline under-edge.
    elBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    elBar->setStyleSheet(QStringLiteral(
        "#bidiElementsToolbar {"
        "border-bottom: 1px solid palette(mid);"
        "background: palette(window);}"));
    QHBoxLayout *elb = new QHBoxLayout(elBar);
    elb->setContentsMargins(4, 2, 4, 2);
    elb->setSpacing(4);
    m_inspectToggle = new QToolButton;
    m_inspectToggle->setObjectName(QLatin1String("bidiElementsInspect"));
    m_inspectToggle->setAutoRaise(true);
    m_inspectToggle->setCheckable(true);
    m_inspectToggle->setIcon(inspectPickerIcon(palette()));
    m_inspectToggle->setToolTip(
        tr("Select an element in the page to inspect it"));
    elb->addWidget(m_inspectToggle);
    m_domRefresh = new QToolButton;
    m_domRefresh->setObjectName(QLatin1String("bidiElementsRefresh"));
    m_domRefresh->setAutoRaise(true);
    m_domRefresh->setIcon(
        style()->standardIcon(QStyle::SP_BrowserReload));
    m_domRefresh->setToolTip(tr("Rebuild the element tree"));
    elb->addWidget(m_domRefresh);
    // DEVT19: Chrome's Ctrl-F find — the toggle opens the find bar
    // under the tree.
    QToolButton *domSearchToggle = new QToolButton;
    domSearchToggle->setObjectName(
        QLatin1String("bidiElementsSearchToggle"));
    domSearchToggle->setAutoRaise(true);
    domSearchToggle->setIcon(
        AroraIcon::getChrome(QLatin1String("edit-find")));
    domSearchToggle->setToolTip(
        tr("Find an element by string or selector (Ctrl+F)"));
    elb->addWidget(domSearchToggle);
    elb->addStretch();
    // The right side of the bar reports the materialized tree size —
    // the count Chrome's status surface implies, and what fills the
    // otherwise dead space.
    m_domStats = new QLabel;
    m_domStats->setObjectName(QLatin1String("bidiElementsStats"));
    m_domStats->setStyleSheet(QStringLiteral(
        "color: palette(disabled-text);"));
    elb->addWidget(m_domStats);
    elv->addWidget(elBar);
    QSplitter *elSplit = new QSplitter(Qt::Horizontal);
    m_domTree = new QTreeWidget;
    m_domTree->setObjectName(QLatin1String("bidiElementsTree"));
    m_domTree->setHeaderLabel(tr("DOM"));
    m_domTree->setRootIsDecorated(true);
    m_domTree->setIndentation(14);
    m_domTree->setItemDelegate(new BidiDomDelegate(m_domTree));
    connect(m_domTree, &QTreeWidget::itemExpanded,
            this, &BidiPanel::domItemExpanded);
    elSplit->addWidget(m_domTree);
    QWidget *stylesPane = new QWidget;
    QVBoxLayout *svl = new QVBoxLayout(stylesPane);
    svl->setContentsMargins(6, 4, 4, 2);
    m_stylesTitle = new QLabel(tr("Computed styles"));
    m_stylesTitle->setObjectName(
        QLatin1String("bidiElementsStylesTitle"));
    svl->addWidget(m_stylesTitle);
    m_stylesView = new QTreeWidget;
    m_stylesView->setObjectName(QLatin1String("bidiElementsStyles"));
    m_stylesView->setColumnCount(2);
    m_stylesView->setHeaderLabels({tr("Property"), tr("Value")});
    // DEVT19: the pane is grouped now — Attributes / Computed
    // styles / Box model — so the decoration comes back, and
    // attribute rows open for editing on double-click.
    m_stylesView->setRootIsDecorated(true);
    m_stylesView->setItemsExpandable(true);
    m_stylesView->setEditTriggers(
        QAbstractItemView::DoubleClicked
        | QAbstractItemView::EditKeyPressed);
    m_stylesView->header()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_stylesView->header()->setStretchLastSection(true);
    svl->addWidget(m_stylesView);
    elSplit->addWidget(stylesPane);
    elSplit->setStretchFactor(0, 3);
    elSplit->setStretchFactor(1, 2);
    elv->addWidget(elSplit);
    // DEVT19: Chrome's find bar — query field, result count, prev /
    // next, hidden until Ctrl+F or the toolbar toggle.  DEVT22 adds
    // the results list under the row: every hit's markup line,
    // clicking one selects the node in the tree.
    m_domFindBar = new QWidget;
    m_domFindBar->setObjectName(QLatin1String("bidiElementsFindBar"));
    m_domFindBar->setStyleSheet(QStringLiteral(
        "#bidiElementsFindBar {"
        "border-top: 1px solid palette(mid);}"));
    QVBoxLayout *fbv = new QVBoxLayout(m_domFindBar);
    fbv->setContentsMargins(4, 2, 4, 2);
    fbv->setSpacing(2);
    QWidget *findRow = new QWidget;
    QHBoxLayout *fb = new QHBoxLayout(findRow);
    fb->setContentsMargins(0, 0, 0, 0);
    fb->setSpacing(4);
    m_domFind = new QLineEdit;
    m_domFind->setObjectName(QLatin1String("bidiElementsFind"));
    m_domFind->setPlaceholderText(
        tr("Find by string or selector"));
    m_domFind->setClearButtonEnabled(true);
    fb->addWidget(m_domFind, 1);
    m_domFindCount = new QLabel;
    m_domFindCount->setObjectName(
        QLatin1String("bidiElementsFindCount"));
    fb->addWidget(m_domFindCount);
    m_domFindPrev = new QToolButton;
    m_domFindPrev->setObjectName(QLatin1String("bidiElementsFindPrev"));
    m_domFindPrev->setAutoRaise(true);
    m_domFindPrev->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_domFindPrev->setToolTip(tr("Previous match"));
    fb->addWidget(m_domFindPrev);
    m_domFindNext = new QToolButton;
    m_domFindNext->setObjectName(QLatin1String("bidiElementsFindNext"));
    m_domFindNext->setAutoRaise(true);
    m_domFindNext->setIcon(
        style()->standardIcon(QStyle::SP_ArrowDown));
    m_domFindNext->setToolTip(tr("Next match"));
    fb->addWidget(m_domFindNext);
    fbv->addWidget(findRow);
    m_domFindResults = new QListWidget;
    m_domFindResults->setObjectName(
        QLatin1String("bidiElementsFindResults"));
    m_domFindResults->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    m_domFindResults->setUniformItemSizes(true);
    m_domFindResults->hide();
    fbv->addWidget(m_domFindResults);
    m_domFindBar->hide();
    elv->addWidget(m_domFindBar);
    m_crumbBar = new QWidget;
    m_crumbBar->setObjectName(QLatin1String("bidiElementsCrumbs"));
    m_crumbBar->setStyleSheet(QStringLiteral(
        "#bidiElementsCrumbs {"
        "border-top: 1px solid palette(mid);}"));
    new FlowLayout(m_crumbBar, 0, 4, 0);
    elv->addWidget(m_crumbBar);
    tabs->addTab(elementsPage, tr("Elements"));

    // Console — DEVT12: Chrome's shape.  A toolbar row (clear, text
    // filter, checkable "Default levels" dropdown, console-scoped
    // Preserve log), the structured entry list, then the REPL under
    // a '>' prompt row.
    QWidget *consolePage = new QWidget;
    QVBoxLayout *cv = new QVBoxLayout(consolePage);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);

    QWidget *consoleBar = new QWidget;
    consoleBar->setObjectName(QLatin1String("bidiConsoleToolbar"));
    QHBoxLayout *cb = new QHBoxLayout(consoleBar);
    cb->setContentsMargins(4, 2, 4, 2);
    cb->setSpacing(4);
    m_clearConsole = new QToolButton;
    m_clearConsole->setObjectName(QLatin1String("bidiConsoleClear"));
    m_clearConsole->setAutoRaise(true);
    m_clearConsole->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearConsole->setToolTip(tr("Clear console"));
    cb->addWidget(m_clearConsole);
    m_consoleFilter = new QLineEdit;
    m_consoleFilter->setObjectName(QLatin1String("bidiConsoleFilter"));
    m_consoleFilter->setClearButtonEnabled(true);
    m_consoleFilter->setPlaceholderText(tr("Filter"));
    m_consoleFilter->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Fixed);
    cb->addWidget(m_consoleFilter);
    // Chrome's "Default levels" dropdown — the four checkable level
    // buckets in Chrome's order; the button text reports the set
    // (All/Default/Custom/No levels).
    m_consoleLevels = new QToolButton;
    m_consoleLevels->setObjectName(QLatin1String("bidiConsoleLevels"));
    m_consoleLevels->setAutoRaise(true);
    m_consoleLevels->setPopupMode(QToolButton::InstantPopup);
    m_consoleLevelMask = kDefaultConsoleLevels;
    {
        QMenu *levelsMenu = new QMenu(m_consoleLevels);
        static const struct { const char *label; int level; }
            kLevels[] = {
                {"Verbose", LevelVerbose}, {"Info", LevelInfo},
                {"Warnings", LevelWarning}, {"Errors", LevelError},
            };
        for (const auto &entry : kLevels) {
            QAction *action =
                levelsMenu->addAction(tr(entry.label));
            action->setCheckable(true);
            action->setChecked(entry.level != LevelVerbose);
            action->setData(entry.level);
            connect(action, &QAction::toggled,
                    this, [this](bool) { updateConsoleLevelMask(); });
        }
        m_consoleLevels->setMenu(levelsMenu);
    }
    m_consoleLevels->setText(tr("Default levels"));
    m_consoleLevels->setToolTip(
        tr("Which console message levels are shown"));
    cb->addWidget(m_consoleLevels);
    m_consolePreserve = new QCheckBox(tr("Preserve log"));
    m_consolePreserve->setObjectName(
        QLatin1String("bidiConsolePreserve"));
    m_consolePreserve->setToolTip(
        tr("Do not clear the console on navigation"));
    cb->addWidget(m_consolePreserve);
    cv->addWidget(consoleBar);

    // Three header-less columns: message | repeat badge | source.
    m_console = new QTreeWidget;
    m_console->setObjectName(QLatin1String("bidiConsoleView"));
    m_console->setColumnCount(3);
    m_console->setHeaderHidden(true);
    m_console->setRootIsDecorated(true);
    m_console->setUniformRowHeights(false);
    m_console->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_console->setSelectionMode(QAbstractItemView::NoSelection);
    m_console->header()->setStretchLastSection(false);
    m_console->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_console->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_console->header()->resizeSection(1, 32);
    m_console->header()->setSectionResizeMode(
        2, QHeaderView::Interactive);
    m_console->header()->resizeSection(2, 160);
    cv->addWidget(m_console);

    // The REPL sits on a '>' prompt row like Chrome's bottom line;
    // Up/Down in the field recalls history (eventFilter below).
    QWidget *prompt = new QWidget;
    prompt->setObjectName(QLatin1String("bidiConsolePrompt"));
    prompt->setStyleSheet(QStringLiteral(
        "border-top: 1px solid palette(mid);"));
    QHBoxLayout *pr = new QHBoxLayout(prompt);
    pr->setContentsMargins(8, 1, 4, 1);
    pr->setSpacing(4);
    QLabel *glyph = new QLabel(QStringLiteral(">"));
    glyph->setObjectName(QLatin1String("bidiConsolePromptGlyph"));
    {
        QPalette dim = glyph->palette();
        dim.setColor(QPalette::WindowText,
                     dim.color(QPalette::Disabled,
                               QPalette::WindowText));
        glyph->setPalette(dim);
    }
    pr->addWidget(glyph);
    m_evalLine = new QLineEdit;
    m_evalLine->setFrame(false);
    m_evalHistoryIndex = -1;
    m_evalLine->setPlaceholderText(
        tr("Evaluate JavaScript in the selected context"));
    m_evalLine->installEventFilter(this);
    connect(m_evalLine, &QLineEdit::returnPressed,
            this, &BidiPanel::evaluateInput);
    pr->addWidget(m_evalLine);
    cv->addWidget(prompt);
    tabs->addTab(consolePage, tr("Console"));

    // DEVT13 — Sources: the page's resource tree (Page.getResource-
    // Tree) on the left, the read-only body (Page.getResourceContent)
    // in a monospace view, and a breakpoint row that only lights up
    // when the Debugger domain's probe came back clean.
    QWidget *sourcesPage = new QWidget;
    sourcesPage->setObjectName(QLatin1String("bidiSourcesPage"));
    QVBoxLayout *srcv = new QVBoxLayout(sourcesPage);
    srcv->setContentsMargins(0, 0, 0, 0);
    srcv->setSpacing(0);
    QSplitter *srcSplit = new QSplitter(Qt::Horizontal);
    m_sourcesTree = new QTreeWidget;
    m_sourcesTree->setObjectName(QLatin1String("bidiSourcesTree"));
    m_sourcesTree->setHeaderLabel(tr("Resources"));
    m_sourcesTree->setRootIsDecorated(true);
    srcSplit->addWidget(m_sourcesTree);
    QWidget *srcRight = new QWidget;
    QVBoxLayout *srv = new QVBoxLayout(srcRight);
    srv->setContentsMargins(0, 0, 0, 0);
    srv->setSpacing(0);
    // DEVT20: Chrome's debugger toolbar heads the right pane —
    // pause/resume/step buttons plus the paused-state line.  The
    // buttons map 1:1 onto the Debugger.* commands; enablement
    // tracks the probe verdict (updateDebuggerButtons).
    m_dbgBar = new QWidget;
    m_dbgBar->setObjectName(QLatin1String("bidiSourcesDbgBar"));
    QHBoxLayout *dbl = new QHBoxLayout(m_dbgBar);
    dbl->setContentsMargins(4, 2, 4, 2);
    dbl->setSpacing(4);
    const auto dbgButton = [this, dbl](
            QToolButton *&slot, QStyle::StandardPixmap icon,
            const QString &name, const QString &tip,
            const QString &method) {
        slot = new QToolButton;
        slot->setObjectName(name);
        slot->setAutoRaise(true);
        slot->setIcon(style()->standardIcon(icon));
        slot->setToolTip(tip);
        dbl->addWidget(slot);
        connect(slot, &QToolButton::clicked, this,
                [this, method]() { sendDebuggerCommand(method); });
    };
    dbgButton(m_dbgPause, QStyle::SP_MediaPause,
              QStringLiteral("bidiDbgPause"),
              tr("Pause script execution"),
              QStringLiteral("Debugger.pause"));
    dbgButton(m_dbgResume, QStyle::SP_MediaPlay,
              QStringLiteral("bidiDbgResume"),
              tr("Resume script execution"),
              QStringLiteral("Debugger.resume"));
    dbgButton(m_dbgStepOver, QStyle::SP_MediaSkipForward,
              QStringLiteral("bidiDbgStepOver"),
              tr("Step over the next call"),
              QStringLiteral("Debugger.stepOver"));
    dbgButton(m_dbgStepInto, QStyle::SP_ArrowDown,
              QStringLiteral("bidiDbgStepInto"),
              tr("Step into the next call"),
              QStringLiteral("Debugger.stepInto"));
    dbgButton(m_dbgStepOut, QStyle::SP_ArrowUp,
              QStringLiteral("bidiDbgStepOut"),
              tr("Step out of the current function"),
              QStringLiteral("Debugger.stepOut"));
    m_pauseStatus = new QLabel;
    m_pauseStatus->setObjectName(QLatin1String("bidiDbgStatus"));
    m_pauseStatus->setVisible(false);
    dbl->addWidget(m_pauseStatus, 1);
    srv->addWidget(m_dbgBar);
    m_sourceView = new QPlainTextEdit;
    m_sourceView->setObjectName(QLatin1String("bidiSourceView"));
    m_sourceView->setReadOnly(true);
    m_sourceView->setFont(QFontDatabase::systemFont(
        QFontDatabase::FixedFont));
    srv->addWidget(m_sourceView, 1);
    // Chrome's gutter breakpoints compress to an explicit row here —
    // a QPlainTextEdit has no gutter to click.  Lines are 1-based
    // like Chrome's gutter; toggleBreakpoint maps down to CDP's
    // 0-based lineNumber.
    m_bpBar = new QWidget;
    m_bpBar->setObjectName(QLatin1String("bidiSourcesBpBar"));
    QHBoxLayout *bpl = new QHBoxLayout(m_bpBar);
    bpl->setContentsMargins(4, 2, 4, 2);
    bpl->setSpacing(4);
    bpl->addWidget(new QLabel(tr("Breakpoint at line")));
    m_bpLine = new QSpinBox;
    m_bpLine->setObjectName(QLatin1String("bidiSourcesBpLine"));
    m_bpLine->setRange(1, 9999999);
    bpl->addWidget(m_bpLine);
    QToolButton *bpSet = new QToolButton;
    bpSet->setObjectName(QLatin1String("bidiSourcesBpSet"));
    bpSet->setAutoRaise(true);
    bpSet->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    bpSet->setToolTip(tr("Set a breakpoint on this line of the "
                         "displayed resource"));
    bpl->addWidget(bpSet);
    bpl->addStretch();
    srv->addWidget(m_bpBar);
    m_bpList = new QListWidget;
    m_bpList->setObjectName(QLatin1String("bidiSourcesBpList"));
    m_bpList->setMaximumHeight(90);
    m_bpList->setToolTip(
        tr("Active breakpoints — double-click to remove"));
    srv->addWidget(m_bpList);
    // DEVT20: Chrome's Call stack sidebar compresses to a list under
    // the breakpoints — filled while the debugger is paused.
    QLabel *callLabel = new QLabel(tr("Call stack"));
    srv->addWidget(callLabel);
    m_callFrames = new QListWidget;
    m_callFrames->setObjectName(
        QLatin1String("bidiSourcesCallStack"));
    m_callFrames->setMaximumHeight(90);
    m_callFrames->setToolTip(
        tr("Call stack while the debugger is paused"));
    srv->addWidget(m_callFrames);
    srcSplit->addWidget(srcRight);
    srcSplit->setStretchFactor(0, 1);
    srcSplit->setStretchFactor(1, 3);
    srcv->addWidget(srcSplit);
    tabs->addTab(sourcesPage, tr("Sources"));
    connect(m_sourcesTree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
        sourceSelected(cur);
    });
    connect(bpSet, &QToolButton::clicked,
            this, &BidiPanel::toggleBreakpoint);
    connect(m_bpList, &QListWidget::itemActivated,
            this, [this](QListWidgetItem *item) {
        // Double-click removes — the smallest honest breakpoint UI.
        const QString bpId =
            item->data(Qt::UserRole).toString();
        const QString ctx = currentContext();
        if (!bpId.isEmpty() && !ctx.isEmpty()) {
            sendCdp(ctx, QStringLiteral("Debugger.removeBreakpoint"),
                    QJsonObject{{QStringLiteral("breakpointId"), bpId}},
                    CdpBpRemove, bpId);
        }
    });

    // Network — DEVT06 Chrome-style: toolbar row, type chips, table.
    QWidget *networkPage = new QWidget;
    QVBoxLayout *nv = new QVBoxLayout(networkPage);
    nv->setContentsMargins(0, 0, 0, 0);
    nv->setSpacing(0);

    QWidget *netBar = new QWidget;
    netBar->setObjectName(QLatin1String("bidiNetworkToolbar"));
    QHBoxLayout *nb = new QHBoxLayout(netBar);
    nb->setContentsMargins(4, 2, 4, 2);
    nb->setSpacing(4);
    // DEVT11: Chrome's row — the record/stop toggle and the clear
    // button lead on the left, the filter field stretches, the
    // checkboxes trail, and the throttling + response-search
    // controls sit at the right edge honestly disabled (the debug
    // channel exposes neither).
    m_recordToggle = new QToolButton;
    m_recordToggle->setObjectName(QLatin1String("bidiNetworkRecord"));
    m_recordToggle->setAutoRaise(true);
    m_recordToggle->setCheckable(true);
    m_recordToggle->setChecked(true);
    m_recordToggle->setIcon(recordDotIcon(true, palette()));
    m_recordToggle->setToolTip(tr("Stop recording network log"));
    nb->addWidget(m_recordToggle);
    m_clearRequests = new QToolButton;
    m_clearRequests->setAutoRaise(true);
    m_clearRequests->setIcon(
        style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearRequests->setToolTip(tr("Clear"));
    nb->addWidget(m_clearRequests);
    m_networkFilter = new QLineEdit;
    m_networkFilter->setClearButtonEnabled(true);
    m_networkFilter->setPlaceholderText(tr("Filter"));
    m_networkFilter->setMaximumWidth(200);
    m_networkFilter->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Fixed);
    nb->addWidget(m_networkFilter);
    nb->addStretch();
    m_invertFilter = new QCheckBox(tr("Invert"));
    nb->addWidget(m_invertFilter);
    m_preserveLog = new QCheckBox(tr("Preserve log"));
    m_preserveLog->setToolTip(
        tr("Do not clear the request list on navigation"));
    nb->addWidget(m_preserveLog);
    m_disableCache = new QCheckBox(tr("Disable cache"));
    m_disableCache->setToolTip(
        tr("Bypass the HTTP cache while the panel is connected"));
    nb->addWidget(m_disableCache);
    // Throttling: Chromium's CDP speaks
    // Network.emulateNetworkConditions but the bridge has no BiDi
    // translation for it — the selector shows the slot with the
    // reason rather than a dead combo or a fake option list.
    m_throttling = new QComboBox;
    m_throttling->setObjectName(QLatin1String("bidiNetworkThrottling"));
    m_throttling->addItem(tr("No throttling"));
    m_throttling->setEnabled(false);
    m_throttling->setToolTip(
        tr("Network throttling is not exposed by the debug channel"));
    nb->addWidget(m_throttling);
    // Response search: bodies never reach the panel (a DEVT06 scope
    // call — the bridge forwards no getResponseBody equivalent).
    m_responseSearch = new QToolButton;
    m_responseSearch->setObjectName(
        QLatin1String("bidiNetworkSearchResponses"));
    m_responseSearch->setAutoRaise(true);
    m_responseSearch->setIcon(
        AroraIcon::getChrome(QLatin1String("edit-find")));
    m_responseSearch->setEnabled(false);
    m_responseSearch->setToolTip(
        tr("Search response bodies — unavailable: the debug channel "
           "does not expose response bodies"));
    nb->addWidget(m_responseSearch);
    nv->addWidget(netBar);

    // Type chips — an exclusive filtered view over the same rows.
    // FlowLayout wraps the row when the dock is narrow.
    QWidget *chips = new QWidget;
    chips->setObjectName(QLatin1String("bidiNetworkChips"));
    FlowLayout *chipFlow = new FlowLayout(chips, 0, 2, 0);
    m_typeChips = new QButtonGroup(chips);
    m_typeChips->setExclusive(true);
    static const struct { const char *label; int bucket; } kChips[] = {
        {"All", NetAll}, {"Fetch/XHR", NetFetchXhr}, {"Doc", NetDoc},
        {"CSS", NetCss}, {"JS", NetJs}, {"Font", NetFont},
        {"Img", NetImg}, {"Media", NetMedia}, {"Manifest", NetManifest},
        {"Socket", NetSocket}, {"Wasm", NetWasm}, {"Other", NetOther},
    };
    for (const auto &chip : kChips) {
        QToolButton *button = new QToolButton;
        button->setText(tr(chip.label));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        m_typeChips->addButton(button, chip.bucket);
        chipFlow->addWidget(button);
    }
    m_typeChips->button(NetAll)->setChecked(true);
    nv->addWidget(chips);

    m_networkModel = new BidiNetworkModel(this);
    m_networkProxy = new BidiNetworkProxy(this);
    m_networkProxy->setSourceModel(m_networkModel);
    m_networkView = new QTableView;
    m_networkView->setObjectName(QLatin1String("bidiNetworkView"));
    m_networkView->setModel(m_networkProxy);
    m_networkView->setShowGrid(false);
    m_networkView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_networkView->setSortingEnabled(true);
    m_networkView->verticalHeader()->setVisible(false);
    m_networkView->verticalHeader()->setDefaultSectionSize(
        m_networkView->fontMetrics().height() + 8);
    // DEVT11: the Waterfall section's header is the ms ruler — the
    // custom header paints every other section stock.
    m_networkView->setHorizontalHeader(
        new BidiNetworkHeader(
            static_cast<BidiNetworkModel *>(m_networkModel),
            m_networkView));
    QHeaderView *netHead = m_networkView->horizontalHeader();
    netHead->setSectionResizeMode(QHeaderView::Interactive);
    // DEVT07: every column stays Interactive (user-resizable) with a
    // real floor — see kNetColumnMin.  Name and Waterfall split the
    // elastic space on each viewport resize (fitNetworkColumns); the
    // numeric columns keep their fixed-fit widths and below the
    // floors the table scrolls horizontally rather than losing a
    // column outright.
    static const int kNetColumnWidth[BidiNetworkModel::ColumnCount] = {
        240, 64, 80, 140, 88, 80, 220
    };
    for (int c = 0; c < BidiNetworkModel::ColumnCount; ++c)
        netHead->resizeSection(c, kNetColumnWidth[c]);
    netHead->setStretchLastSection(false);
    netHead->setMinimumSectionSize(50);
    m_networkView->setHorizontalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    m_networkView->viewport()->installEventFilter(this);
    connect(netHead, &QHeaderView::sectionResized,
            this, [this](int logical, int, int newSize) {
        if (m_fittingColumns || logical < 0
            || logical >= BidiNetworkModel::ColumnCount
            || newSize >= kNetColumnMin[logical])
            return;
        // A user drag can't push a column under its floor.
        m_fittingColumns = true;
        m_networkView->horizontalHeader()
            ->resizeSection(logical, kNetColumnMin[logical]);
        m_fittingColumns = false;
    });
    m_networkView->setItemDelegateForColumn(
        BidiNetworkModel::WaterfallColumn,
        new BidiWaterfallDelegate(
            static_cast<BidiNetworkModel *>(m_networkModel),
            m_networkView));
    nv->addWidget(m_networkView);
    // DEVT07: Chrome's bottom line — "N requests · X kB transferred ·
    // X kB resources · Finish: T · DOMContentLoaded: T · Load: T",
    // tracking the filtered row set.
    m_networkSummary = new QLabel;
    m_networkSummary->setObjectName(QLatin1String("bidiNetworkSummary"));
    m_networkSummary->setStyleSheet(QStringLiteral(
        "border-top: 1px solid palette(mid); padding: 2px 6px;"));
    m_networkSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nv->addWidget(m_networkSummary);
    tabs->addTab(networkPage, tr("Network"));

    // DEVT13 — Performance: live Performance.getMetrics plus a real
    // Profiler.start/stop CPU capture summarized top-self-first.
    // When either domain's probe comes back down the body disables
    // and m_perfOffline carries the reason — never a fake.
    QWidget *perfPage = new QWidget;
    perfPage->setObjectName(QLatin1String("bidiPerfPage"));
    QVBoxLayout *pv = new QVBoxLayout(perfPage);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(0);
    QWidget *perfBar = new QWidget;
    perfBar->setObjectName(QLatin1String("bidiPerfToolbar"));
    QHBoxLayout *pb = new QHBoxLayout(perfBar);
    pb->setContentsMargins(4, 2, 4, 2);
    pb->setSpacing(4);
    m_cpuToggle = new QToolButton;
    m_cpuToggle->setObjectName(QLatin1String("bidiPerfRecord"));
    m_cpuToggle->setAutoRaise(true);
    m_cpuToggle->setCheckable(true);
    m_cpuToggle->setIcon(recordDotIcon(false, palette()));
    m_cpuToggle->setToolTip(tr("Record a CPU profile"));
    pb->addWidget(m_cpuToggle);
    QToolButton *perfRefresh = new QToolButton;
    perfRefresh->setAutoRaise(true);
    perfRefresh->setIcon(
        style()->standardIcon(QStyle::SP_BrowserReload));
    perfRefresh->setToolTip(tr("Refresh metrics now"));
    pb->addWidget(perfRefresh);
    m_perfStatus = new QLabel;
    m_perfStatus->setObjectName(QLatin1String("bidiPerfStatus"));
    pb->addWidget(m_perfStatus);
    pb->addStretch();
    pv->addWidget(perfBar);
    m_perfOffline = new QLabel;
    m_perfOffline->setObjectName(QLatin1String("bidiPerfOffline"));
    m_perfOffline->setWordWrap(true);
    m_perfOffline->setStyleSheet(QStringLiteral(
        "padding: 6px; color: palette(disabled-text);"));
    m_perfOffline->hide();
    pv->addWidget(m_perfOffline);
    m_perfBody = new QWidget;
    m_perfBody->setObjectName(QLatin1String("bidiPerfBody"));
    QVBoxLayout *pbody = new QVBoxLayout(m_perfBody);
    pbody->setContentsMargins(0, 0, 0, 0);
    pbody->setSpacing(0);
    pbody->addWidget(new QLabel(tr("Live metrics")));
    m_metricsTable = new QTableWidget(0, 2);
    m_metricsTable->setObjectName(QLatin1String("bidiPerfMetrics"));
    m_metricsTable->setHorizontalHeaderLabels(
        {tr("Metric"), tr("Value")});
    m_metricsTable->horizontalHeader()->setStretchLastSection(true);
    m_metricsTable->verticalHeader()->setVisible(false);
    m_metricsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pbody->addWidget(m_metricsTable);
    pbody->addWidget(
        new QLabel(tr("CPU profile — top functions by self time")));
    m_cpuTable = new QTableWidget(0, 4);
    m_cpuTable->setObjectName(QLatin1String("bidiPerfCpu"));
    m_cpuTable->setHorizontalHeaderLabels(
        {tr("Function"), tr("Self"), tr("Total"), tr("Source")});
    m_cpuTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_cpuTable->horizontalHeader()->setStretchLastSection(false);
    m_cpuTable->verticalHeader()->setVisible(false);
    m_cpuTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pbody->addWidget(m_cpuTable);
    pv->addWidget(m_perfBody, 1);
    tabs->addTab(perfPage, tr("Performance"));
    m_metricsTimer = new QTimer(this);
    m_metricsTimer->setInterval(1000);
    connect(m_metricsTimer, &QTimer::timeout,
            this, &BidiPanel::requestMetrics);
    connect(perfRefresh, &QToolButton::clicked,
            this, &BidiPanel::requestMetrics);
    connect(m_cpuToggle, &QToolButton::toggled,
            this, &BidiPanel::setCpuProfiling);

    // DEVT13 — Memory: a real allocation-sampling capture through
    // HeapProfiler.startSampling/stopSampling, aggregated by
    // function.  Same honest-disabled shape as Performance.
    // DEVT21 adds the one-shot heap summary — Runtime.getHeapUsage
    // totals plus the sampling pass's top allocation sites.
    QWidget *memPage = new QWidget;
    memPage->setObjectName(QLatin1String("bidiMemoryPage"));
    QVBoxLayout *mv = new QVBoxLayout(memPage);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);
    QWidget *memBar = new QWidget;
    memBar->setObjectName(QLatin1String("bidiMemoryToolbar"));
    QHBoxLayout *mb = new QHBoxLayout(memBar);
    mb->setContentsMargins(4, 2, 4, 2);
    mb->setSpacing(4);
    m_heapButton = new QToolButton;
    m_heapButton->setObjectName(QLatin1String("bidiMemoryCollect"));
    m_heapButton->setAutoRaise(true);
    m_heapButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));
    m_heapButton->setText(tr(" Collect allocation profile"));
    m_heapButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_heapButton->setToolTip(
        tr("Sample JS heap allocations for two seconds, then show "
           "where the bytes went"));
    mb->addWidget(m_heapButton);
    m_snapButton = new QToolButton;
    m_snapButton->setObjectName(
        QLatin1String("bidiMemorySnapshot"));
    m_snapButton->setAutoRaise(true);
    m_snapButton->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogInfoView));
    m_snapButton->setText(tr(" Heap summary"));
    m_snapButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_snapButton->setToolTip(
        tr("Report the JS heap's used and total size plus the top "
           "allocation sites (a sampling estimate, not a full heap "
           "snapshot)"));
    mb->addWidget(m_snapButton);
    m_memStatus = new QLabel;
    m_memStatus->setObjectName(QLatin1String("bidiMemoryStatus"));
    mb->addWidget(m_memStatus);
    mb->addStretch();
    mv->addWidget(memBar);
    m_memOffline = new QLabel;
    m_memOffline->setObjectName(QLatin1String("bidiMemoryOffline"));
    m_memOffline->setWordWrap(true);
    m_memOffline->setStyleSheet(QStringLiteral(
        "padding: 6px; color: palette(disabled-text);"));
    m_memOffline->hide();
    mv->addWidget(m_memOffline);
    m_memBody = new QWidget;
    m_memBody->setObjectName(QLatin1String("bidiMemoryBody"));
    QVBoxLayout *mbody = new QVBoxLayout(m_memBody);
    mbody->setContentsMargins(0, 0, 0, 0);
    mbody->setSpacing(0);
    m_heapTable = new QTableWidget(0, 3);
    m_heapTable->setObjectName(QLatin1String("bidiMemoryHeap"));
    m_heapTable->setHorizontalHeaderLabels(
        {tr("Function"), tr("Self size"), tr("Source")});
    m_heapTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_heapTable->horizontalHeader()->setStretchLastSection(false);
    m_heapTable->verticalHeader()->setVisible(false);
    m_heapTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mbody->addWidget(m_heapTable);
    mv->addWidget(m_memBody, 1);
    tabs->addTab(memPage, tr("Memory"));
    connect(m_heapButton, &QToolButton::clicked,
            this, &BidiPanel::collectHeapSample);
    connect(m_snapButton, &QToolButton::clicked,
            this, &BidiPanel::collectHeapSnapshot);

    // DEVT13 — Application: Chrome's storage tree.  One nav tree on
    // the left, the matching values table on the right; every row
    // reads real data (BiDi cookies, script.evaluate for the
    // storage/databases lists).
    QWidget *appPage = new QWidget;
    QVBoxLayout *av = new QVBoxLayout(appPage);
    av->setContentsMargins(0, 0, 0, 0);
    av->setSpacing(0);
    QSplitter *appSplit = new QSplitter(Qt::Horizontal);
    m_appTree = new QTreeWidget;
    m_appTree->setObjectName(QLatin1String("bidiAppTree"));
    m_appTree->setHeaderLabel(tr("Application"));
    m_appTree->setRootIsDecorated(true);
    // Sections keyed by their m_appStack page index.
    QTreeWidgetItem *storageRoot = new QTreeWidgetItem(
        QStringList(tr("Storage")));
    static const struct { const char *label; int page; } kAppSec[] = {
        {"Local Storage", 1}, {"Session Storage", 2},
        {"Cookies", 0}, {"IndexedDB", 3},
    };
    for (const auto &sec : kAppSec) {
        QTreeWidgetItem *item = new QTreeWidgetItem(
            QStringList(tr(sec.label)));
        item->setData(0, Qt::UserRole, sec.page);
        storageRoot->addChild(item);
    }
    m_appTree->addTopLevelItem(storageRoot);
    m_appTree->expandAll();
    appSplit->addWidget(m_appTree);
    m_appStack = new QStackedWidget;
    // Page 0 — cookies.
    QWidget *cookieBox = new QWidget;
    QVBoxLayout *cbv = new QVBoxLayout(cookieBox);
    cbv->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout *ckh = new QHBoxLayout;
    ckh->addWidget(new QLabel(tr("Cookies")));
    ckh->addStretch();
    QPushButton *refresh = new QPushButton(tr("Refresh"));
    connect(refresh, &QPushButton::clicked,
            this, &BidiPanel::refreshStorage);
    ckh->addWidget(refresh);
    cbv->addLayout(ckh);
    m_cookieTable = new QTableWidget(0, 4);
    m_cookieTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Value"), tr("Domain"), tr("Path")});
    m_cookieTable->horizontalHeader()->setStretchLastSection(true);
    m_cookieTable->verticalHeader()->setVisible(false);
    m_cookieTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cbv->addWidget(m_cookieTable);
    m_appStack->addWidget(cookieBox);
    // Page 1 — localStorage.
    QWidget *lsBox = new QWidget;
    QVBoxLayout *lv = new QVBoxLayout(lsBox);
    lv->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout *lh = new QHBoxLayout;
    lh->addWidget(new QLabel(tr("Local Storage")));
    lh->addStretch();
    QPushButton *refreshLs = new QPushButton(tr("Refresh"));
    connect(refreshLs, &QPushButton::clicked,
            this, &BidiPanel::refreshApplication);
    lh->addWidget(refreshLs);
    lv->addLayout(lh);
    m_localStorageTable = new QTableWidget(0, 2);
    m_localStorageTable->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
    m_localStorageTable->horizontalHeader()->setStretchLastSection(true);
    m_localStorageTable->verticalHeader()->setVisible(false);
    m_localStorageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lv->addWidget(m_localStorageTable);
    m_appStack->addWidget(lsBox);
    // Page 2 — sessionStorage.
    QWidget *ssBox = new QWidget;
    QVBoxLayout *ssv = new QVBoxLayout(ssBox);
    ssv->setContentsMargins(0, 0, 0, 0);
    ssv->addWidget(new QLabel(tr("Session Storage")));
    m_sessionStorageTable = new QTableWidget(0, 2);
    m_sessionStorageTable->setHorizontalHeaderLabels(
        {tr("Key"), tr("Value")});
    m_sessionStorageTable->horizontalHeader()
        ->setStretchLastSection(true);
    m_sessionStorageTable->verticalHeader()->setVisible(false);
    m_sessionStorageTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    ssv->addWidget(m_sessionStorageTable);
    m_appStack->addWidget(ssBox);
    // Page 3 — IndexedDB.
    QWidget *idbBox = new QWidget;
    QVBoxLayout *iv = new QVBoxLayout(idbBox);
    iv->setContentsMargins(0, 0, 0, 0);
    iv->addWidget(new QLabel(tr("IndexedDB databases")));
    m_idbTable = new QTableWidget(0, 2);
    m_idbTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Version")});
    m_idbTable->horizontalHeader()->setStretchLastSection(true);
    m_idbTable->verticalHeader()->setVisible(false);
    m_idbTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    iv->addWidget(m_idbTable);
    m_appStack->addWidget(idbBox);
    appSplit->addWidget(m_appStack);
    appSplit->setStretchFactor(0, 1);
    appSplit->setStretchFactor(1, 3);
    av->addWidget(appSplit);
    tabs->addTab(appPage, tr("Application"));
    connect(m_appTree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
        if (!cur)
            return;
        const QVariant page = cur->data(0, Qt::UserRole);
        if (page.isValid())
            m_appStack->setCurrentIndex(page.toInt());
        refreshApplication();
    });

    // DEVT13 — Security: no CDP needed.  The scheme + the page's
    // (host,port) verdict from TlsVerifier — Arora's own second-
    // opinion TLS chain check — plus a mixed-content count observed
    // from the Network rows.
    QWidget *secPage = new QWidget;
    secPage->setObjectName(QLatin1String("bidiSecPage"));
    QVBoxLayout *secv = new QVBoxLayout(secPage);
    QWidget *secBar = new QWidget;
    QHBoxLayout *secl = new QHBoxLayout(secBar);
    secl->setContentsMargins(4, 2, 4, 2);
    QToolButton *secRefresh = new QToolButton;
    secRefresh->setAutoRaise(true);
    secRefresh->setIcon(
        style()->standardIcon(QStyle::SP_BrowserReload));
    secRefresh->setToolTip(tr("Re-read the security state"));
    secl->addWidget(secRefresh);
    secl->addStretch();
    secv->addWidget(secBar);
    m_secHeadline = new QLabel;
    m_secHeadline->setObjectName(QLatin1String("bidiSecHeadline"));
    {
        QFont big = m_secHeadline->font();
        big.setPointSizeF(big.pointSizeF() + 2.0);
        big.setBold(true);
        m_secHeadline->setFont(big);
    }
    m_secHeadline->setContentsMargins(8, 4, 8, 0);
    secv->addWidget(m_secHeadline);
    m_secDetail = new QLabel;
    m_secDetail->setObjectName(QLatin1String("bidiSecDetail"));
    m_secDetail->setWordWrap(true);
    m_secDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secDetail->setContentsMargins(8, 4, 8, 4);
    secv->addWidget(m_secDetail);
    m_secMixed = new QLabel;
    m_secMixed->setObjectName(QLatin1String("bidiSecMixed"));
    m_secMixed->setWordWrap(true);
    m_secMixed->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secMixed->setContentsMargins(8, 0, 8, 4);
    secv->addWidget(m_secMixed);
    secv->addStretch();
    tabs->addTab(secPage, tr("Security"));
    connect(secRefresh, &QToolButton::clicked,
            this, &BidiPanel::refreshSecurity);

    QVBoxLayout *panelLayout = new QVBoxLayout(this);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->addWidget(m_stack);

    connect(m_client, &BidiClient::responseReceived,
            this, &BidiPanel::onResponse);
    connect(m_client, &BidiClient::eventReceived,
            this, &BidiPanel::onEvent);
    connect(m_client, &BidiClient::connectionChanged,
            this, &BidiPanel::onConnectionChanged);
    connect(m_contextCombo, &QComboBox::currentIndexChanged,
            this, [this](int) {
        applyContextSelection();
    });
    // DEVT13: the Elements page's own wiring.
    connect(m_domTree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
        updateCrumbs(cur);
        requestStylesFor(cur);
    });
    connect(m_inspectToggle, &QToolButton::toggled,
            this, &BidiPanel::setInspecting);
    connect(m_domRefresh, &QToolButton::clicked,
            this, &BidiPanel::refreshDom);
    // DEVT19 — Elements parity wiring: find bar, live-refresh
    // debounce, probe watchdog, attribute editing.
    connect(domSearchToggle, &QToolButton::clicked, this, [this]() {
        m_domFindBar->setVisible(!m_domFindBar->isVisible());
        if (m_domFindBar->isVisible()) {
            m_domFind->setFocus();
            m_domFind->selectAll();
        }
    });
    QShortcut *findShortcut = new QShortcut(
        QKeySequence::Find, elementsPage);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this]() {
        m_domFindBar->setVisible(!m_domFindBar->isVisible());
        if (m_domFindBar->isVisible()) {
            m_domFind->setFocus();
            m_domFind->selectAll();
        }
    });
    m_domSearchDebounce = new QTimer(this);
    m_domSearchDebounce->setSingleShot(true);
    m_domSearchDebounce->setInterval(300);
    connect(m_domSearchDebounce, &QTimer::timeout,
            this, &BidiPanel::runDomSearch);
    connect(m_domFind, &QLineEdit::textChanged,
            this, [this]() { m_domSearchDebounce->start(); });
    connect(m_domFind, &QLineEdit::returnPressed, this, [this]() {
        if (m_searchPaths.isEmpty())
            runDomSearch();
        else
            domSearchStep(1);
    });
    connect(m_domFindPrev, &QToolButton::clicked,
            this, [this]() { domSearchStep(-1); });
    connect(m_domFindNext, &QToolButton::clicked,
            this, [this]() { domSearchStep(1); });
    // DEVT22: clicking a results-list row jumps to that hit — same
    // landing as Enter/arrow cycling.
    connect(m_domFindResults, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *it) {
        const int row = m_domFindResults->row(it);
        if (row < 0 || row >= m_searchPaths.size())
            return;
        m_searchIndex = row;
        m_domFindCount->setText(
            tr("%1 of %2").arg(m_searchIndex + 1)
                .arg(m_searchPaths.size()));
        domSelectPath(m_searchPaths.at(row));
    });
    m_domLiveTimer = new QTimer(this);
    m_domLiveTimer->setSingleShot(true);
    m_domLiveTimer->setInterval(350);
    connect(m_domLiveTimer, &QTimer::timeout,
            this, &BidiPanel::liveRefreshDom);
    m_probeWatch = new QTimer(this);
    m_probeWatch->setSingleShot(true);
    m_probeWatch->setInterval(6000);
    connect(m_probeWatch, &QTimer::timeout,
            this, &BidiPanel::probeWatchdog);
    // DEVT20: Page.frameNavigated (and the load milestones) burst on
    // multi-frame pages — coalesce into one resource-tree refresh.
    m_srcRefreshDebounce = new QTimer(this);
    m_srcRefreshDebounce->setSingleShot(true);
    m_srcRefreshDebounce->setInterval(400);
    connect(m_srcRefreshDebounce, &QTimer::timeout,
            this, &BidiPanel::refreshSources);
    // Buttons start down until the Debugger probe verdict lands.
    updateDebuggerButtons();
    connect(m_stylesView, &QTreeWidget::itemChanged,
            this, [this](QTreeWidgetItem *item, int column) {
        if (m_stylesFilling || !item || !item->parent())
            return;
        if (item->parent()->data(0, Qt::UserRole + 4).toString()
                != tr("Attributes"))
            return;
        if (column == 0) {
            // Renames aren't wired — put the original name back.
            m_stylesFilling = true;
            item->setText(0, item->data(0, Qt::UserRole).toString());
            m_stylesFilling = false;
            return;
        }
        editDomAttribute(item, column);
    });
    // The metrics poll only runs while its tab is visible.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        QWidget *w = m_tabs->widget(i);
        if (w && w->objectName() == QLatin1String("bidiPerfPage")
            && domainUp(QLatin1String("Performance")))
            m_metricsTimer->start();
        else
            m_metricsTimer->stop();
        if (w && w->objectName() == QLatin1String("bidiSecPage"))
            refreshSecurity();
    });

    // DEVT12: console toolbar.
    connect(m_clearConsole, &QToolButton::clicked,
            this, &BidiPanel::clearConsole);
    connect(m_consoleFilter, &QLineEdit::textChanged,
            this, [this](const QString &) { applyConsoleFilters(); });

    // Network toolbar + chips drive the proxy; the view just follows.
    connect(m_networkFilter, &QLineEdit::textChanged,
            this, [this](const QString &text) {
        static_cast<BidiNetworkProxy *>(m_networkProxy)
            ->setNeedle(text);
    });
    connect(m_invertFilter, &QCheckBox::toggled,
            this, [this](bool on) {
        static_cast<BidiNetworkProxy *>(m_networkProxy)
            ->setInverted(on);
    });
    connect(m_typeChips, &QButtonGroup::buttonToggled,
            this, [this](QAbstractButton *button, bool checked) {
        if (checked)
            static_cast<BidiNetworkProxy *>(m_networkProxy)
                ->setBucket(m_typeChips->id(button));
    });
    connect(m_clearRequests, &QToolButton::clicked,
            this, &BidiPanel::clearNetworkLog);
    // DEVT11: the record toggle's dot swaps red<->grey and the
    // tooltip flips — the actual event gate lives in onEvent.
    connect(m_recordToggle, &QToolButton::toggled,
            this, [this](bool recording) {
        m_recordToggle->setIcon(
            recordDotIcon(recording, palette()));
        m_recordToggle->setToolTip(
            recording ? tr("Stop recording network log")
                      : tr("Record network log"));
    });
    connect(m_disableCache, &QCheckBox::toggled,
            this, [this](bool) { sendCacheBehavior(); });
    // The footer follows the FILTERED row set — proxy signals already
    // aggregate model inserts/removes/resets/edits and re-slices;
    // layoutChanged covers proxy re-slices and the source model's
    // dataChanged catches in-place value edits.
    connect(m_networkProxy, &QAbstractItemModel::rowsInserted,
            this, [this] { updateNetworkSummary(); });
    connect(m_networkProxy, &QAbstractItemModel::rowsRemoved,
            this, [this] { updateNetworkSummary(); });
    connect(m_networkProxy, &QAbstractItemModel::modelReset,
            this, [this] { updateNetworkSummary(); });
    connect(m_networkProxy, &QAbstractItemModel::dataChanged,
            this, [this] { updateNetworkSummary(); });
    connect(m_networkProxy, &QAbstractItemModel::layoutChanged,
            this, [this] { updateNetworkSummary(); });
    connect(m_networkModel, &QAbstractItemModel::dataChanged,
            this, [this] { updateNetworkSummary(); });
    updateNetworkSummary();
    // The model trims at 1000 rows — rebase requestId -> row so late
    // events still patch the right line.
    connect(m_networkModel, &QAbstractItemModel::rowsRemoved,
            this, [this](const QModelIndex &, int first, int last) {
        const int count = last - first + 1;
        for (auto it = m_requestRows.begin();
             it != m_requestRows.end();) {
            if (it.value() >= first && it.value() <= last) {
                it = m_requestRows.erase(it);
            } else {
                if (it.value() > last)
                    it.value() -= count;
                ++it;
            }
        }
    });

    m_stack->setCurrentIndex(0);
    m_unavailableLabel->setText(tr("Debug channel not connected."));
}

void BidiPanel::setTabWidget(TabWidget *tabs)
{
    if (m_tabWidget == tabs)
        return;
    if (m_tabWidget)
        disconnect(m_tabWidget, nullptr, this, nullptr);
    m_tabWidget = tabs;
    m_attachRetries = 0;
    if (tabs) {
        // Strip churn that produces no CDP traffic (engine/widget
        // tabs, renames, drags) still re-merges the selector against
        // the cached context list.
        connect(tabs, &TabWidget::tabsChanged,
                this, &BidiPanel::rebuildContextCombo);
    }
    rebuildContextCombo();
}

void BidiPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_client->state() == BidiClient::Disconnected)
        m_client->connectToEngine();
}

void BidiPanel::onConnectionChanged(int state)
{
    m_channelLive = (state == BidiClient::Connected);
    if (state == BidiClient::Connected) {
        m_stack->setCurrentIndex(1);
        m_attachRetries = 0;
        // Rows drawn while the channel was down read "no debug
        // target" — re-merge so a waiting tab flips to "attaching…"
        // before the first getTree answer even lands.
        rebuildContextCombo();
        refreshContexts();
        refreshApplication();
        refreshSecurity();
        // A reconnect is a fresh channel — the bridge forgot the
        // cache flag, so re-arm it when the box is still ticked.
        if (m_disableCache->isChecked())
            sendCacheBehavior();
    } else {
        m_stack->setCurrentIndex(0);
        m_metricsTimer->stop();
        m_domLiveTimer->stop();
        // DEVT19: drop the probe verdicts with the channel — a
        // reconnect is a fresh session and re-probes; keeping the
        // old map would pin a dead endpoint's "down" forever (or
        // worse, report "up" on a channel that can't serve).
        m_probeWatch->stop();
        m_domainErr.clear();
        m_probedContext.clear();
        // DEVT20: the channel died — script ids, pause chrome and
        // session breakpoint ids are all stale.
        resetDebuggerState();
        // DEVT21: an in-flight heap capture's answers died with the
        // channel — without the reset the flags would block every
        // later capture on the reconnected session.
        m_heapSampling = false;
        m_snapshotRun = false;
        m_snapStopSeen = false;
        m_snapUsed = m_snapTotal = -1;
        m_snapErr.clear();
        m_breakpoints.clear();
        rebuildBreakpointList();
        updateDebuggerButtons();
        m_unavailableLabel->setText(
            m_client->unavailableReason().isEmpty()
                ? tr("Debug channel disconnected.")
                : m_client->unavailableReason());
    }
}

QString BidiPanel::currentContext() const
{
    return m_contextCombo->currentData().toString();
}

// Every way the selection can move — the user picking a combo row,
// or rebuildContextCombo landing on a different enabled row behind
// blocked signals — runs this one cascade: a new page owns a fresh
// milestone clock, a fresh Elements tree, fresh domain verdicts,
// and fresh Sources/Security/Application views.
void BidiPanel::applyContextSelection()
{
    // Milestones belong to the page that fired them — a context
    // switch starts a fresh clock.
    m_dclMs = m_loadMs = -1.0;
    refreshDom();
    updateNetworkSummary();
    // DEVT13: the verdicts are per-session — a different page is a
    // different attach, so re-probe, and the Sources/Security/
    // Application views follow the new page.  DEVT20: the debugger
    // surface is per-session too — script ids and breakpoint ids
    // belong to the page we just left.
    resetDebuggerState();
    m_breakpoints.clear();
    rebuildBreakpointList();
    probeDomains();
    refreshSources();
    refreshSecurity();
    refreshApplication();
}

void BidiPanel::consoleLine(const QString &prefix, const QString &text)
{
    // REPL echo/result lines — the "< error:" form takes the error
    // wash like Chrome's red-thrown reply; none of them dedupe.
    const int level = prefix.startsWith(QLatin1String("< error"))
        ? LevelError : LevelInfo;
    addConsoleRow(level, prefix + text, QJsonObject(), QJsonArray(),
                  false);
}

// DEVT12: one row of the structured console list.  Identical
// consecutive entries (same level + text + source) collapse into
// the previous row with a count badge — Chrome's repeat marker,
// which is what keeps a spammed warning readable.
void BidiPanel::addConsoleRow(int level, const QString &text,
                              const QJsonObject &source,
                              const QJsonArray &args,
                              bool dedupeable)
{
    const QString url =
        source[QLatin1String("url")].toString();
    const int line = source[QLatin1String("line")].toInt(-1);
    QString sourceText;
    if (!url.isEmpty()) {
        sourceText = requestName(url);
        if (line > 0)
            sourceText += QLatin1Char(':') + QString::number(line);
    }

    QString key;
    if (dedupeable) {
        key = QString::number(level) + QLatin1Char('|') + text
            + QLatin1Char('|') + url + QLatin1Char(':')
            + QString::number(line);
        const int rows = m_console->topLevelItemCount();
        if (rows > 0) {
            QTreeWidgetItem *last =
                m_console->topLevelItem(rows - 1);
            if (last->data(0, ConsoleKeyRole).toString() == key) {
                const int count =
                    last->data(0, ConsoleCountRole).toInt() + 1;
                last->setData(0, ConsoleCountRole, count);
                last->setText(1, QString::number(count));
                return;
            }
        }
    }

    // Keep the view's autoscroll-on-new-entry behavior: jump only
    // when the tail was already in view.
    QScrollBar *bar = m_console->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    // Same 5000-entry cap the flat text edit enforced.
    while (m_console->topLevelItemCount() >= 5000)
        delete m_console->takeTopLevelItem(0);

    QTreeWidgetItem *item = new QTreeWidgetItem;
    item->setText(0, text);
    item->setToolTip(0, text);
    item->setData(0, ConsoleLevelRole, level);
    item->setData(0, ConsoleCountRole, 1);
    if (dedupeable)
        item->setData(0, ConsoleKeyRole, key);
    if (level == LevelError)
        item->setIcon(
            0, style()->standardIcon(QStyle::SP_MessageBoxCritical));
    else if (level == LevelWarning)
        item->setIcon(
            0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
    const QColor wash = consoleWash(level, palette());
    if (wash.isValid()) {
        for (int c = 0; c < 3; ++c)
            item->setBackground(c, QBrush(wash));
    } else if (level == LevelVerbose) {
        item->setForeground(
            0, palette().color(QPalette::Disabled, QPalette::Text));
    }
    if (!sourceText.isEmpty()) {
        item->setText(2, sourceText);
        item->setTextAlignment(
            2, Qt::AlignRight | Qt::AlignVCenter);
        QString tip = url;
        if (line > 0)
            tip += QLatin1Char(':') + QString::number(line);
        item->setToolTip(2, tip);
        item->setForeground(2, palette().color(QPalette::Link));
    }
    // Expandable "name: value" children only where the event's args
    // carried a real CDP preview — no objectId round-trip exists to
    // fake a deeper read.
    consolePreviewChildren(item, args, wash, 0);
    m_console->addTopLevelItem(item);

    // Apply the active level/text filters to the fresh row.
    const int allowed =
        m_consoleLevelMask & (1 << item->data(0, ConsoleLevelRole)
                                       .toInt());
    const QString needle = m_consoleFilter->text();
    const bool match =
        needle.isEmpty()
        || item->text(0).contains(needle, Qt::CaseInsensitive)
        || item->text(2).contains(needle, Qt::CaseInsensitive);
    item->setHidden(!allowed || !match);
    if (atBottom)
        m_console->scrollToBottom();
}

void BidiPanel::applyConsoleFilters()
{
    const QString needle = m_consoleFilter->text();
    for (int i = 0; i < m_console->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_console->topLevelItem(i);
        const int level =
            item->data(0, ConsoleLevelRole).toInt();
        bool ok = (m_consoleLevelMask & (1 << level)) != 0;
        if (ok && !needle.isEmpty())
            ok = item->text(0).contains(needle, Qt::CaseInsensitive)
                || item->text(2).contains(needle,
                                          Qt::CaseInsensitive);
        item->setHidden(!ok);
    }
}

// Chrome's dropdown label reports the checked set: all -> All
// levels, the factory set -> Default levels, empty -> No levels,
// anything else -> Custom levels.
void BidiPanel::updateConsoleLevelMask()
{
    int mask = 0;
    for (QAction *action : m_consoleLevels->menu()->actions()) {
        if (action->isChecked())
            mask |= 1 << action->data().toInt();
    }
    m_consoleLevelMask = mask;
    const int all = (1 << LevelCount) - 1;
    m_consoleLevels->setText(
        mask == all ? tr("All levels")
        : mask == kDefaultConsoleLevels ? tr("Default levels")
        : mask == 0 ? tr("No levels")
                    : tr("Custom levels"));
    applyConsoleFilters();
}

void BidiPanel::clearConsole()
{
    m_console->clear();
}

void BidiPanel::evaluateInput()
{
    const QString expr = m_evalLine->text().trimmed();
    if (expr.isEmpty())
        return;
    m_evalLine->clear();
    // Up/Down recall — newest last, no consecutive duplicates.
    if (m_evalHistory.isEmpty()
        || m_evalHistory.constLast() != expr) {
        m_evalHistory.append(expr);
        if (m_evalHistory.size() > 100)
            m_evalHistory.removeFirst();
    }
    m_evalHistoryIndex = -1;
    m_evalDraft.clear();
    consoleLine(QStringLiteral("> "), expr);
    sendEval(currentContext(), expr, TagConsole);
}

quint64 BidiPanel::sendEval(const QString &context,
                            const QString &expression, quint64 tag)
{
    QJsonObject target;
    target[QStringLiteral("context")] = context;
    QJsonObject params;
    params[QStringLiteral("expression")] = expression;
    params[QStringLiteral("target")] = target;
    params[QStringLiteral("awaitPromise")] = true;
    const quint64 id = m_client->sendCommand(
        QStringLiteral("script.evaluate"),
        QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (id)
        m_evalTags.insert(id, tag);
    return id;
}

void BidiPanel::refreshContexts()
{
    const quint64 id = m_client->sendCommand(
        QStringLiteral("browsingContext.getTree"));
    if (id)
        m_evalTags.insert(id, 0); // 0 = getTree bookkeeping
    if (!m_preserveLog->isChecked())
        clearNetworkLog();
}

// DEVT16: a tab can legitimately outrun its target — a getTree
// snapshot taken before targetCreated lands reads "no debug target"
// for a page that registers a second later.  While the channel is
// live, re-ask for the tree a bounded number of times so the row
// resolves into a real context instead of pinning the verdict; the
// budget resets on every browsingContext event (new evidence means
// the snapshot moved, so waiting is worth it again).
void BidiPanel::armAttachRetry()
{
    if (m_attachPending
        || m_attachRetries >= kMaxAttachRetries
        || !m_channelLive)
        return;
    m_attachPending = true;
    QTimer::singleShot(600, this, [this]() {
        m_attachPending = false;
        if (!m_channelLive)
            return;
        ++m_attachRetries;
        refreshContexts();
    });
}

// DEVT10: the selector mirrors the window's REAL tab strip.  CDP
// only ever reports type=="page" targets — engine tabs (Servo) and
// widget tabs (Preferences, Downloads, ...) have none, so they used
// to be silently absent and a missing first tab read as a bug.  Now
// every strip tab gets a row in strip order; non-debuggable rows are
// disabled with the reason in the tooltip and as a suffix — the same
// honesty pattern as the engine indicator's locked-with-tooltip.
// WebEngine tabs claim their context by committed url; targets no
// strip tab claimed (other windows, the inspector page) keep the old
// append-only behavior at the end of the list.
//
// DEVT16: claims resolve in a first pass, rows render in a second —
// the url forms the two sides report legitimately diverge (the tab
// may still hold the requested/typed spelling while the target shows
// the committed one, a fresh target surfaces "" where the tab reads
// about:blank), so matching normalizes instead of comparing raw
// strings; a failed navigation leaves the target on a chrome-error:
// url the tab never shows, which pairs only when it is the unique
// remaining candidate.  A web tab with no claim while the channel is
// live is "attaching…" — armAttachRetry re-asks for the tree a few
// times so a late-registering target lands on a real row; "no debug
// target" is the verdict only after that budget is spent.
void BidiPanel::rebuildContextCombo()
{
    const QString keep = currentContext();
    m_contextCombo->blockSignals(true);
    m_contextCombo->clear();
    QStandardItemModel *model =
        qobject_cast<QStandardItemModel *>(m_contextCombo->model());

    auto cdpLabel = [](const QJsonObject &c) {
        const QString title = c[QLatin1String("title")].toString();
        return title.isEmpty()
            ? c[QLatin1String("url")].toString() : title;
    };

    // Unclaimed page targets — a matched web tab consumes one entry.
    QVector<QJsonObject> pending;
    pending.reserve(m_contexts.size());
    for (const QJsonValue &cv : m_contexts)
        pending.append(cv.toObject());

    const auto addDisabled = [&](const QString &title,
                                 const QString &tag,
                                 const QString &reason) {
        const int row = m_contextCombo->count();
        m_contextCombo->addItem(title.isEmpty()
                                ? tag
                                : tr("%1 — %2").arg(title, tag));
        if (model) {
            if (QStandardItem *item = model->item(row))
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        m_contextCombo->setItemData(row, reason, Qt::ToolTipRole);
    };

    int unmatchedWeb = 0;
    if (m_tabWidget) {
        // Pass 1 — resolve claims.  The tab url and the target url
        // agree in content but not always in spelling: Chromium
        // normalizes a bare host to "…/" while the tab can still be
        // holding the typed form, and a not-yet-committed target
        // reports "" for the tab's about:blank.
        const auto urlsMatch = [](const QString &targetUrl,
                                  const QUrl &tabUrl) {
            if (targetUrl == tabUrl.toString())
                return true;
            // QUrl::StripTrailingSlash keeps a bare host's root "/",
            // so drop it by hand — "http://h/" and "http://h" are
            // the same page to a user.
            const auto rootless = [](QUrl url) {
                if (url.path() == QLatin1String("/"))
                    url.setPath(QString());
                return url;
            };
            if (rootless(QUrl(targetUrl)) == rootless(tabUrl))
                return true;
            const bool targetBlank = targetUrl.isEmpty()
                || targetUrl == QLatin1String("about:blank");
            const bool tabBlank = tabUrl.isEmpty()
                || tabUrl == QUrl(QStringLiteral("about:blank"));
            return targetBlank && tabBlank;
        };
        const int tabs = m_tabWidget->count();
        QVector<QJsonObject> claimed(tabs);
        QVector<int> openWeb;
        for (int i = 0; i < tabs; ++i) {
            if (m_tabWidget->engineTab(i)
                || !m_tabWidget->webView(i))
                continue;
            WebView *view = m_tabWidget->webView(i);
            for (int p = 0; p < pending.size(); ++p) {
                if (urlsMatch(pending[p][QLatin1String("url")]
                                  .toString(),
                              view->url())) {
                    claimed[i] = pending.takeAt(p);
                    break;
                }
            }
            if (claimed[i].isEmpty())
                openWeb.append(i);
        }
        // A net error commits the tab's target on
        // chrome-error://chromewebdata/ — the urls can never agree,
        // so the claim is positional: safe only when a single open
        // tab and a single error target remain.
        if (openWeb.size() == 1) {
            int err = -1;
            bool ambiguous = false;
            for (int p = 0; p < pending.size(); ++p) {
                if (pending[p][QLatin1String("url")].toString()
                        .startsWith(QLatin1String("chrome-error:"))) {
                    if (err >= 0) {
                        ambiguous = true;
                        break;
                    }
                    err = p;
                }
            }
            if (err >= 0 && !ambiguous)
                claimed[openWeb.first()] = pending.takeAt(err);
        }

        // Pass 2 — render one row per strip tab in strip order.
        const bool attaching = m_channelLive
            && m_attachRetries < kMaxAttachRetries;
        for (int i = 0; i < tabs; ++i) {
            const QString title = m_tabWidget->tabDisplayTitle(i);
            if (EngineTab *foreign = m_tabWidget->engineTab(i)) {
                const QString backend = foreign->backend()
                    ? foreign->backend()->displayName() : QString();
                addDisabled(title,
                            backend.isEmpty()
                                ? tr("non-Chromium tab")
                                : tr("%1 tab").arg(backend),
                            backend.isEmpty()
                                ? tr("This engine exposes no devtools "
                                     "backend")
                                : tr("%1 tab — no devtools backend")
                                      .arg(backend));
                continue;
            }
            if (!m_tabWidget->webView(i)) {
                addDisabled(title, tr("not a web page"),
                            tr("Not a web page — the devtools panel "
                               "inspects web content only"));
                continue;
            }
            const QJsonObject c = claimed[i];
            if (c.isEmpty()) {
                const bool sleeping = m_tabWidget->isTabSleeping(i);
                const bool waiting = !sleeping && attaching;
                addDisabled(title,
                            sleeping ? tr("asleep")
                                     : waiting ? tr("attaching…")
                                               : tr("no debug target"),
                            sleeping ? tr("Tab is asleep — wake it to "
                                          "attach devtools")
                                     : waiting ? tr("Web page — waiting "
                                                    "for the debug "
                                                    "channel to attach")
                                               : tr("Web page — not "
                                                    "exposed on the "
                                                    "debug channel"));
                if (waiting)
                    ++unmatchedWeb;
                continue;
            }
            m_contextCombo->addItem(
                title.isEmpty() ? cdpLabel(c) : title,
                c[QLatin1String("context")].toString());
        }
    }

    for (const QJsonObject &c : pending) {
        m_contextCombo->addItem(
            cdpLabel(c), c[QLatin1String("context")].toString());
    }

    if (unmatchedWeb)
        armAttachRetry();

    // Restore by context id; a vanished selection (or a strip whose
    // first slot is a non-CDP tab) must not land on a disabled row —
    // fall back to the first enabled entry, never index 0 blindly.
    const auto enabledAt = [model, this](int i) {
        const QStandardItem *item = model ? model->item(i) : nullptr;
        return !item || (item->flags() & Qt::ItemIsEnabled);
    };
    int idx = keep.isEmpty() ? -1 : m_contextCombo->findData(keep);
    if (idx < 0 || !enabledAt(idx)) {
        idx = -1;
        for (int i = 0; i < m_contextCombo->count(); ++i) {
            if (enabledAt(i)) {
                idx = i;
                break;
            }
        }
    }
    const QString now = idx >= 0
        ? m_contextCombo->itemData(idx).toString() : QString();
    m_contextCombo->setCurrentIndex(idx);
    m_contextCombo->blockSignals(false);
    // currentIndexChanged is blocked — run the context-switch reset
    // ourselves when the effective context actually moved.  DEVT17:
    // this used to only touch the footer clock, so a selection that
    // landed through a rebuild (strip churn, a vanished keep, an
    // enabled row appearing where all rows were disabled) left the
    // Elements tree empty or showing the old page until something
    // else happened to call refreshDom().
    if (now != keep)
        applyContextSelection();
}

void BidiPanel::clearNetworkLog()
{
    static_cast<BidiNetworkModel *>(m_networkModel)->clear();
    m_requestRows.clear();
}

bool BidiPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_networkView && watched == m_networkView->viewport()
        && event->type() == QEvent::Resize)
        fitNetworkColumns();
    // DEVT12: Up/Down at the REPL prompt walks the eval history —
    // first Up stashes the in-progress draft, Down past the newest
    // entry restores it.
    if (watched == m_evalLine && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Up && !m_evalHistory.isEmpty()) {
            if (m_evalHistoryIndex < 0) {
                m_evalDraft = m_evalLine->text();
                m_evalHistoryIndex = m_evalHistory.size() - 1;
            } else if (m_evalHistoryIndex > 0) {
                --m_evalHistoryIndex;
            }
            m_evalLine->setText(
                m_evalHistory.at(m_evalHistoryIndex));
            return true;
        }
        if (key->key() == Qt::Key_Down && m_evalHistoryIndex >= 0) {
            if (++m_evalHistoryIndex >= m_evalHistory.size()) {
                m_evalHistoryIndex = -1;
                m_evalLine->setText(m_evalDraft);
            } else {
                m_evalLine->setText(
                    m_evalHistory.at(m_evalHistoryIndex));
            }
            return true;
        }
        return false;
    }
    return QWidget::eventFilter(watched, event);
}

// DEVT07: split the viewport's elastic space between Name and
// Waterfall, keeping the user's width ratio and every column's floor.
// Numeric columns keep their widths; when the floors alone don't fit
// the table shows a horizontal scrollbar instead of hiding columns.
// DEVT18: under pressure the fixed columns now give up their slack
// down to their floors before the Waterfall is allowed off the right
// edge — at a real right-dock width the old "keep current widths"
// rule pushed it entirely past the viewport (the "no graph" report).
// The Initiator column, the least-essential and widest fixed column,
// auto-hides when even the floor layout can't fit; it comes back once
// the table fits it again with slack to spare.
void BidiPanel::fitNetworkColumns()
{
    if (m_fittingColumns)
        return;
    QHeaderView *head = m_networkView->horizontalHeader();
    const int avail = m_networkView->viewport()->width();
    if (avail <= 0)
        return;
    const int nameMin = kNetColumnMin[BidiNetworkModel::NameColumn];
    const int fallMin = kNetColumnMin[BidiNetworkModel::WaterfallColumn];

    m_fittingColumns = true;

    int floorTotal = 0;
    for (int c = 0; c < BidiNetworkModel::ColumnCount; ++c)
        floorTotal += kNetColumnMin[c];
    const int initCol = BidiNetworkModel::InitiatorColumn;
    if (avail < floorTotal) {
        if (!head->isSectionHidden(initCol))
            head->setSectionHidden(initCol, true);
    } else if (head->isSectionHidden(initCol)
               && avail >= floorTotal + kNetColumnMin[initCol] + 40) {
        // The +40 hysteresis keeps a boundary resize from flickering
        // the column on and off.
        head->setSectionHidden(initCol, false);
    }

    int fixed = 0;
    for (int c = BidiNetworkModel::StatusColumn;
         c <= BidiNetworkModel::TimeColumn; ++c) {
        if (!head->isSectionHidden(c))
            fixed += head->sectionSize(c);
    }
    int elastic = avail - fixed;
    if (elastic < nameMin + fallMin) {
        // Not enough room even for the elastic pair's floors — the
        // fixed columns shrink toward their own floors first so the
        // waterfall stays on screen as long as it possibly can.
        int deficit = (nameMin + fallMin) - elastic;
        for (int c = BidiNetworkModel::StatusColumn;
             c <= BidiNetworkModel::TimeColumn && deficit > 0; ++c) {
            if (head->isSectionHidden(c))
                continue;
            const int spare =
                head->sectionSize(c) - kNetColumnMin[c];
            if (spare <= 0)
                continue;
            const int take = qMin(spare, deficit);
            head->resizeSection(c, head->sectionSize(c) - take);
            fixed -= take;
            deficit -= take;
        }
        elastic = avail - fixed;
    }
    int name, fall;
    if (elastic < nameMin + fallMin) {
        name = nameMin;
        fall = fallMin;
    } else {
        const int cur = qMax(1, head->sectionSize(
                                 BidiNetworkModel::NameColumn)
                            + head->sectionSize(
                                  BidiNetworkModel::WaterfallColumn));
        name = qBound(nameMin,
                      elastic * head->sectionSize(
                                    BidiNetworkModel::NameColumn) / cur,
                      elastic - fallMin);
        fall = elastic - name;
    }
    if (head->sectionSize(BidiNetworkModel::NameColumn) != name)
        head->resizeSection(BidiNetworkModel::NameColumn, name);
    if (head->sectionSize(BidiNetworkModel::WaterfallColumn) != fall)
        head->resizeSection(BidiNetworkModel::WaterfallColumn, fall);
    m_fittingColumns = false;
}

// DEVT07: Chrome's summary line — counts and bytes track the filtered
// (visible) rows; Finish is the last completion of that set, and
// DOMContentLoaded/Load ride the Page-domain milestones when the
// engine emits them.  The "resources" figure sums the bridge's
// decoded (uncompressed) byte totals, falling back to wire size for
// rows that never reported chunks.
void BidiPanel::updateNetworkSummary()
{
    const int rows = m_networkProxy->rowCount();
    qint64 bytes = 0;
    qint64 resources = 0;
    double firstStart = -1.0;
    double lastEnd = -1.0;
    for (int r = 0; r < rows; ++r) {
        const QModelIndex cell =
            m_networkProxy->index(r, BidiNetworkModel::NameColumn);
        const qint64 size = cell.data(BidiNetworkModel::SizeRole)
                                .toLongLong();
        const qint64 decoded = cell.data(BidiNetworkModel::DecodedRole)
                                   .toLongLong();
        if (size > 0)
            bytes += size;
        resources += decoded >= 0 ? decoded : qMax<qint64>(size, 0);
        const double s = cell.data(BidiNetworkModel::StartRole)
                             .toDouble();
        const double e = cell.data(BidiNetworkModel::EndRole)
                             .toDouble();
        if (s >= 0.0 && (firstStart < 0.0 || s < firstStart))
            firstStart = s;
        if (e > lastEnd)
            lastEnd = e;
    }
    // Singular/plural is spelled out — an untranslated "%n" would
    // render the literal "request(s)".
    QString text = (rows == 1 ? tr("1 request")
                              : tr("%1 requests").arg(rows))
        + QStringLiteral(" · ")
        + tr("%1 transferred").arg(formatBytes(bytes))
        + QStringLiteral(" · ")
        + tr("%1 resources").arg(formatBytes(resources));
    if (firstStart >= 0.0 && lastEnd >= firstStart)
        text += tr(" · Finish: %1")
                    .arg(formatMillis(lastEnd - firstStart));
    // Milestone times are relative to the recording window's start
    // (navigation), matching how Chrome reports them.
    const double epoch = static_cast<BidiNetworkModel *>(m_networkModel)
        ->windowStart();
    if (m_dclMs >= 0.0 && epoch >= 0.0 && m_dclMs >= epoch)
        text += tr(" · DOMContentLoaded: %1")
                    .arg(formatMillis(m_dclMs - epoch));
    if (m_loadMs >= 0.0 && epoch >= 0.0 && m_loadMs >= epoch)
        text += tr(" · Load: %1").arg(formatMillis(m_loadMs - epoch));
    m_networkSummary->setText(text);
}

void BidiPanel::sendCacheBehavior()
{
    if (m_client->state() != BidiClient::Connected)
        return;
    // No contexts = global: the bridge applies it to every session
    // and re-arms it on late attaches.
    m_client->sendCommand(
        QStringLiteral("network.setCacheBehavior"),
        m_disableCache->isChecked()
            ? QByteArray("{\"cacheBehavior\":\"bypass\"}")
            : QByteArray("{\"cacheBehavior\":\"default\"}"));
}

void BidiPanel::refreshDom()
{
    m_pendingDomItem.clear();
    m_domTree->clear();
    m_domHelperContext.clear();
    // DEVT13: the tree's siblings — crumbs, styles, and any pending
    // inspect selection — die with it.  DEVT19: so do the search
    // hit list, the mutation snapshot/timer, and the resolved
    // attribute-edit node.
    updateCrumbs(nullptr);
    m_stylesView->clear();
    m_propsPath.clear();
    m_propsNodeId = 0;
    m_selectActive = false;
    m_searchPaths.clear();
    m_searchIndex = -1;
    m_domFindCount->clear();
    m_domFindResults->clear();
    m_domFindResults->hide();
    m_domLiveExpand.clear();
    m_evalSerials.clear();
    m_domLiveTimer->stop();
    updateDomStats();
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    QTreeWidgetItem *root = new QTreeWidgetItem(
        QStringList(QStringLiteral("document")));
    // DEVT22: the first children fill rewrites this to the page's
    // own <html …> markup — the root IS the documentElement (its
    // children index into documentElement's children).
    root->setData(0, DomCrumbRole, QStringLiteral("html"));
    root->setData(0, Qt::UserRole, QString());
    root->addChild(new QTreeWidgetItem(QStringList(tr("loading…"))));
    m_domTree->addTopLevelItem(root);
    ensureDomHelper(ctx);
}

void BidiPanel::ensureDomHelper(const QString &context)
{
    if (m_domHelperContext == context)
        return;
    m_domHelperContext = context;
    sendEval(context, QLatin1String(kDomHelper), TagDomInject);
}

void BidiPanel::domItemExpanded(QTreeWidgetItem *item)
{
    if (!item || item->childCount() != 1)
        return;
    if (!item->child(0)->data(0, Qt::UserRole).isNull())
        return; // real children already
    fetchDomChildren(item);
}

// DEVT19: factored out of domItemExpanded so the live refresh can
// re-pull a materialized branch — no placeholder precondition.
void BidiPanel::fetchDomChildren(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    ensureDomHelper(ctx);
    // path is digits+slashes only — safe to quote inline.
    const QString expr = QStringLiteral(
        "window.__arora_dom_children(\"%1\")").arg(path);
    const quint64 id = m_client->sendCommand(
        QStringLiteral("script.evaluate"),
        QJsonDocument(QJsonObject{
            {QStringLiteral("expression"), expr},
            {QStringLiteral("target"),
             QJsonObject{{QStringLiteral("context"), ctx}}},
            {QStringLiteral("awaitPromise"), true},
        }).toJson(QJsonDocument::Compact));
    if (id) {
        m_evalTags.insert(id, TagDomChildren);
        m_pendingDomItem.insert(id, item);
    }
}

// DEVT19: a mutation report debounced onto this — re-fetch the
// root's children, which replaces one level; the expansion
// snapshot then walks matching children back open, and each of
// those expands fetches its own level in turn.  Sibling-index
// reshuffles collapse what they must rather than pin stale rows.
void BidiPanel::liveRefreshDom()
{
    ++m_domLiveRefreshCount;
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    m_domLiveExpand.clear();
    const QLatin1Char sep(0x1f);
    QVector<QTreeWidgetItem *> stack;
    for (int i = 0; i < m_domTree->topLevelItemCount(); ++i)
        stack.append(m_domTree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        // Materialized = the lazy fetch already ran (a placeholder /
        // verdict child keeps a null path — not materialized).
        if (item->childCount() != 0
            && (item->childCount() != 1
                || !item->child(0)->data(0, Qt::UserRole).isNull())) {
            QStringList expanded;
            for (int i = 0; i < item->childCount(); ++i) {
                QTreeWidgetItem *kid = item->child(i);
                if (kid->isExpanded())
                    expanded << domItemKey(kid);
                stack.append(kid);
            }
            m_domLiveExpand.insert(
                item->data(0, Qt::UserRole).toString() + sep
                    + domItemKey(item),
                expanded);
        }
    }
    if (QTreeWidgetItem *root = m_domTree->topLevelItem(0))
        fetchDomChildren(root);
    // The selected element's styles/attributes may have moved with
    // the same mutation — re-resolve rather than show stale values.
    if (!m_propsPath.isEmpty())
        requestStylesFor(domItemForPath(m_propsPath));
    // An open find re-runs like Chrome's live result count.
    if (!m_domFind->text().isEmpty())
        m_domSearchDebounce->start();
}

// After a live-refreshed branch fills, re-expand the children that
// were open before — matched by label, so a reshuffled sibling
// still keeps its chain while a renamed element does not silently
// expand the wrong row.
void BidiPanel::restoreDomExpansion(QTreeWidgetItem *item)
{
    if (m_domLiveExpand.isEmpty() || !item)
        return;
    const QLatin1Char sep(0x1f);
    const QStringList expanded = m_domLiveExpand.value(
        item->data(0, Qt::UserRole).toString() + sep
            + domItemKey(item));
    if (expanded.isEmpty())
        return;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *kid = item->child(i);
        if (expanded.contains(domItemKey(kid)))
            kid->setExpanded(true); // fires the lazy fetch down
    }
}

// DEVT19: Chrome's Elements find — the query goes to the page-side
// helper (selector first, text fallback), which answers index paths
// in the tree's coordinate space.
void BidiPanel::runDomSearch()
{
    const QString query = m_domFind->text().trimmed();
    if (query.isEmpty()) {
        m_searchPaths.clear();
        m_searchIndex = -1;
        m_domFindCount->clear();
        m_domFindResults->clear();
        m_domFindResults->hide();
        return;
    }
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected)
        return;
    ensureDomHelper(ctx); // __arora_dom_label/markup feed the hits
    const QByteArray quoted = QJsonDocument(QJsonArray{query})
                                  .toJson(QJsonDocument::Compact);
    sendEval(ctx,
             QString::fromLatin1(kDomSearch)
                 + QStringLiteral(";window.__arora_dom_search(%1)")
                       .arg(QString::fromUtf8(
                           quoted.mid(1, quoted.size() - 2))),
             TagDomSearch);
    m_domFindCount->setText(tr("searching…"));
}

// Enter / the arrow buttons walk the hit ring; the tree materializes
// each hop lazily, so jumping to a match reuses the inspect pick's
// descend-one-level-at-a-time walk.
void BidiPanel::domSearchStep(int delta)
{
    if (m_searchPaths.isEmpty()) {
        runDomSearch();
        return;
    }
    m_searchIndex = (m_searchIndex + delta + m_searchPaths.size())
                    % m_searchPaths.size();
    m_domFindCount->setText(
        tr("%1 of %2").arg(m_searchIndex + 1)
            .arg(m_searchPaths.size()));
    {
        const QSignalBlocker hold(m_domFindResults);
        m_domFindResults->setCurrentRow(m_searchIndex);
    }
    domSelectPath(m_searchPaths.at(m_searchIndex));
}

void BidiPanel::applyDomSearchResult(const QStringList &paths,
                                     const QStringList &labels)
{
    m_searchPaths = paths;
    m_searchIndex = -1;
    m_domFindCount->setText(
        paths.isEmpty() ? tr("0 of 0") : tr("1 of %1").arg(paths.size()));
    // DEVT22: the results dock lists every hit's markup line — the
    // clicked row jumps the same way Enter/arrow cycling does.
    m_domFindResults->clear();
    for (int i = 0; i < paths.size(); ++i) {
        QListWidgetItem *row = new QListWidgetItem(
            labels.value(i, paths.at(i)));
        row->setData(Qt::UserRole, paths.at(i));
        row->setToolTip(labels.value(i, paths.at(i)));
        m_domFindResults->addItem(row);
    }
    // ~6 rows of hits — taller sets scroll inside the dock.
    const int rowH = m_domFindResults->sizeHintForRow(0);
    m_domFindResults->setMaximumHeight(
        paths.isEmpty() ? 0
                        : qMin(paths.size(), 6) * qMax(rowH, 16) + 6);
    m_domFindResults->setVisible(!paths.isEmpty());
    if (!paths.isEmpty()) {
        m_searchIndex = 0;
        {
            const QSignalBlocker hold(m_domFindResults);
            m_domFindResults->setCurrentRow(0);
        }
        domSelectPath(paths.at(0));
    }
}

void BidiPanel::domFetchFailed(QTreeWidgetItem *item,
                               const QString &why)
{
    if (!item)
        return;
    while (item->childCount())
        delete item->takeChild(0);
    QTreeWidgetItem *row = new QTreeWidgetItem(QStringList(why));
    row->setDisabled(true);
    item->addChild(row);
}

void BidiPanel::refreshStorage()
{
    const quint64 id = m_client->sendCommand(
        QStringLiteral("storage.getCookies"));
    if (id)
        m_evalTags.insert(id, TagCookies);
    const QString ctx = currentContext();
    if (!ctx.isEmpty())
        sendEval(ctx,
                 QStringLiteral("JSON.stringify(Object.keys(localStorage)"
                                ".map(function(k){return [k,"
                                "localStorage.getItem(k)]}))"),
                 TagLocalStorage);
}

// DEVT13: the Application tab's whole sweep — cookies + localStorage
// through refreshStorage, then sessionStorage and the IndexedDB
// database list, both read inside the page (the CDP Storage domain
// answers "browser context management is not supported" here, so
// evaluate is the honest route).
void BidiPanel::refreshApplication()
{
    refreshStorage();
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    sendEval(ctx,
             QStringLiteral("JSON.stringify(Object.keys(sessionStorage)"
                            ".map(function(k){return [k,"
                            "sessionStorage.getItem(k)]}))"),
             TagSessionStorage);
    sendEval(ctx,
             QStringLiteral("(window.indexedDB && indexedDB.databases)"
                            " ? indexedDB.databases().then(function(d){"
                            "return JSON.stringify(d.map(function(x){"
                            "return [x.name, x.version || '']}))})"
                            " : '[]'"),
             TagIdb);
}

void BidiPanel::onResponse(quint64 id, const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonObject root = doc.object();
    // DEVT13: arora.cdp answers carry the raw CDP result/error —
    // they route ahead of the eval envelope handling.
    if (m_cdpReqs.contains(id)) {
        onCdpResponse(id, root);
        return;
    }
    const quint64 tag = m_evalTags.take(id);

    if (root.contains(QLatin1String("error"))) {
        const QString errorText = root[QStringLiteral("error")]
                                      .toObject()[QLatin1String(
                                          "message")]
                                      .toString();
        if (tag == TagConsole)
            consoleLine(QStringLiteral("< error: "), errorText);
        // DEVT17: a refused DOM eval used to be dropped silently —
        // the branch hung on "loading…" forever.  Verdict-row it so
        // the tree reads honest (a manual re-expand retries).
        if (tag == TagDomChildren)
            domFetchFailed(m_pendingDomItem.take(id), errorText);
        if (tag == TagDomInject)
            domFetchFailed(m_domTree->topLevelItem(0), errorText);
        // DEVT19: a refused search/pick eval still gets an honest
        // landing instead of spinning forever.
        if (tag == TagDomSearch)
            applyDomSearchResult(QStringList());
        if (tag == TagDomStylesPick)
            fillComputedSection(QJsonArray(), errorText);
        return;
    }
    const QJsonObject result = root[QStringLiteral("result")].toObject();

    if (tag == 0) { // browsingContext.getTree bookkeeping — but the
        // id->tag map takes() to 0 for ANY unregistered answer, and
        // housekeeping commands (session.new / session.subscribe /
        // network.setCacheBehavior) register none.  Their result
        // carries no "contexts" — without this gate every such answer
        // wiped the context list, dropped every combo claim, and left
        // the Elements tree cleared behind them.
        if (result.contains(QLatin1String("contexts"))) {
            m_contexts = result[QStringLiteral("contexts")].toArray();
            rebuildContextCombo();
            refreshDom();
            refreshApplication();
            // DEVT13: the combo may already have pointed at this
            // context (no currentIndexChanged), so kick the probe +
            // the context-bound tabs from here too.
            probeDomains();
            refreshSources();
            refreshSecurity();
        }
        return;
    }

    // script.evaluate answers: {type:success,result:{type,value}} or
    // {type:exception,exceptionDetails:{text}}
    if (result[QStringLiteral("type")].toString()
        == QLatin1String("exception")) {
        const QString text = result[QStringLiteral("exceptionDetails")]
                                 .toObject()[QStringLiteral("text")]
                                 .toString();
        if (tag == TagConsole)
            // A thrown REPL result reads like Chrome's red
            // exception line, not a plain reply.
            addConsoleRow(LevelError, QStringLiteral("< ") + text,
                          QJsonObject(), QJsonArray(), false);
        if (tag == TagDomChildren)
            domFetchFailed(m_pendingDomItem.take(id), text);
        if (tag == TagDomInject)
            domFetchFailed(m_domTree->topLevelItem(0), text);
        if (tag == TagDomProps) {
            m_stylesView->clear();
            m_stylesView->addTopLevelItem(new QTreeWidgetItem(
                QStringList{QString(), text}));
        }
        if (tag == TagDomStylesPick)
            fillComputedSection(QJsonArray(), text);
        if (tag == TagDomSearch)
            applyDomSearchResult(QStringList());
        return;
    }
    const QJsonObject remote = result[QStringLiteral("result")].toObject();
    const QVariant value = remote[QStringLiteral("value")].toVariant();

    switch (tag) {
    case TagConsole:
        consoleLine(QStringLiteral("< "),
                    remote[QStringLiteral("type")].toString()
                        == QLatin1String("undefined")
                        ? QStringLiteral("undefined")
                        : QString::fromUtf8(
                              QJsonDocument::fromVariant(value)
                                  .toJson(QJsonDocument::Compact)));
        break;
    case TagDomInject:
        // helper installed — re-expand the root
        if (m_domTree->topLevelItemCount())
            domItemExpanded(m_domTree->topLevelItem(0));
        break;
    case TagDomChildren: {
        QTreeWidgetItem *item = m_pendingDomItem.take(id);
        if (!item)
            break;
        // DEVT19: the refill deletes this branch's items — drop any
        // still-pending fetches registered on a descendant first so
        // a slower answer can't write into freed items (a live
        // refresh re-pulling the parent outruns them).
        for (auto it = m_pendingDomItem.begin();
             it != m_pendingDomItem.end();) {
            bool dead = false;
            for (QTreeWidgetItem *q = it.value(); q; q = q->parent())
                if (q == item) {
                    dead = true;
                    break;
                }
            if (dead)
                it = m_pendingDomItem.erase(it);
            else
                ++it;
        }
        // clear the placeholder
        while (item->childCount())
            delete item->takeChild(0);
        const QString base = item->data(0, Qt::UserRole).toString();
        const QByteArray payload = value.toString().toUtf8();
        // DEVT17: the page-side helper answers 'null' when the path
        // resolves to nothing — a document with no documentElement,
        // or an element that vanished between expand and fetch.
        // Silently clearing left a childless node that read as an
        // empty tree; a verdict row keeps it honest and the null
        // path lets a re-expand retry.
        if (payload.trimmed() == "null") {
            domFetchFailed(
                item, tr("element no longer in the document"));
            break;
        }
        // DEVT22: the reply is {label, markup, children:[…]} — the
        // object's own pair rewrites this row's markup (attributes
        // mutated since the parent's fetch repaint in place) while
        // the compact label feeds crumbs/snapshot matching.  Bare
        // arrays and markup-less child objects still parse — the
        // markup is derived from the compact label then.
        const QJsonDocument treeDoc =
            QJsonDocument::fromJson(payload);
        QJsonArray kids;
        if (treeDoc.isObject()) {
            const QJsonObject self = treeDoc.object();
            const QString label =
                self[QStringLiteral("label")].toString();
            QString markup =
                self[QStringLiteral("markup")].toString();
            if (markup.isEmpty() && !label.isEmpty())
                markup = domMarkupFromLabel(label);
            if (!markup.isEmpty())
                item->setText(0, markup);
            if (!label.isEmpty())
                item->setData(0, DomCrumbRole, label);
            kids = self[QStringLiteral("children")].toArray();
        } else {
            kids = treeDoc.array();
        }
        int idx = 0;
        for (const QJsonValue &kv : kids) {
            const QJsonObject k = kv.toObject();
            const QString label =
                k[QStringLiteral("label")].toString();
            QString markup =
                k[QStringLiteral("markup")].toString();
            if (markup.isEmpty())
                markup = domMarkupFromLabel(label);
            QTreeWidgetItem *child =
                new QTreeWidgetItem(QStringList(markup));
            const QString childPath =
                base.isEmpty() ? QString::number(idx)
                               : base + QLatin1Char('/') + QString::number(idx);
            child->setData(0, Qt::UserRole, childPath);
            child->setData(0, DomCrumbRole, label);
            if (k[QStringLiteral("children")].toInt() > 0)
                child->addChild(
                    new QTreeWidgetItem(QStringList(tr("loading…"))));
            item->addChild(child);
            ++idx;
        }
        updateDomStats();
        // DEVT19: a live-refresh fill re-opens the branches the
        // snapshot recorded before the mutation wiped this level.
        restoreDomExpansion(item);
        // DEVT13: a pending click-to-inspect walk keeps descending
        // through the freshly materialized children.
        if (m_selectActive)
            advanceDomSelect();
        break;
    }
    case TagSessionStorage: {
        m_sessionStorageTable->setRowCount(0);
        const QJsonArray entries =
            QJsonDocument::fromJson(value.toString().toUtf8()).array();
        for (const QJsonValue &ev : entries) {
            const QJsonArray pair = ev.toArray();
            const int row = m_sessionStorageTable->rowCount();
            m_sessionStorageTable->insertRow(row);
            m_sessionStorageTable->setItem(
                row, 0, new QTableWidgetItem(pair.at(0).toString()));
            m_sessionStorageTable->setItem(
                row, 1, new QTableWidgetItem(pair.at(1).toString()));
        }
        break;
    }
    case TagIdb: {
        m_idbTable->setRowCount(0);
        const QJsonArray dbs =
            QJsonDocument::fromJson(value.toString().toUtf8()).array();
        for (const QJsonValue &dv : dbs) {
            const QJsonArray pair = dv.toArray();
            const int row = m_idbTable->rowCount();
            m_idbTable->insertRow(row);
            m_idbTable->setItem(
                row, 0, new QTableWidgetItem(pair.at(0).toString()));
            m_idbTable->setItem(
                row, 1, new QTableWidgetItem(
                    pair.at(1).toVariant().toString()));
        }
        break;
    }
    case TagDomProps: {
        // The no-CSS-domain fallback: [name, value] pairs straight
        // off the element (attributes + a getComputedStyle pick
        // list), labeled honestly by m_stylesTitle.  DEVT19: the
        // pane is grouped now — 'computed: x' rows split into the
        // Computed section, the rest are attributes; a stale reply
        // (another selection raced it) is dropped by serial.
        {
            const int serial = m_evalSerials.take(id);
            if (serial && serial != m_stylesSerial)
                break;
        }
        m_stylesView->clear();
        {
            const QJsonArray pairs = QJsonDocument::fromJson(
                value.toString().toUtf8()).array();
            QJsonArray attrs, computed;
            for (const QJsonValue &pv : pairs) {
                const QJsonArray pair = pv.toArray();
                const QString name = pair.at(0).toString();
                if (name.startsWith(QLatin1String("computed: "))) {
                    computed.append(QJsonArray{
                        name.mid(10), pair.at(1).toString()});
                } else {
                    // flat [name,value,…] — the getAttributes shape
                    // fillAttributesSection consumes.
                    attrs.append(pair.at(0));
                    attrs.append(pair.at(1));
                }
            }
            fillAttributesSection(attrs, QString());
            fillComputedSection(computed, QString());
            fillBoxModelSection(
                QJsonObject(), tr("requires the CDP DOM domain"));
        }
        break;
    }
    case TagDomStylesPick: {
        // The CSS domain's getComputedStyleForNode refused — the
        // page-side pick list fills the Computed section honestly
        // while Attributes/Box model keep their CDP answers.
        {
            const int serial = m_evalSerials.take(id);
            if (serial && serial != m_stylesSerial)
                break;
        }
        const QJsonArray pairs = QJsonDocument::fromJson(
            value.toString().toUtf8()).array();
        fillComputedSection(pairs, QString());
        break;
    }
    case TagDomSearch: {
        const QJsonArray hits = QJsonDocument::fromJson(
            value.toString().toUtf8()).array();
        // DEVT22: hits arrive {path, label, markup} — markup rows
        // fill the results list; bare strings (older helper, canned
        // payloads) carry the path as their own label.
        QStringList list, labels;
        for (const QJsonValue &hv : hits) {
            if (hv.isObject()) {
                const QJsonObject h = hv.toObject();
                list << h[QStringLiteral("path")].toString();
                QString markup =
                    h[QStringLiteral("markup")].toString();
                if (markup.isEmpty())
                    markup = h[QStringLiteral("label")].toString();
                labels << markup;
            } else {
                list << hv.toString();
                labels << hv.toString();
            }
        }
        applyDomSearchResult(list, labels);
        break;
    }
    case TagInspect:
        break;
    case TagLocalStorage: {
        m_localStorageTable->setRowCount(0);
        const QJsonArray entries =
            QJsonDocument::fromJson(value.toString().toUtf8()).array();
        for (const QJsonValue &ev : entries) {
            const QJsonArray pair = ev.toArray();
            const int row = m_localStorageTable->rowCount();
            m_localStorageTable->insertRow(row);
            m_localStorageTable->setItem(
                row, 0, new QTableWidgetItem(pair.at(0).toString()));
            m_localStorageTable->setItem(
                row, 1, new QTableWidgetItem(pair.at(1).toString()));
        }
        break;
    }
    case TagCookies: {
        // handled below via the cookies branch — storage.getCookies
        // answers {result:{cookies:[...]}} without an eval envelope.
        break;
    }
    default:
        break;
    }

    if (tag == TagCookies) {
        m_cookieTable->setRowCount(0);
        const QJsonArray cookies =
            root[QStringLiteral("result")].toObject()
                [QStringLiteral("cookies")].toArray();
        for (const QJsonValue &cv : cookies) {
            const QJsonObject c = cv.toObject();
            const int row = m_cookieTable->rowCount();
            m_cookieTable->insertRow(row);
            m_cookieTable->setItem(
                row, 0, new QTableWidgetItem(
                    c[QStringLiteral("name")].toString()));
            m_cookieTable->setItem(
                row, 1, new QTableWidgetItem(
                    c[QStringLiteral("value")].toString()));
            m_cookieTable->setItem(
                row, 2, new QTableWidgetItem(
                    c[QStringLiteral("domain")].toString()));
            m_cookieTable->setItem(
                row, 3, new QTableWidgetItem(
                    c[QStringLiteral("path")].toString()));
        }
    }
}

void BidiPanel::onEvent(const QByteArray &json)
{
    const QJsonObject env =
        QJsonDocument::fromJson(json).object();
    const QString method = env[QStringLiteral("method")].toString();
    const QJsonObject params = env[QStringLiteral("params")].toObject();

    if (method == QLatin1String("log.entryAdded")) {
        // The console is scoped to the selected context like
        // Chrome's frame dropdown — entries from other pages don't
        // belong here.  An empty context (a bridge that didn't
        // resolve one) still lands.
        const QString ctx =
            params[QStringLiteral("context")].toString();
        if (!ctx.isEmpty() && ctx != currentContext())
            return;
        const QString text = params[QStringLiteral("text")].toString();
        // DEVT13: the inspect overlay announces its pick on
        // console.log — it's control traffic, not a page message,
        // so it's intercepted before the console sees it.
        static const QString kPick =
            QStringLiteral("__arora_inspect_pick:");
        if (m_inspectToggle->isChecked()
            && text.startsWith(kPick)) {
            setInspecting(false);
            m_inspectToggle->setChecked(false);
            domSelectPath(text.mid(kPick.size()));
            return;
        }
        // DEVT19: the page-side MutationObserver reports on the
        // same channel — control traffic again, so it re-arms the
        // live-refresh debounce without reaching the console list.
        if (text == QLatin1String("__arora_dom_mutated")) {
            m_domLiveTimer->start();
            return;
        }
        addConsoleRow(
            consoleLevelFor(
                params[QStringLiteral("level")].toString()),
            params[QStringLiteral("text")].toString(),
            params[QStringLiteral("source")].toObject(),
            params[QStringLiteral("args")].toArray(),
            true);
        return;
    }
    if (method == QLatin1String("browsingContext.contextCreated")
        || method == QLatin1String("browsingContext.contextDestroyed")) {
        // keep the combo in sync without clobbering an in-flight tree
        m_attachRetries = 0;
        refreshContexts();
        return;
    }
    if (method == QLatin1String("browsingContext.navigationStarted")) {
        // DEVT16: the bridge forwards Target.targetInfoChanged as
        // navigationStarted — a committed navigation just moved this
        // context's url while the cached getTree row still holds the
        // old one, so a tab that simply finished loading would
        // mis-merge as "no debug target".  Patch the cache and
        // re-merge; a context the snapshot never saw needs a fresh
        // tree instead.
        const QString navCtx =
            params[QStringLiteral("context")].toString();
        const QString navUrl =
            params[QStringLiteral("url")].toString();
        bool found = false;
        for (int i = 0; i < m_contexts.size(); ++i) {
            QJsonObject c = m_contexts[i].toObject();
            if (c[QLatin1String("context")].toString() != navCtx)
                continue;
            found = true;
            if (c[QLatin1String("url")].toString() != navUrl) {
                c[QLatin1String("url")] = navUrl;
                m_contexts[i] = c;
                m_attachRetries = 0;
                rebuildContextCombo();
            }
            break;
        }
        if (!found && !navCtx.isEmpty()) {
            m_attachRetries = 0;
            refreshContexts();
        }
        if (navCtx == currentContext()) {
            // A fresh load owns fresh footer timings regardless of
            // Preserve log keeping the old request rows — and fresh
            // waterfall marker lines (DEVT09).
            m_dclMs = m_loadMs = -1.0;
            static_cast<BidiNetworkModel *>(m_networkModel)
                ->setPageTimings(-1.0, -1.0);
            // Chrome clears the request log on navigation unless
            // Preserve log is ticked.
            if (!m_preserveLog->isChecked())
                clearNetworkLog();
            // DEVT12: same for the console — its own Preserve log
            // checkbox gates the wipe.
            if (!m_consolePreserve->isChecked())
                clearConsole();
            refreshDom();
            updateNetworkSummary();
            // DEVT13: a fresh document owns fresh resources and a
            // fresh security verdict.  DEVT20: script ids die with
            // the document and the page resumes on navigation —
            // the url breakpoints survive (same session).
            refreshSources();
            refreshSecurity();
            refreshApplication();
            resetDebuggerState();
            m_selectActive = false;
        }
        return;
    }
    // DEVT07: page-load milestones (the bridge maps CDP
    // Page.domContentEventFired/loadEventFired onto these BiDi names)
    // — they feed the footer's DOMContentLoaded/Load times.
    if (method == QLatin1String("browsingContext.domContentLoaded")
        || method == QLatin1String("browsingContext.load")) {
        const QString ctx = params[QStringLiteral("context")].toString();
        const double ts =
            params[QStringLiteral("timestamp")].toDouble(-1.0);
        if (ctx.isEmpty() || ctx == currentContext()) {
            // DEVT17: the Elements tree's only re-fetch trigger used
            // to be navigationStarted — its evals race the commit and
            // land in the old or the not-yet-parsed document, so the
            // tree came back empty and stayed empty after the real
            // page parsed.  Re-fetch once the document actually
            // exists: domContentLoaded always rebuilds, load rebuilds
            // only if the tree is still stuck on the placeholder (a
            // domContentLoaded refresh could itself have raced).
            const bool dcl =
                method == QLatin1String("browsingContext.domContentLoaded");
            bool domStale = true;
            if (QTreeWidgetItem *root = m_domTree->topLevelItem(0)) {
                domStale = root->childCount() == 0
                    || (root->childCount() == 1
                        && root->child(0)->data(0, Qt::UserRole)
                               .isNull());
            }
            if (dcl || domStale)
                refreshDom();
            if (ts >= 0.0) {
                if (dcl)
                    m_dclMs = ts;
                else
                    m_loadMs = ts;
                static_cast<BidiNetworkModel *>(m_networkModel)
                    ->setPageTimings(m_dclMs, m_loadMs);
                updateNetworkSummary();
            }
            // DEVT20: resources stream in during the load — the
            // navigationStarted fetch ran before scripts/styles
            // existed, so re-pull the tree once the document lands.
            m_srcRefreshDebounce->start();
        }
        return;
    }

    // DEVT20: Sources' debugger surface — the bridge forwards the
    // raw CDP events under the "debugger" subscription group.
    if (method.startsWith(QLatin1String("Debugger."))
        || method == QLatin1String("Page.frameNavigated")) {
        onDebuggerEvent(method, params);
        return;
    }

    // network.*
    if (!method.startsWith(QLatin1String("network.")))
        return;
    // DEVT11: the record toggle is Chrome's stop — events keep
    // arriving from the bridge but nothing new is logged, so the
    // table, footer and waterfall freeze exactly as they were.
    if (!m_recordToggle->isChecked())
        return;
    BidiNetworkModel *model =
        static_cast<BidiNetworkModel *>(m_networkModel);
    const QString requestId = params[QStringLiteral("request")]
                                  .toObject()[QStringLiteral("request")]
                                  .toVariant().toString();
    // CDP MonotonicTime passed through as ms — waterfall + Time.
    const double ts = params[QStringLiteral("timestamp")].toDouble(-1.0);
    if (ts >= 0.0)
        model->noteTimestamp(ts);
    int row = m_requestRows.value(requestId, -1);
    if (method == QLatin1String("network.beforeRequestSent")) {
        const QJsonObject req = params[QStringLiteral("request")].toObject();
        // A redirect re-fires beforeRequestSent under the same id —
        // close the old hop out like Chrome, then log the new one.
        if (BidiNetworkModel::Row *old = model->rowAt(row)) {
            const int rstatus = params[QStringLiteral("redirectResponse")]
                                    .toObject()[QStringLiteral("status")]
                                    .toInt(-1);
            if (rstatus > 0)
                old->status = rstatus;
            if (old->state < BidiNetworkModel::Finished)
                old->state = BidiNetworkModel::Finished;
            if (old->endMs < 0.0)
                old->endMs = ts;
            model->rowChanged(row);
        }
        BidiNetworkModel::Row fresh;
        fresh.requestId = requestId;
        fresh.url = req[QStringLiteral("url")].toString();
        fresh.name = requestName(fresh.url);
        fresh.type = req[QStringLiteral("type")].toString();
        fresh.bucket = netBucketFor(fresh.type);
        fresh.initiator =
            initiatorText(params[QStringLiteral("initiator")].toObject());
        fresh.startMs = ts;
        row = model->addRequest(fresh);
        m_requestRows.insert(requestId, row);
        return;
    }
    BidiNetworkModel::Row *data = model->rowAt(row);
    if (!data)
        return;
    if (method == QLatin1String("network.responseStarted")) {
        const QJsonObject resp =
            params[QStringLiteral("response")].toObject();
        data->status = resp[QStringLiteral("status")].toInt(-1);
        if (data->state < BidiNetworkModel::Receiving)
            data->state = BidiNetworkModel::Receiving;
        // DEVT07: cache provenance → "(disk cache)" etc. in the Size
        // column; receiveHeadersEnd → the waterfall's wait/download
        // split.
        if (resp[QStringLiteral("fromServiceWorker")].toBool())
            data->cacheKind = BidiNetworkModel::CacheServiceWorker;
        else if (resp[QStringLiteral("fromPrefetchCache")].toBool())
            data->cacheKind = BidiNetworkModel::CachePrefetch;
        else if (resp[QStringLiteral("fromDiskCache")].toBool())
            data->cacheKind = BidiNetworkModel::CacheDisk;
        const QJsonObject timing =
            resp[QStringLiteral("timing")].toObject();
        // DEVT11: keep the whole block — the waterfall tooltip's
        // per-phase breakdown reads past receiveHeadersEnd.
        data->timing = timing;
        const double ttfb = timing[QStringLiteral(
                                     "receiveHeadersEnd")]
                                .toDouble(-1.0);
        if (ttfb >= 0.0)
            data->ttfbMs = ttfb;
        const QString type = params[QStringLiteral("type")].toString();
        if (!type.isEmpty()) {
            data->type = type;
            data->bucket = netBucketFor(type);
        }
        // DEVT07: the response's own encodedDataLength is the wire
        // bytes so far — an honest interim Size while the body loads.
        const double partial =
            resp[QStringLiteral("encodedDataLength")].toDouble(-1.0);
        if (partial >= 0.0)
            data->size = qint64(partial);
        if (resp[QStringLiteral("fromDiskCache")].toBool()
            || resp[QStringLiteral("fromPrefetchCache")].toBool()
            || resp[QStringLiteral("fromServiceWorker")].toBool())
            data->cached = true;
        model->rowChanged(row);
    } else if (method
               == QLatin1String("network.requestServedFromCache")) {
        // DEVT07: memory-cache hits arrive on their own event.
        data->cacheKind = BidiNetworkModel::CacheMemory;
        model->rowChanged(row);
    } else if (method == QLatin1String("network.responseCompleted")) {
        data->state = BidiNetworkModel::Finished;
        data->endMs = ts;
        const QJsonObject resp =
            params[QStringLiteral("response")].toObject();
        const double size = resp[QStringLiteral("encodedDataLength")]
                                .toDouble(-1.0);
        if (size >= 0.0)
            data->size = qint64(size);
        const double decoded =
            resp[QStringLiteral("decodedDataLength")].toDouble(-1.0);
        if (decoded >= 0.0)
            data->decodedSize = qint64(decoded);
        model->rowChanged(row);
    } else if (method == QLatin1String("network.fetchError")) {
        data->state = BidiNetworkModel::Failed;
        data->endMs = ts;
        data->errorText =
            params[QStringLiteral("errorText")].toString();
        const QString type = params[QStringLiteral("type")].toString();
        if (!type.isEmpty()) {
            data->type = type;
            data->bucket = netBucketFor(type);
        }
        model->rowChanged(row);
    }
}

// ===== DEVT13 — the arora.cdp passthrough and the full tab set =====

quint64 BidiPanel::sendCdp(const QString &context,
                           const QString &method,
                           const QJsonObject &params, int tag,
                           const QString &a, const QString &b, int n)
{
    QJsonObject wrapper;
    wrapper[QStringLiteral("method")] = method;
    wrapper[QStringLiteral("params")] = params;
    if (!context.isEmpty())
        wrapper[QStringLiteral("context")] = context;
    const quint64 id = m_client->sendCommand(
        QStringLiteral("arora.cdp"),
        QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
    if (id)
        m_cdpReqs.insert(id, CdpReq{tag, a, b, n});
    return id;
}

bool BidiPanel::domainUp(const QString &domain) const
{
    // Missing key = probe still in flight; a present empty string =
    // the domain answered; anything else is the CDP error text.
    return m_domainErr.contains(domain)
        && m_domainErr.value(domain).isEmpty();
}

QString BidiPanel::domainReason(const QString &domain) const
{
    if (!m_domainErr.contains(domain))
        return tr("not probed");
    return m_domainErr.value(domain);
}

void BidiPanel::probeDomains()
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected)
        return;
    if (m_probedContext == ctx)
        return;
    m_probedContext = ctx;
    m_domainErr.clear();
    for (int i = 0; i < kDomainProbeCount; ++i)
        sendCdp(ctx, QLatin1String(kDomainProbes[i].method),
                QJsonDocument::fromJson(kDomainProbes[i].params)
                    .object(),
                CdpProbe, QLatin1String(kDomainProbes[i].domain));
    // DEVT19: a probe reply that never comes used to strand the
    // feature in its constructed default — the watchdog applies
    // honest verdicts anyway.
    m_probeWatch->start();
}

// DEVT19: fire-and-forget probes assume every request gets a reply;
// a dropped answer left applyDomainSupport() waiting on 9/9 for
// ever.  Six seconds is a probe, not a page request — whatever has
// not answered by then reads "no response" instead of "dead".
void BidiPanel::probeWatchdog()
{
    if (m_probedContext.isEmpty()
        || m_domainErr.size() >= kDomainProbeCount)
        return;
    for (int i = 0; i < kDomainProbeCount; ++i) {
        const QLatin1String domain(kDomainProbes[i].domain);
        if (!m_domainErr.contains(QString(domain)))
            m_domainErr.insert(QString(domain),
                               tr("probe timed out"));
    }
    applyDomainSupport();
}

// Verdicts -> UI.  A down domain flips its feature to the
// disabled-with-reason shape: the body greys out, the offline label
// names the domain and the error, and the tab tooltip carries the
// same line.
void BidiPanel::applyDomainSupport()
{
    m_stylesTitle->setText(
        domainUp(QLatin1String("CSS"))
            ? tr("Computed styles")
            : tr("Attributes & style — CDP CSS domain unavailable: %1")
                  .arg(domainReason(QLatin1String("CSS"))));
    if (m_domTree->currentItem())
        requestStylesFor(m_domTree->currentItem());

    const bool srcUp = domainUp(QLatin1String("Page"));
    m_sourcesTree->setEnabled(srcUp);
    if (!srcUp)
        m_sourceView->setPlainText(
            tr("Resource listing requires the CDP Page domain — %1")
                .arg(domainReason(QLatin1String("Page"))));
    else if (m_sourcesTree->topLevelItemCount() == 0)
        refreshSources();

    const bool dbgUp = domainUp(QLatin1String("Debugger"));
    m_bpBar->setEnabled(dbgUp);
    m_bpBar->setToolTip(
        dbgUp ? QString()
              : tr("Requires the CDP Debugger domain — %1")
                    .arg(domainReason(QLatin1String("Debugger"))));
    m_dbgBar->setToolTip(
        dbgUp ? QString()
              : tr("Requires the CDP Debugger domain — %1")
                    .arg(domainReason(QLatin1String("Debugger"))));
    updateDebuggerButtons();

    QStringList perfMissing;
    if (!domainUp(QLatin1String("Performance")))
        perfMissing << tr("Performance (%1)")
            .arg(domainReason(QLatin1String("Performance")));
    if (!domainUp(QLatin1String("Profiler")))
        perfMissing << tr("Profiler (%1)")
            .arg(domainReason(QLatin1String("Profiler")));
    const bool perfUp = perfMissing.isEmpty();
    m_perfBody->setEnabled(perfUp);
    m_cpuToggle->setEnabled(perfUp);
    m_perfOffline->setVisible(!perfUp);
    if (perfUp) {
        m_perfOffline->clear();
        requestMetrics();
        // DEVT19: if the user was already sitting on the tab when
        // the last probe landed, the tab-change that starts the
        // poll already happened — arm it here too.
        if (m_tabs->currentWidget()
            && m_tabs->currentWidget()->objectName()
                   == QLatin1String("bidiPerfPage"))
            m_metricsTimer->start(1000);
    } else {
        m_metricsTimer->stop();
        m_perfOffline->setText(
            tr("Disabled — requires the CDP %1 domain(s), which this "
               "debug endpoint does not expose")
                .arg(perfMissing.join(tr(" and "))));
    }

    const bool memUp = domainUp(QLatin1String("HeapProfiler"));
    m_memBody->setEnabled(memUp);
    m_heapButton->setEnabled(memUp);
    m_snapButton->setEnabled(memUp);
    m_memOffline->setVisible(!memUp);
    m_memOffline->setText(
        memUp ? QString()
              : tr("Disabled — requires the CDP HeapProfiler domain, "
                   "which this debug endpoint does not expose: %1")
                    .arg(domainReason(QLatin1String("HeapProfiler"))));

    for (int i = 0; i < m_tabs->count(); ++i) {
        const QString name = m_tabs->widget(i)->objectName();
        QString tip;
        if (name == QLatin1String("bidiPerfPage") && !perfUp)
            tip = m_perfOffline->text();
        else if (name == QLatin1String("bidiMemoryPage") && !memUp)
            tip = m_memOffline->text();
        else if (name == QLatin1String("bidiSourcesPage") && !srcUp)
            tip = tr("Requires the CDP Page domain — %1")
                      .arg(domainReason(QLatin1String("Page")));
        m_tabs->setTabToolTip(i, tip);
    }
}

void BidiPanel::onCdpResponse(quint64 id, const QJsonObject &root)
{
    const CdpReq req = m_cdpReqs.take(id);
    const bool err = root.contains(QLatin1String("error"));
    const QString errText = root[QStringLiteral("error")]
                                .toObject()[QStringLiteral("message")]
                                .toString();
    const QJsonObject result =
        root[QStringLiteral("result")].toObject();
    const QString ctx = currentContext();

    switch (req.tag) {
    case CdpProbe:
        m_domainErr.insert(req.a, err ? errText : QString());
        if (int(m_domainErr.size()) >= kDomainProbeCount) {
            m_probeWatch->stop();
            applyDomainSupport();
        }
        return;

    case CdpResourceTree: {
        if (err) {
            m_sourcesTree->clear();
            new QTreeWidgetItem(m_sourcesTree,
                                QStringList(errText));
            return;
        }
        m_sourcesTree->clear();
        // frameTree: {frame:{id,url}, resources:[{url,type,mimeType}],
        // childFrames:[...]}.  The frame row itself is the Document
        // resource; its resources group by type like Chrome's left
        // pane.
        QVector<QPair<QTreeWidgetItem *, QJsonObject>> stack;
        stack.append({nullptr,
                      result[QStringLiteral("frameTree")].toObject()});
        while (!stack.isEmpty()) {
            const auto pair = stack.takeLast();
            const QJsonObject ft = pair.second;
            const QJsonObject frame =
                ft[QStringLiteral("frame")].toObject();
            const QString fid = frame[QStringLiteral("id")].toString();
            const QString furl =
                frame[QStringLiteral("url")].toString();
            QTreeWidgetItem *frameItem = new QTreeWidgetItem(
                QStringList(furl.isEmpty() ? tr("(frame)")
                                           : furl));
            if (pair.first)
                pair.first->addChild(frameItem);
            else
                m_sourcesTree->addTopLevelItem(frameItem);
            frameItem->setData(0, Qt::UserRole, furl);
            frameItem->setData(0, Qt::UserRole + 1, fid);
            frameItem->setData(0, Qt::UserRole + 2,
                               QStringLiteral("Document"));
            QMap<QString, QTreeWidgetItem *> groups;
            const QJsonArray res =
                ft[QStringLiteral("resources")].toArray();
            for (const QJsonValue &rv : res) {
                const QJsonObject r = rv.toObject();
                const QString type =
                    r[QStringLiteral("type")].toString();
                // The frame item already stands for the document.
                if (type == QLatin1String("Document"))
                    continue;
                QString group = tr("Other");
                if (type == QLatin1String("Stylesheet"))
                    group = tr("Stylesheets");
                else if (type == QLatin1String("Script"))
                    group = tr("Scripts");
                else if (type == QLatin1String("Image"))
                    group = tr("Images");
                else if (type == QLatin1String("Font"))
                    group = tr("Fonts");
                else if (type == QLatin1String("Media"))
                    group = tr("Media");
                QTreeWidgetItem *&g = groups[group];
                if (!g)
                    g = new QTreeWidgetItem(
                        QStringList(group));
                QTreeWidgetItem *item = new QTreeWidgetItem(
                    QStringList(requestName(
                        r[QStringLiteral("url")].toString())));
                item->setData(
                    0, Qt::UserRole,
                    r[QStringLiteral("url")].toString());
                item->setData(0, Qt::UserRole + 1, fid);
                item->setData(0, Qt::UserRole + 2, type);
                item->setData(
                    0, Qt::UserRole + 3,
                    r[QStringLiteral("mimeType")].toString());
                item->setToolTip(
                    0, r[QStringLiteral("url")].toString());
                g->addChild(item);
            }
            for (QTreeWidgetItem *g : groups)
                frameItem->addChild(g);
            const QJsonArray kids =
                ft[QStringLiteral("childFrames")].toArray();
            for (const QJsonValue &kv : kids)
                stack.append({frameItem, kv.toObject()});
        }
        m_sourcesTree->expandAll();
        return;
    }

    case CdpResourceContent: {
        if (err) {
            m_sourceView->setPlainText(
                tr("(%1)").arg(errText));
            return;
        }
        const QString content =
            result[QStringLiteral("content")].toString();
        const QByteArray bytes =
            result[QStringLiteral("base64Encoded")].toBool()
                ? QByteArray::fromBase64(content.toUtf8())
                : content.toUtf8();
        delete m_srcHl;
        m_srcHl = nullptr;
        m_sourceView->setPlainText(QString::fromUtf8(bytes));
        m_sourceView->setExtraSelections(
            QList<QTextEdit::ExtraSelection>());
        // The in-tree highlighter speaks markup — attach it to the
        // document-ish resources it can honestly color, leave the
        // rest plain monospace.
        const QString low = req.b.toLower();
        if (low.endsWith(QLatin1String(".html"))
            || low.endsWith(QLatin1String(".htm"))
            || low.endsWith(QLatin1String(".xhtml"))
            || low.endsWith(QLatin1String(".svg"))
            || low.endsWith(QLatin1String(".xml"))
            || low.endsWith(QLatin1String(".css")))
            m_srcHl = new SourceHighlighter(
                m_sourceView->document());
        return;
    }

    case CdpScriptSource: {
        if (err) {
            // getScriptSource failed — for the still-displayed url
            // the resource body is the honest fallback; anything
            // else lands as readable error text.
            if (!req.b.isEmpty() && req.b == m_sourceUrl
                && domainUp(QLatin1String("Page"))) {
                sendCdp(ctx, QStringLiteral("Page.getResourceContent"),
                        QJsonObject{
                            {QStringLiteral("frameId"),
                             m_sourceFrameId},
                            {QStringLiteral("url"), req.b}},
                        CdpResourceContent, m_sourceFrameId, req.b);
            } else {
                m_sourceView->setPlainText(
                    tr("(%1)").arg(errText));
            }
            return;
        }
        delete m_srcHl;
        m_srcHl = nullptr;
        m_sourceView->setPlainText(
            result[QStringLiteral("scriptSource")].toString());
        // n carries the paused line to highlight (1-based; 0 for a
        // plain resource selection, which clears instead).
        if (req.n > 0)
            highlightSourceLine(req.n);
        else
            m_sourceView->setExtraSelections(
                QList<QTextEdit::ExtraSelection>());
        return;
    }

    case CdpEvalNode: {
        if (req.n != m_stylesSerial)
            return;
        if (err) {
            // Node resolution failed — the DOM-eval fallback still
            // gives a real readout.
            if (!m_propsPath.isEmpty()) {
                const quint64 id = sendEval(
                    ctx,
                    QString::fromLatin1(kDomProps)
                        + QStringLiteral("(")
                        + QString::fromLatin1(kDomResolve)
                        + QStringLiteral("(\"%1\"))")
                              .arg(m_propsPath),
                    TagDomProps);
                if (id)
                    m_evalSerials.insert(id, req.n);
            }
            return;
        }
        const QJsonObject remote =
            result[QStringLiteral("result")].toObject();
        const QString objectId =
            remote[QStringLiteral("objectId")].toString();
        if (objectId.isEmpty()) {
            m_stylesView->clear();
            m_stylesView->addTopLevelItem(new QTreeWidgetItem(
                QStringList{QString(),
                            tr("element no longer in the document")}));
            return;
        }
        sendCdp(ctx, QStringLiteral("DOM.requestNode"),
                QJsonObject{{QStringLiteral("objectId"), objectId}},
                CdpRequestNode, QString(), QString(), req.n);
        return;
    }

    case CdpRequestNode: {
        if (req.n != m_stylesSerial)
            return;
        const int nodeId =
            result[QStringLiteral("nodeId")].toInt();
        if (err || !nodeId) {
            // Node resolution failed — the DOM-eval fallback still
            // gives a real readout.
            if (!m_propsPath.isEmpty()) {
                const quint64 id = sendEval(
                    ctx,
                    QString::fromLatin1(kDomProps)
                        + QStringLiteral("(")
                        + QString::fromLatin1(kDomResolve)
                        + QStringLiteral("(\"%1\"))")
                              .arg(m_propsPath),
                    TagDomProps);
                if (id)
                    m_evalSerials.insert(id, req.n);
            }
            return;
        }
        // DEVT19: one nodeId feeds all three sections — attributes
        // (with the resolved node parked for DOM.setAttributeValue
        // edits), computed styles, and the box model.
        m_propsNodeId = nodeId;
        m_propsNodeSerial = req.n;
        sendCdp(ctx, QStringLiteral("DOM.getAttributes"),
                QJsonObject{{QStringLiteral("nodeId"), nodeId}},
                CdpAttributes, QString(), QString(), req.n);
        sendCdp(ctx, QStringLiteral("CSS.getComputedStyleForNode"),
                QJsonObject{{QStringLiteral("nodeId"), nodeId}},
                CdpComputed, QString(), QString(), req.n);
        sendCdp(ctx, QStringLiteral("DOM.getBoxModel"),
                QJsonObject{{QStringLiteral("nodeId"), nodeId}},
                CdpBoxModel, QString(), QString(), req.n);
        return;
    }

    case CdpAttributes: {
        if (req.n != m_stylesSerial)
            return;
        if (err) {
            fillAttributesSection(QJsonArray(), errText);
            return;
        }
        fillAttributesSection(
            result[QStringLiteral("attributes")].toArray(),
            QString());
        return;
    }

    case CdpComputed: {
        if (req.n != m_stylesSerial)
            return;
        if (err) {
            // A CSS domain that probed "up" but refuses the call —
            // the page-side pick list still gives real computed
            // values for this section only.
            if (!m_propsPath.isEmpty()) {
                const quint64 id = sendEval(
                    ctx,
                    QString::fromLatin1(kDomStylesPick)
                        + QStringLiteral("(")
                        + QString::fromLatin1(kDomResolve)
                        + QStringLiteral("(\"%1\"))")
                              .arg(m_propsPath),
                    TagDomStylesPick);
                if (id)
                    m_evalSerials.insert(id, req.n);
                else
                    fillComputedSection(QJsonArray(), errText);
            }
            return;
        }
        fillComputedSection(
            result[QStringLiteral("computedStyle")].toArray(),
            QString());
        return;
    }

    case CdpBoxModel: {
        if (req.n != m_stylesSerial)
            return;
        if (err) {
            fillBoxModelSection(QJsonObject(), errText);
            return;
        }
        fillBoxModelSection(
            result[QStringLiteral("model")].toObject(), QString());
        return;
    }

    case CdpAttrSet: {
        // The MutationObserver repaint refills the section; an
        // error needs its own landing so the edit doesn't look
        // swallowed.  On success re-pull the attributes now — the
        // observer debounce would eventually do it, but the row
        // should confirm the write it claims.
        if (err) {
            consoleLine(
                QStringLiteral("< setAttributeValue failed: "),
                errText);
        } else if (m_propsNodeId
                   && m_propsNodeSerial == m_stylesSerial) {
            sendCdp(ctx, QStringLiteral("DOM.getAttributes"),
                    QJsonObject{{QStringLiteral("nodeId"),
                                 m_propsNodeId}},
                    CdpAttributes, QString(), QString(),
                    m_stylesSerial);
        }
        return;
    }

    case CdpMetrics: {
        if (err)
            return;
        const QJsonArray metrics =
            result[QStringLiteral("metrics")].toArray();
        m_metricsTable->setRowCount(0);
        for (const QJsonValue &mv : metrics) {
            const QJsonObject m = mv.toObject();
            const QString name =
                m[QStringLiteral("name")].toString();
            const double value =
                m[QStringLiteral("value")].toDouble();
            const int row = m_metricsTable->rowCount();
            m_metricsTable->insertRow(row);
            m_metricsTable->setItem(
                row, 0, new QTableWidgetItem(name));
            const bool bytes =
                name.contains(QLatin1String("Size"))
                || name.contains(QLatin1String("Heap"));
            m_metricsTable->setItem(
                row, 1, new QTableWidgetItem(
                    bytes ? formatBytes(qint64(value))
                          : QString::number(value)));
            if (name == QLatin1String("JSHeapUsedSize"))
                m_memStatus->setText(
                    tr("JS heap in use: %1")
                        .arg(formatBytes(qint64(value))));
        }
        return;
    }

    case CdpProfStart:
        if (err) {
            m_perfStatus->setText(
                tr("CPU capture unavailable: %1").arg(errText));
            m_cpuToggle->blockSignals(true);
            m_cpuToggle->setChecked(false);
            m_cpuToggle->blockSignals(false);
            m_cpuToggle->setIcon(recordDotIcon(false, palette()));
            return;
        }
        m_perfStatus->setText(tr("Recording CPU profile…"));
        m_cpuToggle->setIcon(recordDotIcon(true, palette()));
        return;

    case CdpProfStop: {
        m_cpuToggle->blockSignals(true);
        m_cpuToggle->setChecked(false);
        m_cpuToggle->blockSignals(false);
        m_cpuToggle->setIcon(recordDotIcon(false, palette()));
        if (err) {
            m_perfStatus->setText(
                tr("CPU capture failed: %1").arg(errText));
            return;
        }
        // CDP profile: nodes (call tree), samples (node ids),
        // timeDeltas (µs between samples).  Self time is the delta
        // after each sample; total time is self + descendants.
        const QJsonObject profile =
            result[QStringLiteral("profile")].toObject();
        const QJsonArray nodes =
            profile[QStringLiteral("nodes")].toArray();
        const QJsonArray samples =
            profile[QStringLiteral("samples")].toArray();
        const QJsonArray deltas =
            profile[QStringLiteral("timeDeltas")].toArray();
        QHash<int, double> selfUs;
        for (int i = 0; i < samples.size() && i < deltas.size(); ++i)
            selfUs[samples[i].toInt()] += deltas[i].toDouble();
        struct ProfNode {
            QString fn, url;
            int line = -1;
            QVector<int> kids;
            double self = 0.0;
        };
        QHash<int, ProfNode> nodeMap;
        QSet<int> isChild;
        for (const QJsonValue &nv : nodes) {
            const QJsonObject n = nv.toObject();
            const int nid = n[QStringLiteral("id")].toInt();
            ProfNode pn;
            const QJsonObject cf =
                n[QStringLiteral("callFrame")].toObject();
            pn.fn = cf[QStringLiteral("functionName")].toString();
            if (pn.fn.isEmpty())
                pn.fn = QStringLiteral("(anonymous)");
            pn.url = cf[QStringLiteral("url")].toString();
            pn.line = cf[QStringLiteral("lineNumber")].toInt(-1);
            for (const QJsonValue &kv :
                 n[QStringLiteral("children")].toArray()) {
                const int kid = kv.toInt();
                pn.kids.append(kid);
                isChild.insert(kid);
            }
            pn.self = selfUs.value(nid);
            nodeMap.insert(nid, pn);
        }
        // Totals ride a post-order walk; the call tree has no
        // cycles, so recursion is safe.
        const std::function<double(int, QSet<int> &)> total =
            [&](int nid, QSet<int> &visiting) -> double {
            if (visiting.contains(nid) || !nodeMap.contains(nid))
                return 0.0;
            visiting.insert(nid);
            double t = nodeMap[nid].self;
            for (int kid : nodeMap[nid].kids)
                t += total(kid, visiting);
            visiting.remove(nid);
            return t;
        };
        struct Row {
            QString fn, url;
            int line;
            double self, tot;
        };
        QHash<QString, int> byKey;
        QVector<Row> rows;
        for (auto it = nodeMap.begin(); it != nodeMap.end(); ++it) {
            QSet<int> visiting;
            const double tot = total(it.key(), visiting);
            const QString key = it->fn + QLatin1Char('|') + it->url
                + QLatin1Char(':') + QString::number(it->line);
            if (byKey.contains(key)) {
                Row &r = rows[byKey.value(key)];
                r.self += it->self;
                r.tot += tot;
            } else {
                byKey.insert(key, rows.size());
                rows.append(
                    {it->fn, it->url, it->line, it->self, tot});
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row &a, const Row &b) {
                      return a.self > b.self;
                  });
        m_cpuTable->setRowCount(0);
        for (int i = 0; i < rows.size() && i < 200; ++i) {
            const Row &r = rows.at(i);
            const int row = m_cpuTable->rowCount();
            m_cpuTable->insertRow(row);
            m_cpuTable->setItem(
                row, 0, new QTableWidgetItem(r.fn));
            m_cpuTable->setItem(
                row, 1, new QTableWidgetItem(
                    QStringLiteral("%1 ms").arg(r.self / 1000.0,
                                               0, 'f', 1)));
            m_cpuTable->setItem(
                row, 2, new QTableWidgetItem(
                    QStringLiteral("%1 ms").arg(r.tot / 1000.0,
                                               0, 'f', 1)));
            QString src = r.url.isEmpty()
                ? QString() : requestName(r.url);
            if (r.line >= 0)
                src += QLatin1Char(':') + QString::number(r.line);
            m_cpuTable->setItem(
                row, 3, new QTableWidgetItem(src));
        }
        m_perfStatus->setText(tr("CPU profile: %1 functions")
                                  .arg(rows.size()));
        return;
    }

    case CdpHeapStart:
        if (err) {
            m_snapshotRun = false;
            m_memStatus->setText(
                tr("Heap sampling unavailable: %1").arg(errText));
            return;
        }
        m_heapSampling = true;
        m_heapContext = ctx;
        m_memStatus->setText(
            m_snapshotRun ? tr("Collecting heap summary…")
                          : tr("Sampling allocations…"));
        QTimer::singleShot(2000, this,
                           [this] { stopHeapSample(); });
        return;

    case CdpHeapStop: {
        m_heapSampling = false;
        if (err) {
            m_snapshotRun = false;
            m_memStatus->setText(
                tr("Heap sampling failed: %1").arg(errText));
            return;
        }
        // profile.head is a tree of {callFrame, selfSize, children}
        // — flatten to self size per function.
        struct HeapRow {
            QString fn, src;
            qint64 self = 0;
        };
        QHash<QString, int> byKey;
        QVector<HeapRow> rows;
        const std::function<void(const QJsonObject &)> walk =
            [&](const QJsonObject &node) {
            const QJsonObject cf =
                node[QStringLiteral("callFrame")].toObject();
            QString fn =
                cf[QStringLiteral("functionName")].toString();
            if (fn.isEmpty())
                fn = QStringLiteral("(anonymous)");
            const QString url =
                cf[QStringLiteral("url")].toString();
            const int line =
                cf[QStringLiteral("lineNumber")].toInt(-1);
            QString src = url.isEmpty() ? QString()
                                        : requestName(url);
            if (line >= 0)
                src += QLatin1Char(':') + QString::number(line);
            const QString key = fn + QLatin1Char('|') + src;
            const qint64 self =
                qint64(node[QStringLiteral("selfSize")]
                           .toDouble());
            if (byKey.contains(key)) {
                rows[byKey.value(key)].self += self;
            } else {
                byKey.insert(key, rows.size());
                rows.append({fn, src, self});
            }
            for (const QJsonValue &kv :
                 node[QStringLiteral("children")].toArray())
                walk(kv.toObject());
        };
        walk(result[QStringLiteral("profile")]
                 .toObject()[QStringLiteral("head")].toObject());
        std::sort(rows.begin(), rows.end(),
                  [](const HeapRow &a, const HeapRow &b) {
                      return a.self > b.self;
                  });
        m_heapTable->setRowCount(0);
        for (int i = 0; i < rows.size() && i < 200; ++i) {
            const HeapRow &r = rows.at(i);
            const int row = m_heapTable->rowCount();
            m_heapTable->insertRow(row);
            m_heapTable->setItem(
                row, 0, new QTableWidgetItem(r.fn));
            m_heapTable->setItem(
                row, 1, new QTableWidgetItem(formatBytes(r.self)));
            m_heapTable->setItem(
                row, 2, new QTableWidgetItem(r.src));
        }
        // DEVT21: a snapshot run composes its final line here when
        // the getHeapUsage leg already answered — the sampling pass
        // owns the sites, the usage answer the totals.  A still-
        // pending usage reply stays a "pending" line its own reply
        // replaces; a failed one is an honest "unavailable" with the
        // CDP error, never a faked figure.
        if (m_snapshotRun) {
            m_snapStopSeen = true;
            if (m_snapTotal > 0) {
                m_snapshotRun = false;
                m_memStatus->setText(
                    tr("Heap: %1 used of %2 total — %3 allocation "
                       "sites (sampling estimate)")
                        .arg(formatBytes(m_snapUsed))
                        .arg(formatBytes(m_snapTotal))
                        .arg(rows.size()));
            } else if (!m_snapErr.isEmpty()) {
                m_snapshotRun = false;
                m_memStatus->setText(
                    tr("Allocation profile: %1 functions — heap "
                       "usage unavailable: %2")
                        .arg(rows.size())
                        .arg(m_snapErr));
            } else {
                m_memStatus->setText(
                    tr("Allocation profile: %1 functions — heap "
                       "usage pending…")
                        .arg(rows.size()));
            }
        } else {
            m_memStatus->setText(tr("Allocation profile: %1 functions")
                                     .arg(rows.size()));
        }
        return;
    }

    case CdpHeapUsage:
        // DEVT21: Runtime.getHeapUsage — the summary's used/total
        // pair.  The legs answer in either order: before the
        // sampling stop lands this reply only stashes the figures;
        // after it (m_snapStopSeen) this is the last leg and writes
        // the final line itself.
        if (err) {
            m_snapErr = errText;
            if (m_snapshotRun && m_snapStopSeen) {
                m_snapshotRun = false;
                m_memStatus->setText(
                    tr("Allocation profile: %1 functions — heap "
                       "usage unavailable: %2")
                        .arg(m_heapTable->rowCount())
                        .arg(errText));
            }
            return;
        }
        m_snapUsed = qint64(
            result[QStringLiteral("usedSize")].toDouble());
        m_snapTotal = qint64(
            result[QStringLiteral("totalSize")].toDouble());
        if (m_snapshotRun && m_snapStopSeen) {
            m_snapshotRun = false;
            m_memStatus->setText(
                tr("Heap: %1 used of %2 total — %3 allocation "
                   "sites (sampling estimate)")
                    .arg(formatBytes(m_snapUsed))
                    .arg(formatBytes(m_snapTotal))
                    .arg(m_heapTable->rowCount()));
        }
        return;

    case CdpBpSet: {
        if (err) {
            QListWidgetItem *item = new QListWidgetItem(
                tr("%1:%2 — rejected: %3")
                    .arg(requestName(req.b))
                    .arg(req.n)
                    .arg(errText));
            item->setForeground(palette().color(
                QPalette::Disabled, QPalette::Text));
            m_bpList->addItem(item);
            return;
        }
        const QString bpId =
            result[QStringLiteral("breakpointId")].toString();
        if (!bpId.isEmpty())
            m_breakpoints.insert(bpId, {req.b, req.n});
        rebuildBreakpointList();
        return;
    }

    case CdpBpRemove:
        m_breakpoints.remove(req.a);
        rebuildBreakpointList();
        return;

    case CdpGeneric:
    default:
        return;
    }
}

// DEVT22: the toolbar's right-aligned element count — how many
// real element rows the lazy tree has materialized so far.
void BidiPanel::updateDomStats()
{
    int count = 0;
    QVector<QTreeWidgetItem *> stack;
    for (int i = 0; i < m_domTree->topLevelItemCount(); ++i)
        stack.append(m_domTree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        if (!item->data(0, Qt::UserRole).isNull())
            ++count;
        for (int i = 0; i < item->childCount(); ++i)
            stack.append(item->child(i));
    }
    m_domStats->setText(
        count ? tr("%1 elements").arg(count) : QString());
}

// One probe per domain the tabs depend on — the verdicts are the
// source of truth for the honest-disabled states.
void BidiPanel::updateCrumbs(QTreeWidgetItem *item)
{
    const QList<QWidget *> kids = m_crumbBar->findChildren<QWidget *>(
        QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(kids);
    if (!item || item->data(0, Qt::UserRole).isNull())
        return;
    // Ancestor chain, root first — Chrome's bottom breadcrumb bar.
    // The buttons die with this bar's next rebuild, so plain
    // pointers are safe.
    QVector<QTreeWidgetItem *> chain;
    for (QTreeWidgetItem *i = item; i; i = i->parent())
        chain.prepend(i);
    bool first = true;
    for (QTreeWidgetItem *ptr : chain) {
        if (!first) {
            QLabel *sep = new QLabel(QStringLiteral("›"),
                                     m_crumbBar);
            sep->setStyleSheet(QStringLiteral(
                "color: palette(disabled-text);"));
            m_crumbBar->layout()->addWidget(sep);
        }
        first = false;
        QToolButton *b = new QToolButton(m_crumbBar);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        // DEVT22: Chrome's trail shows the compact selector form
        // (div#id.class), not the full markup line.
        b->setText(domItemKey(ptr));
        connect(b, &QToolButton::clicked, this,
                [this, ptr]() {
            m_domTree->setCurrentItem(ptr);
        });
        m_crumbBar->layout()->addWidget(b);
    }
}

void BidiPanel::requestStylesFor(QTreeWidgetItem *item)
{
    m_stylesView->clear();
    // DEVT19: a fresh selection invalidates the resolved node —
    // attribute edits must never land on the previous element.
    m_propsNodeId = 0;
    m_propsPath.clear();
    if (!item || item->data(0, Qt::UserRole).isNull())
        return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected)
        return;
    m_propsPath = path;
    const int serial = ++m_stylesSerial;
    if (domainUp(QLatin1String("CSS"))) {
        // path -> objectId -> nodeId -> attributes + computed +
        // box model.  Each hop is a real CDP call; a stale reply
        // (serial mismatch) is dropped in onCdpResponse.
        sendCdp(ctx, QStringLiteral("Runtime.releaseObjectGroup"),
                QJsonObject{{QStringLiteral("objectGroup"),
                             QStringLiteral("arora-elements")}},
                CdpGeneric);
        sendCdp(ctx, QStringLiteral("Runtime.evaluate"),
                QJsonObject{
                    {QStringLiteral("expression"),
                     QString::fromLatin1(kDomResolve)
                         + QStringLiteral("(\"%1\")").arg(path)},
                    {QStringLiteral("returnByValue"), false},
                    {QStringLiteral("objectGroup"),
                     QStringLiteral("arora-elements")}},
                CdpEvalNode, QString(), QString(), serial);
    } else {
        // Honest subset when the CSS domain is absent: attributes,
        // inline style, a getComputedStyle pick list.
        const quint64 id = sendEval(
            ctx,
            QString::fromLatin1(kDomProps) + QStringLiteral("(")
                + QString::fromLatin1(kDomResolve)
                + QStringLiteral("(\"%1\"))").arg(path),
            TagDomProps);
        if (id)
            m_evalSerials.insert(id, serial);
    }
}

// DEVT19: the styles pane's three groups — Chrome's Elements
// sidebar shows Attributes (as tag text), Computed styles, and the
// box model.  Sections are addressed by header text so partial
// answers can fill independently of arrival order.
QTreeWidgetItem *BidiPanel::stylesSection(const QString &header)
{
    const int role = Qt::UserRole + 4;
    for (int i = 0; i < m_stylesView->topLevelItemCount(); ++i) {
        QTreeWidgetItem *s = m_stylesView->topLevelItem(i);
        if (s->data(0, role).toString() == header)
            return s;
    }
    QTreeWidgetItem *s = new QTreeWidgetItem(QStringList(header));
    s->setData(0, role, header);
    QFont f = s->font(0);
    f.setBold(true);
    s->setFont(0, f);
    // Keep the Chrome order: Attributes, Computed styles, Box model.
    const auto rankOf = [this](const QString &h) {
        return h == tr("Attributes") ? 0
             : h == tr("Box model") ? 2 : 1;
    };
    int pos = m_stylesView->topLevelItemCount();
    for (int i = 0; i < m_stylesView->topLevelItemCount(); ++i) {
        const QString other =
            m_stylesView->topLevelItem(i)->data(0, role).toString();
        if (rankOf(other) > rankOf(header)) {
            pos = i;
            break;
        }
    }
    m_stylesView->insertTopLevelItem(pos, s);
    return s;
}

void BidiPanel::fillAttributesSection(const QJsonArray &attrs,
                                      const QString &error)
{
    QTreeWidgetItem *group = stylesSection(tr("Attributes"));
    while (group->childCount())
        delete group->takeChild(0);
    QSignalBlocker block(m_stylesView);
    for (int i = 0; i + 1 < attrs.size(); i += 2) {
        QTreeWidgetItem *row = new QTreeWidgetItem(QStringList{
            attrs.at(i).toString(), attrs.at(i + 1).toString()});
        // Editable in column 1 — DOM.setAttributeValue lands it.
        // Column 0 keeps the true name in the role so a stray edit
        // there can be put back.
        row->setData(0, Qt::UserRole, attrs.at(i).toString());
        row->setFlags(row->flags() | Qt::ItemIsEditable);
        group->addChild(row);
    }
    if (attrs.isEmpty())
        group->addChild(new QTreeWidgetItem(
            QStringList{QString(),
                        error.isEmpty() ? tr("(no attributes)") : error}));
    group->setExpanded(true);
}

void BidiPanel::fillComputedSection(const QJsonArray &computed,
                                    const QString &error)
{
    QTreeWidgetItem *group = stylesSection(tr("Computed styles"));
    while (group->childCount())
        delete group->takeChild(0);
    QSignalBlocker block(m_stylesView);
    // kDomStylesPick answers [[name,value]…] pairs;
    // CSS.getComputedStyleForNode answers [{name,value}…].
    for (const QJsonValue &cv : computed) {
        QString name, val;
        if (cv.isArray()) {
            const QJsonArray pair = cv.toArray();
            name = pair.at(0).toString();
            val = pair.at(1).toString();
        } else {
            const QJsonObject c = cv.toObject();
            name = c[QStringLiteral("name")].toString();
            val = c[QStringLiteral("value")].toString();
        }
        group->addChild(new QTreeWidgetItem(QStringList{name, val}));
    }
    if (computed.isEmpty())
        group->addChild(new QTreeWidgetItem(
            QStringList{QString(),
                        error.isEmpty() ? tr("(no values)") : error}));
    group->setExpanded(true);
}

// DEVT22: the Box model section's content is a spanned diagram row
// (Chrome's nested-boxes figure), not text rows.  The row is the
// section's only child — collapsing the header folds it like any
// other group.
QTreeWidgetItem *BidiPanel::boxModelDiagramItem(QTreeWidgetItem *group)
{
    for (int i = 0; i < group->childCount(); ++i) {
        QTreeWidgetItem *it = group->child(i);
        if (it->data(0, DomBoxRole).toBool())
            return it;
    }
    QTreeWidgetItem *row = new QTreeWidgetItem(group);
    row->setData(0, DomBoxRole, true);
    row->setFlags(Qt::ItemIsEnabled);
    BidiBoxModelView *view = new BidiBoxModelView;
    row->setSizeHint(0, QSize(0, view->sizeHint().height()));
    m_stylesView->setFirstColumnSpanned(
        group->indexOfChild(row), m_stylesView->indexFromItem(group),
        true);
    m_stylesView->setItemWidget(row, 0, view);
    return row;
}

void BidiPanel::fillBoxModelSection(const QJsonObject &model,
                                    const QString &error)
{
    QTreeWidgetItem *group = stylesSection(tr("Box model"));
    // Clear plain children but keep the spanned diagram row — the
    // view is cheap state to reuse (and survives a m_stylesView
    // ->clear() since boxModelDiagramItem recreates it on demand).
    for (int i = group->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *c = group->child(i);
        if (!c->data(0, DomBoxRole).toBool())
            delete group->takeChild(i);
    }
    QSignalBlocker block(m_stylesView);
    group->setExpanded(true);
    QTreeWidgetItem *row = boxModelDiagramItem(group);
    BidiBoxModelView *view = static_cast<BidiBoxModelView *>(
        m_stylesView->itemWidget(row, 0));
    if (!view)
        return;
    if (!error.isEmpty()) {
        view->setError(error);
        return;
    }
    // DOM.getBoxModel answers content/padding/border/margin quads —
    // [x1,y1, x2,y2, x3,y3, x4,y4].  Each ring's four edges are the
    // inset of the inner quad's bounds inside the outer's, Chrome's
    // top/right/bottom/left.
    const auto bounds = [](const QJsonArray &quad) {
        QRectF r;
        if (quad.size() < 8)
            return r;
        double minX = quad.at(0).toDouble(), maxX = minX;
        double minY = quad.at(1).toDouble(), maxY = minY;
        for (int i = 0; i + 1 < quad.size(); i += 2) {
            minX = qMin(minX, quad.at(i).toDouble());
            maxX = qMax(maxX, quad.at(i).toDouble());
            minY = qMin(minY, quad.at(i + 1).toDouble());
            maxY = qMax(maxY, quad.at(i + 1).toDouble());
        }
        return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
    };
    const auto edges = [](const QRectF &outer, const QRectF &inner) {
        // {top, right, bottom, left} — Chrome's edge order.
        return QVector<double>{
            inner.top() - outer.top(),
            outer.right() - inner.right(),
            outer.bottom() - inner.bottom(),
            inner.left() - outer.left()};
    };
    const QRectF content =
        bounds(model[QStringLiteral("content")].toArray());
    const QRectF padding =
        bounds(model[QStringLiteral("padding")].toArray());
    const QRectF border =
        bounds(model[QStringLiteral("border")].toArray());
    const QRectF margin =
        bounds(model[QStringLiteral("margin")].toArray());
    view->setModel(edges(margin, border), edges(border, padding),
                   edges(padding, content),
                   QSizeF(content.width(), content.height()));
}

// DEVT19: attribute value rows edit in place.  With a resolved CDP
// node the write lands through DOM.setAttributeValue; without one
// (the no-CSS-domain fallback pane) it goes through a page-side
// setAttribute — either way the MutationObserver-driven live
// refresh repaints the section from the element itself, so the
// pane never claims a write the DOM didn't take.
void BidiPanel::editDomAttribute(QTreeWidgetItem *item, int column)
{
    if (column != 1 || m_propsPath.isEmpty())
        return;
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    if (m_propsNodeId && m_propsNodeSerial == m_stylesSerial) {
        sendCdp(ctx, QStringLiteral("DOM.setAttributeValue"),
                QJsonObject{
                    {QStringLiteral("nodeId"), m_propsNodeId},
                    {QStringLiteral("name"), item->text(0)},
                    {QStringLiteral("value"), item->text(1)}},
                CdpAttrSet, item->text(0), QString(),
                m_stylesSerial);
        return;
    }
    const auto js = [](const QString &s) {
        const QByteArray j = QJsonDocument(QJsonArray{s})
                                 .toJson(QJsonDocument::Compact);
        return QString::fromUtf8(j.mid(1, j.size() - 2));
    };
    sendEval(ctx, QString::fromLatin1(kDomResolve)
             + QStringLiteral("(\"%1\").setAttribute(%2,%3)")
                   .arg(m_propsPath, js(item->text(0)),
                        js(item->text(1))),
             TagInspect);
}

void BidiPanel::setInspecting(bool on)
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected) {
        m_inspectToggle->blockSignals(true);
        m_inspectToggle->setChecked(false);
        m_inspectToggle->blockSignals(false);
        return;
    }
    sendEval(ctx, QLatin1String(on ? kInspectOn : kInspectOff),
             TagInspect);
}

// The inspect pick arrives as an index path — the tree only
// materializes lazily, so walk down one hop per expansion: find the
// deepest existing prefix, expand it, and let the children reply
// re-enter here.
void BidiPanel::domSelectPath(const QString &path)
{
    m_pendingSelectPath = path;
    m_selectActive = true;
    advanceDomSelect();
}

QTreeWidgetItem *BidiPanel::domItemForPath(const QString &path) const
{
    QVector<QTreeWidgetItem *> stack;
    for (int i = 0; i < m_domTree->topLevelItemCount(); ++i)
        stack.append(m_domTree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        const QVariant data = item->data(0, Qt::UserRole);
        if (!data.isNull() && data.toString() == path)
            return item;
        for (int i = 0; i < item->childCount(); ++i)
            stack.append(item->child(i));
    }
    return nullptr;
}

void BidiPanel::advanceDomSelect()
{
    if (!m_selectActive)
        return;
    const QString want = m_pendingSelectPath;
    QString prefix = want;
    QTreeWidgetItem *item = domItemForPath(prefix);
    while (!item && !prefix.isEmpty()) {
        prefix = prefix.section(QLatin1Char('/'), 0, -2);
        item = domItemForPath(prefix);
    }
    if (!item) {
        // The tree isn't even built — rebuild; the root's children
        // reply re-enters through TagDomChildren.
        if (m_domTree->topLevelItemCount() == 0) {
            refreshDom();
            m_selectActive = true;
            m_pendingSelectPath = want;
        } else {
            m_selectActive = false;
        }
        return;
    }
    if (prefix == want) {
        m_domTree->setCurrentItem(item);
        m_domTree->scrollToItem(item);
        m_selectActive = false;
        m_pendingSelectPath.clear();
        return;
    }
    // The next hop needs this node's real children.
    item->setExpanded(true);
    domItemExpanded(item);
}

void BidiPanel::refreshSources()
{
    m_sourcesTree->clear();
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected
        || !domainUp(QLatin1String("Page")))
        return;
    sendCdp(ctx, QStringLiteral("Page.getResourceTree"),
            QJsonObject(), CdpResourceTree);
}

void BidiPanel::sourceSelected(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const QString url = item->data(0, Qt::UserRole).toString();
    if (url.isEmpty())
        return;
    m_sourceUrl = url;
    m_sourceFrameId = item->data(0, Qt::UserRole + 1).toString();
    const QString type = item->data(0, Qt::UserRole + 2).toString();
    // Breakpoints make sense on resources the engine can actually
    // pause in — scripts and documents with inline script.
    const bool breakable =
        type == QLatin1String("Script")
        || type == QLatin1String("Document");
    m_bpBar->setEnabled(
        breakable && domainUp(QLatin1String("Debugger")));
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    m_sourceView->setPlainText(
        tr("Loading %1…").arg(requestName(url)));
    m_sourceView->setExtraSelections(
        QList<QTextEdit::ExtraSelection>());
    // DEVT20: a script the debugger has parsed reads its own copy —
    // the body breakpoints resolve against — falling back to the
    // resource body for everything unmapped.
    const QString scriptId = m_scriptIds.value(url);
    if (type == QLatin1String("Script") && !scriptId.isEmpty()
        && domainUp(QLatin1String("Debugger"))) {
        sendCdp(ctx, QStringLiteral("Debugger.getScriptSource"),
                QJsonObject{{QStringLiteral("scriptId"), scriptId}},
                CdpScriptSource, scriptId, url);
        return;
    }
    sendCdp(ctx, QStringLiteral("Page.getResourceContent"),
            QJsonObject{{QStringLiteral("frameId"), m_sourceFrameId},
                        {QStringLiteral("url"), url}},
            CdpResourceContent, m_sourceFrameId, url);
}

void BidiPanel::toggleBreakpoint()
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_sourceUrl.isEmpty()
        || !domainUp(QLatin1String("Debugger")))
        return;
    // The spin shows Chrome's 1-based gutter line; CDP lineNumber is
    // 0-based.  The display value is what the list keeps showing.
    const int line = m_bpLine->value();
    sendCdp(ctx, QStringLiteral("Debugger.setBreakpointByUrl"),
            QJsonObject{{QStringLiteral("url"), m_sourceUrl},
                        {QStringLiteral("lineNumber"), line - 1},
                        {QStringLiteral("columnNumber"), 0}},
            CdpBpSet, QString(), m_sourceUrl, line);
}

void BidiPanel::rebuildBreakpointList()
{
    m_bpList->clear();
    for (auto it = m_breakpoints.constBegin();
         it != m_breakpoints.constEnd(); ++it) {
        QListWidgetItem *item = new QListWidgetItem(
            tr("%1:%2").arg(requestName(it.value().first))
                       .arg(it.value().second));
        item->setData(Qt::UserRole, it.key());
        item->setToolTip(it.value().first);
        m_bpList->addItem(item);
    }
}

// ===== DEVT20 — Sources parity: the debugger surface =====

// The bridge forwards the session's raw CDP Debugger.* events (and
// Page.frameNavigated) verbatim with the context id injected.  All
// of it is scoped like the rest of the panel: events for a page the
// selector isn't showing don't move the UI.
void BidiPanel::onDebuggerEvent(const QString &method,
                                const QJsonObject &params)
{
    const QString ctx = params[QStringLiteral("context")].toString();
    if (!ctx.isEmpty() && ctx != currentContext())
        return;

    if (method == QLatin1String("Debugger.scriptParsed")) {
        const QString id =
            params[QStringLiteral("scriptId")].toString();
        const QString url = params[QStringLiteral("url")].toString();
        if (!id.isEmpty() && !url.isEmpty()) {
            m_scriptUrls.insert(id, url);
            m_scriptIds.insert(url, id);
        }
        return;
    }
    if (method == QLatin1String("Page.frameNavigated")) {
        // A frame (top or iframe) moved — the resource tree is
        // stale.  Multi-frame pages fire a burst; the debounce
        // coalesces to one refresh.
        m_srcRefreshDebounce->start();
        return;
    }
    if (method == QLatin1String("Debugger.breakpointResolved")) {
        const QString bpId =
            params[QStringLiteral("breakpointId")].toString();
        const QJsonObject loc =
            params[QStringLiteral("location")].toObject();
        const QString url = m_scriptUrls.value(
            loc[QStringLiteral("scriptId")].toString());
        if (m_breakpoints.contains(bpId) && !url.isEmpty()) {
            // Re-point the row at the real resolved url:line (CDP is
            // 0-based, the list shows 1-based).
            m_breakpoints[bpId] = {
                url, loc[QStringLiteral("lineNumber")].toInt(-1) + 1};
            rebuildBreakpointList();
        }
        return;
    }
    if (method == QLatin1String("Debugger.paused")) {
        const QJsonArray frames =
            params[QStringLiteral("callFrames")].toArray();
        m_callFrames->clear();
        for (const QJsonValue &fv : frames) {
            const QJsonObject f = fv.toObject();
            QString fn =
                f[QStringLiteral("functionName")].toString();
            if (fn.isEmpty())
                fn = tr("(anonymous function)");
            const QJsonObject loc =
                f[QStringLiteral("location")].toObject();
            QString url = f[QStringLiteral("url")].toString();
            if (url.isEmpty())
                url = m_scriptUrls.value(
                    loc[QStringLiteral("scriptId")].toString());
            const int line =
                loc[QStringLiteral("lineNumber")].toInt(-1);
            QString text = fn;
            if (!url.isEmpty()) {
                text += tr(" — %1").arg(requestName(url));
                if (line >= 0)
                    text += QLatin1Char(':')
                        + QString::number(line + 1);
            }
            QListWidgetItem *row = new QListWidgetItem(text);
            if (!url.isEmpty())
                row->setToolTip(url);
            m_callFrames->addItem(row);
        }
        QString why;
        if (!params[QStringLiteral("hitBreakpoints")].toArray()
                 .isEmpty())
            why = tr("Paused on breakpoint");
        else if (params[QStringLiteral("reason")].toString()
                 == QLatin1String("other"))
            why = tr("Paused in debugger");
        else
            why = tr("Paused: %1").arg(
                params[QStringLiteral("reason")].toString());
        setDebuggerPaused(true, why);
        if (!frames.isEmpty())
            showPausedLocation(frames.first().toObject());
        return;
    }
    if (method == QLatin1String("Debugger.resumed")) {
        setDebuggerPaused(false);
        return;
    }
}

void BidiPanel::resetDebuggerState()
{
    // Script ids die with the document/session, and so does the
    // pause chrome — clear both (url breakpoints survive an in-place
    // navigation, so m_breakpoints is deliberately not here).
    m_scriptUrls.clear();
    m_scriptIds.clear();
    setDebuggerPaused(false);
}

void BidiPanel::setDebuggerPaused(bool paused, const QString &why)
{
    m_debuggerPaused = paused;
    if (!paused) {
        m_callFrames->clear();
        m_sourceView->setExtraSelections(
            QList<QTextEdit::ExtraSelection>());
    }
    m_pauseStatus->setText(paused ? why : QString());
    m_pauseStatus->setVisible(paused);
    updateDebuggerButtons();
}

// Chrome's enable rule: Pause arms on demand; Resume and the steps
// only do something while the page is actually paused.
void BidiPanel::updateDebuggerButtons()
{
    const bool up = domainUp(QLatin1String("Debugger"));
    m_dbgPause->setEnabled(up && !m_debuggerPaused);
    m_dbgResume->setEnabled(up && m_debuggerPaused);
    m_dbgStepOver->setEnabled(up && m_debuggerPaused);
    m_dbgStepInto->setEnabled(up && m_debuggerPaused);
    m_dbgStepOut->setEnabled(up && m_debuggerPaused);
}

void BidiPanel::sendDebuggerCommand(const QString &method)
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || !domainUp(QLatin1String("Debugger")))
        return;
    // The replies carry no payload — the paused state tracks the
    // Debugger.paused/.resumed events, not the answer.
    sendCdp(ctx, method, QJsonObject(), CdpGeneric);
}

// A pause jumps the source view to the top frame's file like Chrome
// — getScriptSource serves the body (the file needn't sit in the
// resource tree) and the reply highlights the 0-based CDP line it
// carried as n.
void BidiPanel::showPausedLocation(const QJsonObject &frame)
{
    const QJsonObject loc =
        frame[QStringLiteral("location")].toObject();
    const QString scriptId =
        loc[QStringLiteral("scriptId")].toString();
    const int line0 =
        loc[QStringLiteral("lineNumber")].toInt(-1);
    const QString url = m_scriptUrls.value(
        scriptId, frame[QStringLiteral("url")].toString());
    if (!url.isEmpty())
        // Breakpoint operations follow the file the page paused in.
        m_sourceUrl = url;
    if (scriptId.isEmpty())
        return;
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    const quint64 id = sendCdp(
        ctx, QStringLiteral("Debugger.getScriptSource"),
        QJsonObject{{QStringLiteral("scriptId"), scriptId}},
        CdpScriptSource, scriptId, url, line0 + 1);
    if (id)
        m_sourceView->setPlainText(
            tr("Loading %1…").arg(requestName(url)));
}

void BidiPanel::highlightSourceLine(int line)
{
    QList<QTextEdit::ExtraSelection> sels;
    if (line > 0) {
        const QTextBlock block =
            m_sourceView->document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            QColor wash = palette().color(QPalette::Highlight);
            wash.setAlpha(70);
            sel.format.setBackground(wash);
            sel.format.setProperty(
                QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(block);
            sels << sel;
            m_sourceView->setTextCursor(QTextCursor(block));
            m_sourceView->centerCursor();
        }
    }
    m_sourceView->setExtraSelections(sels);
}

void BidiPanel::refreshSecurity()
{
    QString url;
    const QString ctx = currentContext();
    for (const QJsonValue &cv : m_contexts) {
        const QJsonObject c = cv.toObject();
        if (c[QStringLiteral("context")].toString() == ctx) {
            url = c[QStringLiteral("url")].toString();
            break;
        }
    }
    if (ctx.isEmpty() || url.isEmpty()) {
        m_secHeadline->setText(tr("No page selected"));
        m_secDetail->clear();
        m_secMixed->clear();
        return;
    }
    const QUrl u(url);
    const QString scheme = u.scheme();
    QString headline;
    QStringList detail;
    detail << tr("URL: %1").arg(url);
    if (scheme == QLatin1String("https")) {
        const QString host = u.host();
        const quint16 port = quint16(u.port(443));
        TlsVerifier *verifier = TlsVerifier::instance();
        const TlsVerifier::Status st =
            verifier->statusFor(host, port);
        switch (st) {
        case TlsVerifier::Status::Verified:
            headline = tr("Secure — certificate verified");
            break;
        case TlsVerifier::Status::Warning:
            headline = tr("Warning — certificate check failed");
            break;
        case TlsVerifier::Status::Unverified:
            headline = tr("Encrypted — chain could not be "
                          "independently verified");
            break;
        case TlsVerifier::Status::Pending:
            headline = tr("Encrypted — verification in progress");
            break;
        case TlsVerifier::Status::Refused:
            headline = tr("Encrypted — probe declined");
            break;
        case TlsVerifier::Status::None:
            headline = tr("Encrypted — no independent probe");
            break;
        }
        const QString ver = verifier->tlsVersion(host, port);
        const QString cls = verifier->errorClass(host, port);
        const QString det = verifier->detail(host, port);
        if (!ver.isEmpty())
            detail << tr("Protocol: %1").arg(ver);
        if (!cls.isEmpty())
            detail << tr("Certificate status: %1").arg(cls);
        if (!det.isEmpty())
            detail << tr("Certificate: %1").arg(det);
        if (ver.isEmpty() && cls.isEmpty() && det.isEmpty())
            detail << tr("No certificate details recorded — the "
                         "second-opinion probe has not run for this "
                         "host (Tor, proxy, or private address).");
    } else if (scheme == QLatin1String("http")) {
        headline = tr("Not secure — HTTP");
        detail << tr("The connection to %1 is not encrypted.")
                      .arg(u.host());
    } else {
        headline = tr("%1 page").arg(scheme.isEmpty()
                                        ? tr("Local")
                                        : scheme.toUpper());
        detail << tr("No transport security applies to this scheme.");
    }
    m_secHeadline->setText(headline);
    m_secDetail->setText(detail.join(QLatin1Char('\n')));

    // Mixed content, observed: count http:// rows the Network tab
    // recorded while this page was loaded.
    if (scheme == QLatin1String("https")) {
        BidiNetworkModel *model =
            static_cast<BidiNetworkModel *>(m_networkModel);
        int insecure = 0;
        for (int i = 0; i < model->rowCount(QModelIndex()); ++i) {
            if (BidiNetworkModel::Row *row = model->rowAt(i)) {
                if (row->url.startsWith(QLatin1String("http://")))
                    ++insecure;
            }
        }
        m_secMixed->setText(
            insecure
                ? tr("Mixed content: %1 insecure http:// resource(s) "
                     "observed during this page load").arg(insecure)
                : tr("No insecure subresources observed during this "
                     "page load."));
    } else {
        m_secMixed->clear();
    }
}

void BidiPanel::requestMetrics()
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected
        || !domainUp(QLatin1String("Performance")))
        return;
    sendCdp(ctx, QStringLiteral("Performance.getMetrics"),
            QJsonObject(), CdpMetrics);
}

void BidiPanel::setCpuProfiling(bool on)
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || !domainUp(QLatin1String("Profiler"))) {
        m_cpuToggle->blockSignals(true);
        m_cpuToggle->setChecked(false);
        m_cpuToggle->blockSignals(false);
        return;
    }
    if (on) {
        m_cpuTable->setRowCount(0);
        sendCdp(ctx, QStringLiteral("Profiler.start"),
                QJsonObject(), CdpProfStart);
        m_perfStatus->setText(tr("Starting CPU capture…"));
    } else {
        sendCdp(ctx, QStringLiteral("Profiler.stop"),
                QJsonObject(), CdpProfStop);
        m_perfStatus->setText(tr("Stopping…"));
    }
    m_cpuToggle->setIcon(recordDotIcon(on, palette()));
}

void BidiPanel::collectHeapSample()
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_heapSampling || m_snapshotRun
        || !domainUp(QLatin1String("HeapProfiler")))
        return;
    sendCdp(ctx, QStringLiteral("HeapProfiler.startSampling"),
            QJsonObject{{QStringLiteral("samplingInterval"), 16384}},
            CdpHeapStart);
}

void BidiPanel::stopHeapSample()
{
    if (!m_heapSampling)
        return;
    sendCdp(m_heapContext, QStringLiteral("HeapProfiler.stopSampling"),
            QJsonObject(), CdpHeapStop);
}

// DEVT21: the one-shot heap summary — Runtime.getHeapUsage gives
// the honest used/total figures and a sampling pass the top
// allocation sites; the two legs answer in either order and the
// last one composes the status line (see CdpHeapStop/CdpHeapUsage).
void BidiPanel::collectHeapSnapshot()
{
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_heapSampling || m_snapshotRun
        || !domainUp(QLatin1String("HeapProfiler")))
        return;
    m_snapshotRun = true;
    m_snapStopSeen = false;
    m_snapUsed = m_snapTotal = -1;
    m_snapErr.clear();
    sendCdp(ctx, QStringLiteral("Runtime.getHeapUsage"),
            QJsonObject(), CdpHeapUsage);
    sendCdp(ctx, QStringLiteral("HeapProfiler.startSampling"),
            QJsonObject{{QStringLiteral("samplingInterval"), 16384}},
            CdpHeapStart);
}

#ifdef AUTOTESTS
quint64 BidiPanel::testRegisterCdp(int tag, const QString &a,
                                   const QString &b, int n)
{
    const quint64 id = m_nextTag++;
    m_cdpReqs.insert(id, CdpReq{tag, a, b, n});
    return id;
}

quint64 BidiPanel::testRegisterTag(quint64 tag)
{
    const quint64 id = m_nextTag++;
    m_evalTags.insert(id, tag);
    return id;
}

quint64 BidiPanel::testDomExpand(QTreeWidgetItem *item)
{
    const quint64 id = m_nextTag++;
    m_evalTags.insert(id, TagDomChildren);
    m_pendingDomItem.insert(id, item);
    return id;
}

double BidiPanel::testWaterfallFullEnd()
{
    return static_cast<BidiNetworkModel *>(m_networkModel)->windowEnd();
}

double BidiPanel::testWaterfallDisplayEnd()
{
    return static_cast<BidiNetworkModel *>(m_networkModel)
        ->displayWindowEnd();
}

QVector<double> BidiPanel::testWaterfallTicks()
{
    BidiNetworkModel *model =
        static_cast<BidiNetworkModel *>(m_networkModel);
    // Same rect math the header ruler applies to its section.
    const int width = m_networkView->horizontalHeader()
        ->sectionSize(BidiNetworkModel::WaterfallColumn) - 4;
    return waterfallTicks(model->windowStart(),
                          model->displayWindowEnd(), width);
}

int BidiPanel::testDomLiveRefreshCount() const
{
    return m_domLiveRefreshCount;
}

QStringList BidiPanel::testSearchPaths() const
{
    return m_searchPaths;
}

int BidiPanel::testSearchIndex() const
{
    return m_searchIndex;
}
#endif
