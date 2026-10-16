/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef BIDIPANEL_H
#define BIDIPANEL_H

/*
 * DEVT03 — engine-neutral developer-tools dock panel.
 *
 * DEVT13's full tab set (Chrome order), all fed by the BiDi-shaped
 * channel in BidiClient plus the vendor arora.cdp passthrough that
 * carries raw CDP commands onto a page session:
 *   Elements   lazy DOM tree (the old DOM tab, renamed), Chrome's
 *              breadcrumb trail along the bottom, a click-to-inspect
 *              overlay driven by Runtime.evaluate, and a right-side
 *              styles pane — real CSS.getComputedStyleForNode output
 *              when the CSS domain answers, an honest DOM attributes/
 *              inline-style readout when it does not.
 *   Console    log.entryAdded event stream + script.evaluate REPL —
 *              DEVT12's Chromium parity pass: a structured entry list
 *              (per-level wash + icon, source url:line on the right
 *              edge, repeat-count badge for identical consecutive
 *              entries, expandable children when the event's args
 *              carry object previews), a toolbar with clear, a live
 *              text filter, Chrome's checkable "Default levels"
 *              dropdown and a console-scoped Preserve log, and a '>'
 *              prompt row with Up/Down history recall.
 *   Sources    Page.getResourceTree resource list, read-only source
 *              view fed by Page.getResourceContent, and URL:line
 *              breakpoints through Debugger.setBreakpointByUrl when
 *              the Debugger domain answers.
 *   Network    Chrome-DevTools-style request table (DEVT06): filter
 *              field, type-filter chips, Preserve log / Disable cache
 *              toggles, a Name/Status/Type/Initiator/Size/Time/
 *              Waterfall model fed by the network.* event stream, and
 *              (DEVT07) per-column minimum widths so narrow docks keep
 *              every column, a two-tone waterfall bar split at
 *              time-to-first-byte, and a Chrome-style summary footer
 *              (requests / transferred / Finish / DOMContentLoaded /
 *              Load).  DEVT11's parity pass added the record/stop
 *              capture toggle, honestly-disabled throttling and
 *              response-search controls (the bridge exposes neither),
 *              per-resource-type waterfall swatches with an ms ruler
 *              in the column header, CDP-timing phase breakdowns on
 *              hover, and grayed cache-served rows.  Bodies
 *              intentionally out of scope.
 *   Performance  live Performance.getMetrics table plus a
 *              Profiler.start/stop CPU profile summarized as top
 *              functions by self time.  Disabled-with-reason when
 *              the domains don't answer.
 *   Memory     HeapProfiler.startSampling/stopSampling allocation
 *              profile aggregated by function, plus the JS heap
 *              figure from the metrics poll.  Disabled-with-reason
 *              when HeapProfiler doesn't answer.
 *   Application  (was Storage) — a Chrome-style storage tree over
 *              cookies (storage.getCookies), localStorage and
 *              sessionStorage (script.evaluate), and an IndexedDB
 *              database list (indexedDB.databases()).
 *   Security   built from Arora's own data, no CDP needed — the
 *              page scheme, the TlsVerifier second-opinion verdict
 *              (status, TLS version, chain detail), and a mixed-
 *              content count observed from the Network rows.
 *
 * On connect (and on each context switch) the panel probes the CDP
 * domains the tabs depend on; a domain that answers "not found /
 * not implemented" flips its feature to the honest-disabled state
 * instead of a fake.
 *
 * When the debug channel is unavailable (tor window, setting off, or
 * the engine lacks it) the panel shows a readable unavailable page
 * instead of tabs.
 *
 * SIDE05: this is the CONTENT of BrowserMainWindow's m_bidiPanelDock,
 * not a dock itself — as a QDockWidget it drew a second title bar
 * inside the outer one.
 */

#include <qhash.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qpointer.h>
#include <qstringlist.h>
#include <qvector.h>
#include <qwidget.h>

class QAbstractTableModel;
class BidiClient;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSortFilterProxyModel;
class SourceHighlighter;
class QSpinBox;
class QStackedWidget;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class TabWidget;

class BidiPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BidiPanel(QWidget *parent = nullptr);

    // DEVT10: bind the owning window's tab strip so the context
    // selector lists EVERY tab — engine tabs (Servo, ...) and widget
    // tabs have no CDP target, so they join the combo as disabled
    // entries carrying the reason instead of being silently absent
    // (a missing first tab read as a bug).  The strip is not owned.
    void setTabWidget(TabWidget *tabs);

protected:
    void showEvent(QShowEvent *event) override;
    // The Network view's viewport Resize drives the Name/Waterfall
    // elastic column split (DEVT07).
    bool eventFilter(QObject *watched, QEvent *event) override;

