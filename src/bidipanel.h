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
 *   Network  request rows from network.* events (url, method,
 *            status, state — bodies intentionally out of scope)
 *   Storage  cookies via storage.getCookies plus per-context
 *            localStorage via script.evaluate
 *
 * When the debug channel is unavailable (tor window, setting off, or
 * the engine lacks it) the panel shows a readable unavailable page
 * instead of tabs.
 */

#include <qdockwidget.h>
#include <qhash.h>

class BidiClient;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

class BidiPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit BidiPanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
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

    BidiClient *m_client;
    QStackedWidget *m_stack;
    QLabel *m_unavailableLabel;
    QComboBox *m_contextCombo;
    QPlainTextEdit *m_console;
    QLineEdit *m_evalLine;
    QTreeWidget *m_domTree;
    QTableWidget *m_networkTable;
    QTableWidget *m_cookieTable;
    QTableWidget *m_localStorageTable;

    // pending eval bookkeeping: bidi id -> purpose
    QHash<quint64, quint64> m_evalTags;
    quint64 m_nextTag;
    QString m_domHelperContext;
    QHash<QString, int> m_requestRows; // requestId -> network row
    QHash<quint64, QTreeWidgetItem *> m_pendingDomItem;
};

#endif // BIDIPANEL_H
