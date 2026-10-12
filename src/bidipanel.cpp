/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "bidipanel.h"

#include "bidiclient.h"
#include "utils/flowlayout.h"

#include <qabstractitemmodel.h>
#include <qapplication.h>
#include <qboxlayout.h>
#include <qbuttongroup.h>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qheaderview.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsortfilterproxymodel.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qstyle.h>
#include <qstyleditemdelegate.h>
#include <qtableview.h>
#include <qtablewidget.h>
#include <qtabwidget.h>
#include <qtoolbutton.h>
#include <qtreewidget.h>
#include <qurl.h>
#include <qvector.h>

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

/*
 * DEVT06 — Chrome DevTools-style Network tab.
 *
 * The tab mirrors the Chrome layout: a toolbar row (filter field,
 * Invert, Preserve log, Disable cache, Clear), a row of type-filter
 * chips, and a table whose columns match Chrome's ordering — Name,
 * Status, Type, Initiator, Size, Time, Waterfall.  One row set lives
 * in BidiNetworkModel; the filter field and chips re-slice it through
 * BidiNetworkProxy, so a chip is a filtered view, not separate data.
 * The Waterfall column is painted by a delegate against the whole
 * recording window — the bridge emits CDP MonotonicTime in ms, so the
 * bars are real timings, not decoration.
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
    // Raw-value roles — the proxy sorts/filters on these and the
    // waterfall delegate reads the timings.
    enum Role {
        StartRole = Qt::UserRole + 1,
        EndRole,
        SizeRole,
        StatusRole,
        BucketRole,
        StateRole,
        UrlRole
    };
    struct Row {
        QString requestId, url, name, type, initiator, errorText;
        int bucket = NetOther;
        int status = -1;
        int state = Pending;
        qint64 size = -1;
        double startMs = -1.0;
        double endMs = -1.0;
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
            return QVariant();
        case Qt::ForegroundRole:
            if (row.state == Failed || row.status >= 400)
                return failColor();
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
        if (m_rows.isEmpty())
            return;
        beginRemoveRows(QModelIndex(), 0, m_rows.size() - 1);
        m_rows.clear();
        endRemoveRows();
    }

    // Waterfall window: earliest start .. latest end (pending rows
    // count at their start so the scale never runs backwards).
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
        double v = -1.0;
        for (const Row &row : m_rows)
            v = qMax(v, row.endMs >= 0.0 ? row.endMs : row.startMs);
        return v;
    }

private:
    QVector<Row> m_rows;
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
        m_needle = needle;
        invalidateFilter();
    }
    void setInverted(bool inverted)
    {
        if (m_inverted == inverted)
            return;
        m_inverted = inverted;
        invalidateFilter();
    }
    void setBucket(int bucket)
    {
        if (m_bucket == bucket)
            return;
        m_bucket = bucket;
        invalidateFilter();
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

// Paints the Chrome-style start+duration bar scaled to the whole
// recording window; pending rows run to the window end, translucent.
class BidiWaterfallDelegate : public QStyledItemDelegate
{
public:
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
        const double start =
            index.data(BidiNetworkModel::StartRole).toDouble();
        if (!m_model || start < 0.0)
            return;
        const double w0 = m_model->windowStart();
        const double w1 = m_model->windowEnd();
        if (w0 < 0.0 || w1 <= w0)
            return;
        double end = index.data(BidiNetworkModel::EndRole).toDouble();
        const bool pending = end < 0.0;
        if (pending)
            end = w1;

        const QRect area = option.rect.adjusted(2, 3, -2, -3);
        const double span = qMax(1.0, w1 - w0);
        const int x = area.x()
            + int((start - w0) / span * (area.width() - 1));
        const int width = qMin(
            qMax(2, int((end - start) / span * (area.width() - 1))),
            area.right() - x + 1);
        if (width <= 0)
            return;

        const bool failed =
            index.data(BidiNetworkModel::StateRole).toInt()
            == BidiNetworkModel::Failed;
        QColor bar = failed
            ? failColor()
            : option.palette.color(QPalette::Highlight);
        if (pending)
            bar.setAlpha(120);
        painter->fillRect(QRect(x, area.y(), width, area.height()), bar);
    }

private:
    BidiNetworkModel *m_model;
};

BidiPanel::BidiPanel(QWidget *parent)
    : QWidget(parent)
    , m_client(new BidiClient(this))
    , m_nextTag(10)
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
    m_networkFilter = new QLineEdit;
    m_networkFilter->setClearButtonEnabled(true);
    m_networkFilter->setPlaceholderText(tr("Filter"));
    m_networkFilter->setMaximumWidth(200);
    m_networkFilter->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Fixed);
    nb->addWidget(m_networkFilter);
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
    nb->addStretch();
    m_clearRequests = new QToolButton;
    m_clearRequests->setAutoRaise(true);
    m_clearRequests->setIcon(
        style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearRequests->setToolTip(tr("Clear"));
    nb->addWidget(m_clearRequests);
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
    QHeaderView *netHead = m_networkView->horizontalHeader();
    netHead->setSectionResizeMode(QHeaderView::Interactive);
    netHead->setStretchLastSection(true); // Waterfall soaks the width
    netHead->resizeSection(BidiNetworkModel::NameColumn, 220);
    netHead->resizeSection(BidiNetworkModel::InitiatorColumn, 140);
    m_networkView->setItemDelegateForColumn(
        BidiNetworkModel::WaterfallColumn,
        new BidiWaterfallDelegate(
            static_cast<BidiNetworkModel *>(m_networkModel),
            m_networkView));
    nv->addWidget(m_networkView);
    tabs->addTab(networkPage, tr("Network"));

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
            this, [this](int) { refreshDom(); });

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
    connect(m_disableCache, &QCheckBox::toggled,
            this, [this](bool) { sendCacheBehavior(); });
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
        refreshStorage();
        // A reconnect is a fresh channel — the bridge forgot the
        // cache flag, so re-arm it when the box is still ticked.
        if (m_disableCache->isChecked())
            sendCacheBehavior();
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
    if (!m_preserveLog->isChecked())
        clearNetworkLog();
}

void BidiPanel::clearNetworkLog()
{
    static_cast<BidiNetworkModel *>(m_networkModel)->clear();
    m_requestRows.clear();
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
            == currentContext()) {
            // Chrome clears the request log on navigation unless
            // Preserve log is ticked.
            if (!m_preserveLog->isChecked())
                clearNetworkLog();
            refreshDom();
        }
        return;
    }

    // network.*
    if (!method.startsWith(QLatin1String("network.")))
        return;
    BidiNetworkModel *model =
        static_cast<BidiNetworkModel *>(m_networkModel);
    const QString requestId = params[QStringLiteral("request")]
                                  .toObject()[QStringLiteral("request")]
                                  .toVariant().toString();
    // CDP MonotonicTime passed through as ms — waterfall + Time.
    const double ts = params[QStringLiteral("timestamp")].toDouble(-1.0);
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
        const QString type = params[QStringLiteral("type")].toString();
        if (!type.isEmpty()) {
            data->type = type;
            data->bucket = netBucketFor(type);
        }
        model->rowChanged(row);
    } else if (method == QLatin1String("network.responseCompleted")) {
        data->state = BidiNetworkModel::Finished;
        data->endMs = ts;
        const double size = params[QStringLiteral("response")]
                                .toObject()[QStringLiteral("encodedDataLength")]
                                .toDouble(-1.0);
        if (size >= 0.0)
            data->size = qint64(size);
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

#ifdef AUTOTESTS
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
#endif
