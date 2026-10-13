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
 *            Load).  Bodies intentionally out of scope.
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

class BidiPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BidiPanel(QWidget *parent = nullptr);

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
#else
private:
#endif
    void onResponse(quint64 id, const QByteArray &json);
    void onEvent(const QByteArray &json);
    void onConnectionChanged(int state);
    void evaluateInput();
    void refreshContexts();
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
    QButtonGroup *m_typeChips;
    QLabel *m_networkSummary;
    QTableWidget *m_cookieTable;
    QTableWidget *m_localStorageTable;

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
