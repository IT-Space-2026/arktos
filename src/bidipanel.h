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
 * Four tabs, all fed by the BiDi-shaped channel in BidiClient:
 *   Console  log.entryAdded event stream + script.evaluate REPL
 *   DOM      lazy-loaded element tree (walking happens inside the
 *            page via a JS helper — BiDi has no DOM walker and the
 *            evaluate round-trip keeps this engine-neutral)
 *   Network  Chrome-DevTools-style request table (DEVT06): filter
 *            field, type-filter chips, Preserve log / Disable cache
 *            toggles, a Name/Status/Type/Initiator/Size/Time/
 *            Waterfall model fed by the network.* event stream, and
 *            (DEVT07) per-column minimum widths so narrow docks keep
 *            every column, a two-tone waterfall bar split at
 *            time-to-first-byte, and a Chrome-style summary footer
 *            (requests / transferred / Finish / DOMContentLoaded /
 *            Load).  DEVT11's parity pass added the record/stop
 *            capture toggle, honestly-disabled throttling and
 *            response-search controls (the bridge exposes neither),
 *            per-resource-type waterfall swatches with an ms ruler
 *            in the column header, CDP-timing phase breakdowns on
 *            hover, and grayed cache-served rows.  Bodies
 *            intentionally out of scope.
 *   Storage  cookies via storage.getCookies plus per-context
 *            localStorage via script.evaluate
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
#include <qpointer.h>
#include <qvector.h>
#include <qwidget.h>

class QAbstractTableModel;
class BidiClient;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTableWidget;
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
#else
private:
#endif
    void onResponse(quint64 id, const QByteArray &json);
    void onEvent(const QByteArray &json);
    void onConnectionChanged(int state);
    void evaluateInput();
    void refreshContexts();
    // DEVT10: merge the last getTree answer with the bound tab strip —
    // one combo row per strip tab in strip order, then CDP pages with
    // no tab here.  Selection restores by context id and falls back to
    // the first ENABLED row (never a disabled non-CDP entry).
    void rebuildContextCombo();
    void refreshDom();
    void refreshStorage();
    void domItemExpanded(QTreeWidgetItem *item);
    QString currentContext() const;
    void consoleLine(const QString &prefix, const QString &text);
    void ensureDomHelper(const QString &context);
    void sendEval(const QString &context, const QString &expression,
                  quint64 tag);
    void clearNetworkLog();
    void sendCacheBehavior();
    void fitNetworkColumns();
    void updateNetworkSummary();

    BidiClient *m_client;
    QStackedWidget *m_stack;
    QLabel *m_unavailableLabel;
    QComboBox *m_contextCombo;
    QPlainTextEdit *m_console;
    QLineEdit *m_evalLine;
    QTreeWidget *m_domTree;
    // DEVT06: the Network tab is a model/proxy/view so the toolbar's
    // filter field and the type chips can re-slice one row set.
    QAbstractTableModel *m_networkModel;
    QSortFilterProxyModel *m_networkProxy;
    QTableView *m_networkView;
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
    QTableWidget *m_cookieTable;
    QTableWidget *m_localStorageTable;

    // DEVT10: the owning window's strip (not owned) and the cached
    // browsingContext.getTree answer the selector merges against.
    QPointer<TabWidget> m_tabWidget;
    QJsonArray m_contexts;

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
