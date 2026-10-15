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
    CdpGeneric   // quiet housekeeping (e.g. Runtime.releaseObjectGroup)
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
    {"Performance", "Performance.getMetrics", "{}"},
    {"Profiler", "Profiler.enable", "{}"},
    {"HeapProfiler", "HeapProfiler.enable", "{}"},
};
static const int kDomainProbeCount =
    int(sizeof(kDomainProbes) / sizeof(kDomainProbes[0]));

// DOM walking lives in the page — BiDi (and the CDP backend we
// translate through) stays out of the layout details.  The helper
// resolves an element by an index path ("0/4/2") and returns its
// element children as JSON [{label, children}].
static const char kDomHelper[] =
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
    "    var s = c.tagName.toLowerCase();"
    "    if (c.id) s += '#' + c.id;"
    "    var cn = (typeof c.className === 'string') ? c.className.trim() : '';"
    "    if (cn) s += '.' + cn.split(/\\s+/).join('.');"
    "    var cc = 0; for (var k = c.firstElementChild; k; k = k.nextElementSibling) cc++;"
    "    out.push({label: s, children: cc}); n++;"
    "  }"
    "  return JSON.stringify(out);"
    "}, 'installed'";

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

// DEVT13: click-to-inspect — a translucent overlay chases the
// hovered element; a click swallows the event, derives the same
// index path the tree uses, and announces it on console.log (the
// one channel page->panel the bridge already relays).  The entry
// is intercepted before it reaches the console list.
static const char kInspectOn[] =
    "(function(){var st=window.__arora_inspect;"
    "if(!st){st={};window.__arora_inspect=st;"
    "var ov=document.createElement('div');"
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
    tv->addWidget(m_contextCombo);
    m_tabs = new QTabWidget;
    QTabWidget *tabs = m_tabs;
    tv->addWidget(tabs);
    m_stack->addWidget(tabsPage);

    // DEVT13 — Elements: the old DOM tab grew Chrome's chrome.  The
    // toolbar carries click-to-inspect and refresh; the right pane
    // shows computed styles (or the DOM-derived fallback); Chrome's
    // breadcrumb trail runs along the bottom edge.
    QWidget *elementsPage = new QWidget;
    QVBoxLayout *elv = new QVBoxLayout(elementsPage);
    elv->setContentsMargins(0, 0, 0, 0);
    elv->setSpacing(0);
    QWidget *elBar = new QWidget;
    elBar->setObjectName(QLatin1String("bidiElementsToolbar"));
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
    elb->addStretch();
    elv->addWidget(elBar);
    QSplitter *elSplit = new QSplitter(Qt::Horizontal);
    m_domTree = new QTreeWidget;
    m_domTree->setObjectName(QLatin1String("bidiElementsTree"));
    m_domTree->setHeaderLabel(tr("DOM"));
    m_domTree->setRootIsDecorated(true);
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
    m_stylesView->setRootIsDecorated(false);
    m_stylesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stylesView->header()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_stylesView->header()->setStretchLastSection(true);
    svl->addWidget(m_stylesView);
    elSplit->addWidget(stylesPane);
    elSplit->setStretchFactor(0, 3);
    elSplit->setStretchFactor(1, 2);
    elv->addWidget(elSplit);
    m_crumbBar = new QWidget;
    m_crumbBar->setObjectName(QLatin1String("bidiElementsCrumbs"));
    m_crumbBar->setStyleSheet(QStringLiteral(
        "border-top: 1px solid palette(mid);"));
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
    m_sourceView = new QPlainTextEdit;
    m_sourceView->setObjectName(QLatin1String("bidiSourceView"));
    m_sourceView->setReadOnly(true);
    m_sourceView->setFont(QFontDatabase::systemFont(
        QFontDatabase::FixedFont));
    srv->addWidget(m_sourceView, 1);
    // Chrome's gutter breakpoints compress to an explicit row here —
    // a QPlainTextEdit has no gutter to click.
    m_bpBar = new QWidget;
    m_bpBar->setObjectName(QLatin1String("bidiSourcesBpBar"));
    QHBoxLayout *bpl = new QHBoxLayout(m_bpBar);
    bpl->setContentsMargins(4, 2, 4, 2);
    bpl->setSpacing(4);
    bpl->addWidget(new QLabel(tr("Breakpoint at line")));
    m_bpLine = new QSpinBox;
    m_bpLine->setRange(0, 9999999);
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
        // Milestones belong to the page that fired them — a context
        // switch starts a fresh clock.
        m_dclMs = m_loadMs = -1.0;
        refreshDom();
        updateNetworkSummary();
        // DEVT13: the verdicts are per-session — a different page is
        // a different attach, so re-probe, and the Sources/Security/
        // Application views follow the new page.
        probeDomains();
        refreshSources();
        refreshSecurity();
        refreshApplication();
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
    if (state == BidiClient::Connected) {
        m_stack->setCurrentIndex(1);
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

void BidiPanel::sendEval(const QString &context, const QString &expression,
                         quint64 tag)
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

    if (m_tabWidget) {
        for (int i = 0; i < m_tabWidget->count(); ++i) {
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
            WebView *view = m_tabWidget->webView(i);
            if (!view) {
                addDisabled(title, tr("not a web page"),
                            tr("Not a web page — the devtools panel "
                               "inspects web content only"));
                continue;
            }
            int found = -1;
            for (int p = 0; p < pending.size(); ++p) {
                const QString url =
                    pending[p][QLatin1String("url")].toString();
                if (url == view->url().toString()
                    || QUrl(url) == view->url()) {
                    found = p;
                    break;
                }
            }
            if (found < 0) {
                addDisabled(title,
                            m_tabWidget->isTabSleeping(i)
                                ? tr("asleep")
                                : tr("no debug target"),
                            m_tabWidget->isTabSleeping(i)
                                ? tr("Tab is asleep — wake it to "
                                     "attach devtools")
                                : tr("Web page — not exposed on the "
                                     "debug channel"));
                continue;
            }
            const QJsonObject c = pending.takeAt(found);
            m_contextCombo->addItem(
                title.isEmpty() ? cdpLabel(c) : title,
                c[QLatin1String("context")].toString());
        }
    }

    for (const QJsonObject &c : pending) {
        m_contextCombo->addItem(
            cdpLabel(c), c[QLatin1String("context")].toString());
    }

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
    // ourselves when the effective context actually moved.
    if (now != keep) {
        m_dclMs = m_loadMs = -1.0;
        updateNetworkSummary();
    }
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
void BidiPanel::fitNetworkColumns()
{
    if (m_fittingColumns)
        return;
    QHeaderView *head = m_networkView->horizontalHeader();
    const int avail = m_networkView->viewport()->width();
    if (avail <= 0)
        return;
    int fixed = 0;
    for (int c = BidiNetworkModel::StatusColumn;
         c <= BidiNetworkModel::TimeColumn; ++c)
        fixed += head->sectionSize(c);
    const int nameMin = kNetColumnMin[BidiNetworkModel::NameColumn];
    const int fallMin = kNetColumnMin[BidiNetworkModel::WaterfallColumn];
    const int elastic = avail - fixed;
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
    m_fittingColumns = true;
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
    // inspect selection — die with it.
    updateCrumbs(nullptr);
    m_stylesView->clear();
    m_propsItem = nullptr;
    m_selectActive = false;
    const QString ctx = currentContext();
    if (ctx.isEmpty())
        return;
    QTreeWidgetItem *root = new QTreeWidgetItem(
        QStringList(QStringLiteral("document")));
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
        if (tag == TagConsole)
            consoleLine(QStringLiteral("< error: "),
                        root[QStringLiteral("error")]
                            .toObject()[QStringLiteral("message")]
                            .toString());
        return;
    }
    const QJsonObject result = root[QStringLiteral("result")].toObject();

    if (tag == 0) { // browsingContext.getTree
        m_contexts = result[QStringLiteral("contexts")].toArray();
        rebuildContextCombo();
        refreshDom();
        refreshApplication();
        // DEVT13: the combo may already have pointed at this context
        // (no currentIndexChanged), so kick the probe + the
        // context-bound tabs from here too.
        probeDomains();
        refreshSources();
        refreshSecurity();
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
        if (tag == TagDomProps) {
            m_stylesView->clear();
            m_stylesView->addTopLevelItem(new QTreeWidgetItem(
                QStringList{QString(), text}));
        }
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
        // clear the placeholder
        while (item->childCount())
            delete item->takeChild(0);
        const QString base = item->data(0, Qt::UserRole).toString();
        const QJsonArray kids =
            QJsonDocument::fromJson(value.toString().toUtf8()).array();
        int idx = 0;
        for (const QJsonValue &kv : kids) {
            const QJsonObject k = kv.toObject();
            QTreeWidgetItem *child = new QTreeWidgetItem(
                QStringList(k[QStringLiteral("label")].toString()));
            const QString childPath =
                base.isEmpty() ? QString::number(idx)
                               : base + QLatin1Char('/') + QString::number(idx);
            child->setData(0, Qt::UserRole, childPath);
            if (k[QStringLiteral("children")].toInt() > 0)
                child->addChild(
                    new QTreeWidgetItem(QStringList(tr("loading…"))));
            item->addChild(child);
            ++idx;
        }
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
        // list), labeled honestly by m_stylesTitle.
        m_stylesView->clear();
        const QJsonArray pairs =
            QJsonDocument::fromJson(value.toString().toUtf8()).array();
        for (const QJsonValue &pv : pairs) {
            const QJsonArray pair = pv.toArray();
            m_stylesView->addTopLevelItem(new QTreeWidgetItem(
                QStringList{pair.at(0).toString(),
                            pair.at(1).toString()}));
        }
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
        refreshContexts();
        return;
    }
    if (method == QLatin1String("browsingContext.navigationStarted")) {
        if (params[QStringLiteral("context")].toString()
            == currentContext()) {
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
            // fresh security verdict.
            refreshSources();
            refreshSecurity();
            refreshApplication();
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
        if (ts >= 0.0
            && (ctx.isEmpty() || ctx == currentContext())) {
            if (method == QLatin1String("browsingContext.domContentLoaded"))
                m_dclMs = ts;
            else
                m_loadMs = ts;
            static_cast<BidiNetworkModel *>(m_networkModel)
                ->setPageTimings(m_dclMs, m_loadMs);
            updateNetworkSummary();
        }
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
        if (int(m_domainErr.size()) >= kDomainProbeCount)
            applyDomainSupport();
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

    case CdpEvalNode: {
        if (err || req.n != m_stylesSerial)
            return;
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
            if (m_propsItem) {
                const QString path =
                    m_propsItem->data(0, Qt::UserRole).toString();
                sendEval(ctx,
                         QString::fromLatin1(kDomProps)
                             + QStringLiteral("(")
                             + QString::fromLatin1(kDomResolve)
                             + QStringLiteral("(\"%1\"))").arg(path),
                         TagDomProps);
            }
            return;
        }
        sendCdp(ctx, QStringLiteral("CSS.getComputedStyleForNode"),
                QJsonObject{{QStringLiteral("nodeId"), nodeId}},
                CdpComputed, QString(), QString(), req.n);
        return;
    }

    case CdpComputed: {
        if (req.n != m_stylesSerial)
            return;
        m_stylesView->clear();
        if (err) {
            if (m_propsItem) {
                const QString path =
                    m_propsItem->data(0, Qt::UserRole).toString();
                sendEval(ctx,
                         QString::fromLatin1(kDomProps)
                             + QStringLiteral("(")
                             + QString::fromLatin1(kDomResolve)
                             + QStringLiteral("(\"%1\"))").arg(path),
                         TagDomProps);
            }
            return;
        }
        const QJsonArray styles =
            result[QStringLiteral("computedStyle")].toArray();
        for (const QJsonValue &sv : styles) {
            const QJsonObject s = sv.toObject();
            m_stylesView->addTopLevelItem(new QTreeWidgetItem(
                QStringList{
                    s[QStringLiteral("name")].toString(),
                    s[QStringLiteral("value")].toString()}));
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
            m_memStatus->setText(
                tr("Heap sampling unavailable: %1").arg(errText));
            return;
        }
        m_heapSampling = true;
        m_heapContext = ctx;
        m_memStatus->setText(tr("Sampling allocations…"));
        QTimer::singleShot(2000, this,
                           [this] { stopHeapSample(); });
        return;

    case CdpHeapStop: {
        m_heapSampling = false;
        if (err) {
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
        m_memStatus->setText(tr("Allocation profile: %1 functions")
                                 .arg(rows.size()));
        return;
    }

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
        b->setText(ptr->text(0));
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
    if (!item || item->data(0, Qt::UserRole).isNull())
        return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString ctx = currentContext();
    if (ctx.isEmpty() || m_client->state() != BidiClient::Connected)
        return;
    m_propsItem = item;
    const int serial = ++m_stylesSerial;
    if (domainUp(QLatin1String("CSS"))) {
        // path -> objectId -> nodeId -> computed style.  Each hop is
        // a real CDP call; a stale reply (serial mismatch) is
        // dropped in onCdpResponse.
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
        sendEval(ctx,
                 QString::fromLatin1(kDomProps) + QStringLiteral("(")
                     + QString::fromLatin1(kDomResolve)
                     + QStringLiteral("(\"%1\"))").arg(path),
                 TagDomProps);
    }
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
    sendCdp(ctx, QStringLiteral("Debugger.setBreakpointByUrl"),
            QJsonObject{{QStringLiteral("url"), m_sourceUrl},
                        {QStringLiteral("lineNumber"),
                         m_bpLine->value()},
                        {QStringLiteral("columnNumber"), 0}},
            CdpBpSet, QString(), m_sourceUrl, m_bpLine->value());
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
    if (ctx.isEmpty() || m_heapSampling
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
#endif
