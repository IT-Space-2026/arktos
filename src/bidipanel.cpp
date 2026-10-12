/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "bidipanel.h"

#include "bidiclient.h"

#include <qboxlayout.h>
#include <qcombobox.h>
#include <qheaderview.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qtablewidget.h>
#include <qtabwidget.h>
#include <qtreewidget.h>

// eval tags
static const quint64 TagConsole = 1;
static const quint64 TagDomInject = 2;
static const quint64 TagDomChildren = 3;
static const quint64 TagLocalStorage = 4;
static const quint64 TagCookies = 5;

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

BidiPanel::BidiPanel(QWidget *parent)
    : QDockWidget(tr("Development Tools"), parent)
    , m_client(new BidiClient(this))
    , m_nextTag(10)
{
    setObjectName(QLatin1String("bidiPanelDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

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
    QTabWidget *tabs = new QTabWidget;
    tv->addWidget(tabs);
    m_stack->addWidget(tabsPage);

    // Console
    QWidget *consolePage = new QWidget;
    QVBoxLayout *cv = new QVBoxLayout(consolePage);
    cv->setContentsMargins(0, 0, 0, 0);
    m_console = new QPlainTextEdit;
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(5000);
    cv->addWidget(m_console);
    m_evalLine = new QLineEdit;
    m_evalLine->setPlaceholderText(
        tr("Evaluate JavaScript in the selected context"));
    connect(m_evalLine, &QLineEdit::returnPressed,
            this, &BidiPanel::evaluateInput);
    cv->addWidget(m_evalLine);
    tabs->addTab(consolePage, tr("Console"));

    // DOM
    m_domTree = new QTreeWidget;
    m_domTree->setHeaderLabel(tr("DOM"));
    m_domTree->setRootIsDecorated(true);
    connect(m_domTree, &QTreeWidget::itemExpanded,
            this, &BidiPanel::domItemExpanded);
    tabs->addTab(m_domTree, tr("DOM"));

    // Network
    m_networkTable = new QTableWidget(0, 4);
    m_networkTable->setHorizontalHeaderLabels(
        {tr("Method"), tr("Status"), tr("State"), tr("URL")});
    m_networkTable->horizontalHeader()->setStretchLastSection(true);
    m_networkTable->verticalHeader()->setVisible(false);
    m_networkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs->addTab(m_networkTable, tr("Network"));

    // Storage: cookies + localStorage side by side
    QSplitter *storage = new QSplitter(Qt::Vertical);
    QWidget *cookieBox = new QWidget;
    QVBoxLayout *cbv = new QVBoxLayout(cookieBox);
    cbv->setContentsMargins(0, 0, 0, 0);
    cbv->addWidget(new QLabel(tr("Cookies")));
    m_cookieTable = new QTableWidget(0, 4);
    m_cookieTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Value"), tr("Domain"), tr("Path")});
    m_cookieTable->horizontalHeader()->setStretchLastSection(true);
    m_cookieTable->verticalHeader()->setVisible(false);
    m_cookieTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cbv->addWidget(m_cookieTable);
    storage->addWidget(cookieBox);
    QWidget *lsBox = new QWidget;
    QVBoxLayout *lv = new QVBoxLayout(lsBox);
    lv->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout *lh = new QHBoxLayout;
    lh->addWidget(new QLabel(tr("Local Storage")));
    lh->addStretch();
    QPushButton *refresh = new QPushButton(tr("Refresh"));
    connect(refresh, &QPushButton::clicked,
            this, &BidiPanel::refreshStorage);
    lh->addWidget(refresh);
    lv->addLayout(lh);
    m_localStorageTable = new QTableWidget(0, 2);
    m_localStorageTable->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
    m_localStorageTable->horizontalHeader()->setStretchLastSection(true);
    m_localStorageTable->verticalHeader()->setVisible(false);
    m_localStorageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lv->addWidget(m_localStorageTable);
    storage->addWidget(lsBox);
    tabs->addTab(storage, tr("Storage"));

    setWidget(m_stack);

    connect(m_client, &BidiClient::responseReceived,
            this, &BidiPanel::onResponse);
    connect(m_client, &BidiClient::eventReceived,
            this, &BidiPanel::onEvent);
    connect(m_client, &BidiClient::connectionChanged,
            this, &BidiPanel::onConnectionChanged);
    connect(m_contextCombo, &QComboBox::currentIndexChanged,
            this, [this](int) { refreshDom(); });

    m_stack->setCurrentIndex(0);
    m_unavailableLabel->setText(tr("Debug channel not connected."));
}

void BidiPanel::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (m_client->state() == BidiClient::Disconnected)
        m_client->connectToEngine();
}

void BidiPanel::onConnectionChanged(int state)
{
    if (state == BidiClient::Connected) {
        m_stack->setCurrentIndex(1);
        refreshContexts();
        refreshStorage();
    } else {
        m_stack->setCurrentIndex(0);
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
    m_console->appendPlainText(prefix + text);
}

void BidiPanel::evaluateInput()
{
    const QString expr = m_evalLine->text().trimmed();
    if (expr.isEmpty())
        return;
    m_evalLine->clear();
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
    m_networkTable->setRowCount(0);
    m_requestRows.clear();
}

void BidiPanel::refreshDom()
{
    m_pendingDomItem.clear();
    m_domTree->clear();
    m_domHelperContext.clear();
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

void BidiPanel::onResponse(quint64 id, const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonObject root = doc.object();
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
        const QString keep = currentContext();
        m_contextCombo->blockSignals(true);
        m_contextCombo->clear();
        const QJsonArray contexts =
            result[QStringLiteral("contexts")].toArray();
        for (const QJsonValue &cv : contexts) {
            const QJsonObject c = cv.toObject();
            const QString ctx = c[QStringLiteral("context")].toString();
            const QString label =
                c[QStringLiteral("title")].toString().isEmpty()
                    ? c[QStringLiteral("url")].toString()
                    : c[QStringLiteral("title")].toString();
            m_contextCombo->addItem(label, ctx);
        }
        const int idx = m_contextCombo->findData(keep);
        m_contextCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        m_contextCombo->blockSignals(false);
        refreshDom();
        refreshStorage();
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
            consoleLine(QStringLiteral("< "), text);
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
        break;
    }
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
        consoleLine(
            QStringLiteral("[%1] ")
                .arg(params[QStringLiteral("level")].toString()),
            params[QStringLiteral("text")].toString());
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
            == currentContext())
            refreshDom();
        return;
    }

    // network.*
    if (!method.startsWith(QLatin1String("network.")))
        return;
    const QString requestId = params[QStringLiteral("request")]
                                  .toObject()[QStringLiteral("request")]
                                  .toVariant().toString();
    int row = m_requestRows.value(requestId, -1);
    if (method == QLatin1String("network.beforeRequestSent")) {
        row = m_networkTable->rowCount();
        m_networkTable->insertRow(row);
        const QJsonObject req = params[QStringLiteral("request")].toObject();
        m_networkTable->setItem(
            row, 0, new QTableWidgetItem(
                req[QStringLiteral("method")].toString()));
        m_networkTable->setItem(row, 1, new QTableWidgetItem(QString()));
        m_networkTable->setItem(
            row, 2, new QTableWidgetItem(tr("pending")));
        m_networkTable->setItem(
            row, 3, new QTableWidgetItem(
                req[QStringLiteral("url")].toString()));
        m_requestRows.insert(requestId, row);
        if (m_networkTable->rowCount() > 1000) {
            m_networkTable->removeRow(0);
            // row indexes shift — rebuild cheaply
            for (auto it = m_requestRows.begin();
                 it != m_requestRows.end(); ++it)
                it.value() = it.value() - 1;
        }
        return;
    }
    if (row < 0)
        return;
    if (method == QLatin1String("network.responseStarted")) {
        const QJsonObject resp =
            params[QStringLiteral("response")].toObject();
        m_networkTable->setItem(
            row, 1, new QTableWidgetItem(
                QString::number(resp[QStringLiteral("status")].toInt())));
        m_networkTable->setItem(
            row, 2, new QTableWidgetItem(tr("receiving")));
    } else if (method == QLatin1String("network.responseCompleted")) {
        m_networkTable->setItem(
            row, 2, new QTableWidgetItem(tr("finished")));
    } else if (method == QLatin1String("network.fetchError")) {
        m_networkTable->setItem(
            row, 2, new QTableWidgetItem(
                tr("error: %1").arg(
                    params[QStringLiteral("errorText")].toString())));
    }
}