#ifdef AUTOTESTS
public:
    // COV08: compiled into autotest binaries only — registers a
    // pending eval tag / DOM-item mapping the way sendEval and
    // domItemExpanded would, so the suite can drive the JSON
    // handlers below down every tagged branch without a live
    // debug channel (sendCommand answers 0 when disconnected).
    quint64 testRegisterTag(quint64 tag);
    quint64 testDomExpand(QTreeWidgetItem *item);
    // DEVT09: waterfall window seams — the true recording end vs the
    // dense-capped paint end, so the suite can pin the zoom math
    // without scraping pixels.
    double testWaterfallFullEnd();
    double testWaterfallDisplayEnd();
    // DEVT11: the absolute-ms tick positions the header ruler and
    // the cell gridlines draw for the current window — lets the
    // suite pin the ladder without scraping header pixels.
    QVector<double> testWaterfallTicks();
    // DEVT13: register a pending arora.cdp tag the way sendCdp
    // would, so the suite can drive raw result/error handling.
    quint64 testRegisterCdp(int tag, const QString &a = QString(),
                            const QString &b = QString(), int n = 0);
#else
private:
#endif
    void onResponse(quint64 id, const QByteArray &json);
    void onEvent(const QByteArray &json);
    void onConnectionChanged(int state);
    void evaluateInput();
    void refreshContexts();
    // DEVT17: the shared context-switch cascade — a moved selection
    // (combo pick or a rebuild landing on a different row) owns a
    // fresh milestone clock, DOM tree, domain probes, and
    // context-bound views.
    void applyContextSelection();
    // DEVT10: merge the last getTree answer with the bound tab strip —
    // one combo row per strip tab in strip order, then CDP pages with
    // no tab here.  Selection restores by context id and falls back to
    // the first ENABLED row (never a disabled non-CDP entry).
    void rebuildContextCombo();
    // DEVT16: while the channel is live, a web tab with no claimed
    // target re-asks for the tree a bounded number of times ("…
    // attaching") before the row settles on "no debug target".
    void armAttachRetry();
    void refreshDom();
    void refreshStorage();
    void domItemExpanded(QTreeWidgetItem *item);
    QString currentContext() const;
    void consoleLine(const QString &prefix, const QString &text);
    // DEVT12: one structured console row — level wash/icon, source
    // link, repeat badge, preview children.  consoleLine() funnels
    // the REPL echo/result lines through it too.
    void addConsoleRow(int level, const QString &text,
                       const QJsonObject &source,
                       const QJsonArray &args, bool dedupeable);
    void applyConsoleFilters();
    void updateConsoleLevelMask();
    void clearConsole();
    void ensureDomHelper(const QString &context);
    void sendEval(const QString &context, const QString &expression,
                  quint64 tag);
    // DEVT17: a failed/vanished DOM fetch swaps its "loading…"
    // placeholder for an honest verdict row instead of hanging the
    // branch forever; the row keeps the placeholder's null path so a
    // manual re-expand retries the fetch.
    void domFetchFailed(QTreeWidgetItem *item, const QString &why);
    void clearNetworkLog();
    void sendCacheBehavior();
    void fitNetworkColumns();
    void updateNetworkSummary();

    // DEVT13 — the full tab set.
    // arora.cdp passthrough plumbing: sendCdp returns the bidi id
    // and registers CdpReq so the raw result/error routes back to
    // the right handler in onResponse.
    struct CdpReq {
        int tag = 0;
        QString a;   // domain name / frame id / object id by tag
        QString b;   // url / extra payload
        int n = 0;   // line number, misc int
    };
    quint64 sendCdp(const QString &context, const QString &method,
                    const QJsonObject &params, int tag,
                    const QString &a = QString(),
                    const QString &b = QString(), int n = 0);
    void onCdpResponse(quint64 id, const QJsonObject &root);
    // Probe the CDP domains the tabs depend on once per context;
    // verdicts land in m_domainErr (empty string = up) and
    // applyDomainSupport() turns them into tab state.
    void probeDomains();
    void applyDomainSupport();
    bool domainUp(const QString &domain) const;
    QString domainReason(const QString &domain) const;
    // Elements: breadcrumbs under the tree, click-to-inspect
    // overlay, computed styles (or the DOM-derived fallback), and
    // select-by-path for the inspect pick.
    void updateCrumbs(QTreeWidgetItem *item);
    void requestStylesFor(QTreeWidgetItem *item);
    void setInspecting(bool on);
    void domSelectPath(const QString &path);
    void advanceDomSelect();
    QTreeWidgetItem *domItemForPath(const QString &path) const;
    // Sources / Application / Security / Performance / Memory.
    void refreshSources();
    void sourceSelected(QTreeWidgetItem *item);
    void toggleBreakpoint();
    void rebuildBreakpointList();
    void refreshApplication();
    void refreshSecurity();
    void requestMetrics();
    void setCpuProfiling(bool on);
    void collectHeapSample();
    void stopHeapSample();

    BidiClient *m_client;
    QStackedWidget *m_stack;
    QLabel *m_unavailableLabel;
    QComboBox *m_contextCombo;
    // DEVT12: the Console tab — a structured tree (message | repeat
    // badge | source link) instead of a flat text dump.
    QTreeWidget *m_console;
    QToolButton *m_clearConsole;
    QLineEdit *m_consoleFilter;
    QToolButton *m_consoleLevels;
    QCheckBox *m_consolePreserve;
    int m_consoleLevelMask = 0;
    QStringList m_evalHistory;
    QString m_evalDraft;
    int m_evalHistoryIndex = -1;
    QLineEdit *m_evalLine;
    QTreeWidget *m_domTree;
    // DEVT06: the Network tab is a model/proxy/view so the toolbar's
    // filter field and the type chips can re-slice one row set.
    QAbstractTableModel *m_networkModel;
    QSortFilterProxyModel *m_networkProxy;
    // nullptr until the Network tab is built — the Console's
    // installEventFilter(this) runs events through eventFilter()
    // first, so the member must be defined before then.
    QTableView *m_networkView = nullptr;
    QLineEdit *m_networkFilter;
    QCheckBox *m_invertFilter;
    QCheckBox *m_preserveLog;
    QCheckBox *m_disableCache;
    QToolButton *m_clearRequests;
    // DEVT11: Chrome's toolbar row — the record/stop capture toggle,
    // and the honestly-disabled throttling selector + response-body
    // search button (the debug channel exposes neither).
    QToolButton *m_recordToggle;
    QComboBox *m_throttling;
    QToolButton *m_responseSearch;
    QButtonGroup *m_typeChips;
    QLabel *m_networkSummary;

    // DEVT13: the tab strip itself, kept for verdict tooltips and
    // the tests' order assertions.
    QTabWidget *m_tabs;

    // DEVT13 — Elements (the old DOM page, plus chrome).
    QToolButton *m_inspectToggle;
    QToolButton *m_domRefresh;
    QWidget *m_crumbBar;
    QLabel *m_stylesTitle;
    QTreeWidget *m_stylesView;
    // Plain pointer: guarded by m_stylesSerial — a stale styles
    // reply is dropped before it can paint over a newer selection,
    // and refreshDom clears it with the tree.
    QTreeWidgetItem *m_propsItem = nullptr;
    QString m_pendingSelectPath;
    bool m_selectActive = false;
    // Serial so a late computed-styles reply can't paint over the
    // styles of a newer selection.
    int m_stylesSerial = 0;

    // DEVT13 — Sources.
    QTreeWidget *m_sourcesTree;
    QPlainTextEdit *m_sourceView;
    SourceHighlighter *m_srcHl = nullptr;
    QWidget *m_bpBar;
    QSpinBox *m_bpLine;
    QListWidget *m_bpList;
    QString m_sourceFrameId;
    QString m_sourceUrl;
    // breakpointId -> (url, line) — Debugger answers the real ids.
    QHash<QString, QPair<QString, int>> m_breakpoints;

    // DEVT13 — Performance.
    QWidget *m_perfBody;
    QToolButton *m_cpuToggle;
    QLabel *m_perfStatus;
    QLabel *m_perfOffline;
    QTableWidget *m_metricsTable;
    QTableWidget *m_cpuTable;
    QTimer *m_metricsTimer;

    // DEVT13 — Memory.
    QWidget *m_memBody;
    QLabel *m_memOffline;
    QToolButton *m_heapButton;
    QLabel *m_memStatus;
    QTableWidget *m_heapTable;
    bool m_heapSampling = false;
    QString m_heapContext;

    // DEVT13 — Application (was Storage).
    QTreeWidget *m_appTree;
    QStackedWidget *m_appStack;
    QTableWidget *m_cookieTable;
    QTableWidget *m_localStorageTable;
    QTableWidget *m_sessionStorageTable;
    QTableWidget *m_idbTable;

    // DEVT13 — Security.
    QLabel *m_secHeadline;
    QLabel *m_secDetail;
    QLabel *m_secMixed;

    // DEVT13: domain verdicts — domain -> "" when up, else the CDP
    // error text; a missing key means "not probed yet".
    QHash<QString, QString> m_domainErr;
    QString m_probedContext;
    QHash<quint64, CdpReq> m_cdpReqs;

    // DEVT10: the owning window's strip (not owned) and the cached
    // browsingContext.getTree answer the selector merges against.
    QPointer<TabWidget> m_tabWidget;
    QJsonArray m_contexts;
    // DEVT16: attach-retry bookkeeping — m_channelLive mirrors the
    // last connectionChanged verdict (the client state is not
    // directly readable under the test harness), m_attachPending
    // dedupes the deferred getTree, m_attachRetries spends the
    // kMaxAttachRetries budget.
    bool m_channelLive = false;
    bool m_attachPending = false;
    int m_attachRetries = 0;

    // pending eval bookkeeping: bidi id -> purpose
    QHash<quint64, quint64> m_evalTags;
    quint64 m_nextTag;
    QString m_domHelperContext;
    QHash<QString, int> m_requestRows; // requestId -> network row
    QHash<quint64, QTreeWidgetItem *> m_pendingDomItem;
    // DEVT07: page-load milestones for the summary footer (CDP
    // monotonic ms), reset on navigation/context switch.
    double m_dclMs = -1.0;
    double m_loadMs = -1.0;
    // Reentrancy guard while fitNetworkColumns / the minimum-width
    // clamp call resizeSection() inside sectionResized().
    bool m_fittingColumns = false;
};

#endif // BIDIPANEL_H
