/*
 * Copyright 2009 Benjamin K. Stuhl <bks24@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "historycompleter.h"

#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "historymanager.h"
#include "safetext.h"

#ifdef ARORA_RUSTCORE
#include "rustcorebridge.h"

#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <rustcore.h>
#endif

#include <qevent.h>
#include <qfontmetrics.h>
#include <qheaderview.h>

HistoryCompletionView::HistoryCompletionView(QWidget *parent)
    : QTableView(parent)
{
    // Page titles are web-controlled; keep markup-looking text literal.
    setItemDelegate(new PlainTextItemDelegate(this));
    horizontalHeader()->hide();
    verticalHeader()->hide();

    setShowGrid(false);

    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setTextElideMode(Qt::ElideRight);

    QFontMetrics metrics = fontMetrics();
    verticalHeader()->setDefaultSectionSize(metrics.height());

    // As URLs are always LRT, this should be LRT as well
    setLayoutDirection(Qt::LeftToRight);
}

void HistoryCompletionView::resizeEvent(QResizeEvent *event)
{
    horizontalHeader()->resizeSection(0, int(0.65 * width()));
    horizontalHeader()->setStretchLastSection(true);

    QTableView::resizeEvent(event);
}

int HistoryCompletionView::sizeHintForRow(int row) const
{
    Q_UNUSED(row)
    QFontMetrics metrics = fontMetrics();
    return metrics.height();
}

HistoryCompletionModel::HistoryCompletionModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_isValid(false)
{
    setDynamicSortFilter(true);
}

QVariant HistoryCompletionModel::data(const QModelIndex &index, int role) const
{
    // if we are valid, tell QCompleter that everything we have filtered matches
    // what the user typed; if not, nothing matches
    if (role == HistoryCompletionRole && index.isValid()) {
        if (isValid())
            return QLatin1String("a");
        else
            return QLatin1String("b");
    }

    if (role == Qt::FontRole && index.column() == 1) {
        QFont font = qvariant_cast<QFont>(QSortFilterProxyModel::data(index, role));
        font.setWeight(QFont::Light);
        return font;
    }

    if (role == Qt::DisplayRole)
        role = (index.column() == 0) ? HistoryModel::UrlStringRole : HistoryModel::TitleRole;

    return QSortFilterProxyModel::data(index, role);
}

QString HistoryCompletionModel::searchString() const
{
    return m_searchString;
}

void HistoryCompletionModel::setSearchString(QString str)
{
    if (str == m_searchString)
        return;

    m_wordMatcher.setPattern(QLatin1String("\\b") + QRegularExpression::escape(str));
    m_wordMatcher.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    m_searchString = std::move(str);
#ifdef ARORA_RUSTCORE
    // OMNI01: the rustcore source re-queries its ranked rows for the
    // new term; the legacy filter model needs no such push.
    if (RustHistorySuggestModel *suggest =
            qobject_cast<RustHistorySuggestModel*>(sourceModel()))
        suggest->setTerm(m_searchString);
#endif
    beginFilterChange();
    endFilterChange();
}

bool HistoryCompletionModel::isValid() const
{
    return m_isValid;
}

void HistoryCompletionModel::setValid(bool b)
{
    if (b == m_isValid)
        return;

    m_isValid = b;

    // tell the HistoryCompleter that we've changed
    emit dataChanged(index(0, 0), index(0, rowCount() - 1));
}

bool HistoryCompletionModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
{
    // do a case-insensitive substring match against both the url and title;
    // we have also made sure that the user doesn't accidentally use regexp
    // metacharacters
    QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
    QString url = sourceModel()->data(idx, HistoryModel::UrlStringRole).toString();

    if (url.contains(m_searchString, Qt::CaseInsensitive))
        return true;

    QString title = sourceModel()->data(idx, HistoryModel::TitleRole).toString();

    if (title.contains(m_searchString, Qt::CaseInsensitive))
        return true;

    return false;
}

bool HistoryCompletionModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    // We give a bonus to hits that match on a word boundary so that e.g. "dot.kde.org"
    // is a better result for typing "dot" than "slashdot.org". However, we only look
    // for the string in the host name, not the entire url, since while it makes sense
    // to e.g. give "www.phoronix.com" a bonus for "ph", it does _not_ make sense to
    // give "www.yadda.com/foo.php" the bonus.
    int frecency_l = sourceModel()->data(left, HistoryFilterModel::FrecencyRole).toInt();
    QString url_l = sourceModel()->data(left, HistoryModel::UrlRole).toUrl().host();
    QString title_l = sourceModel()->data(left, HistoryModel::TitleRole).toString();

    if (m_wordMatcher.match(url_l).hasMatch() || m_wordMatcher.match(title_l).hasMatch())
        frecency_l *= 2;

    int frecency_r = sourceModel()->data(right, HistoryFilterModel::FrecencyRole).toInt();
    QString url_r = sourceModel()->data(right, HistoryModel::UrlRole).toUrl().host();
    QString title_r = sourceModel()->data(right, HistoryModel::TitleRole).toString();
    if (m_wordMatcher.match(url_r).hasMatch() || m_wordMatcher.match(title_r).hasMatch())
        frecency_r *= 2;

    // sort results in descending frecency-derived score
    return (frecency_r < frecency_l);
}

#ifdef ARORA_RUSTCORE
RustHistorySuggestModel::RustHistorySuggestModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    // The store is the authority: a visit landing mid-completion
    // re-queries exactly like the legacy in-memory model's
    // rowsInserted did.
    connect(RustCoreBridge::instance(), &RustCoreBridge::storeChanged,
            this, [this](const QString &topic) {
        if (topic == QLatin1String("history"))
            refresh();
    });
}

void RustHistorySuggestModel::setTerm(const QString &term)
{
    if (term == m_term)
        return;
    m_term = term;
    refresh();
}

QString RustHistorySuggestModel::term() const
{
    return m_term;
}

void RustHistorySuggestModel::refresh()
{
    beginResetModel();
    m_rows.clear();
    // The cap bounds the FFI marshal; the dropdown never shows more.
    const QByteArray t = m_term.toUtf8();
    if (char *json = rc_history_suggest(t.constData(), 500)) {
        const QJsonArray rows =
            QJsonDocument::fromJson(QByteArray(json)).array();
        rc_string_free(json);
        m_rows.reserve(rows.size());
        for (const QJsonValue &value : rows) {
            const QJsonObject o = value.toObject();
            Row row;
            row.url = o.value(QLatin1String("url")).toString();
            row.title = o.value(QLatin1String("title")).toString();
            row.ts = o.value(QLatin1String("ts")).toInteger();
            row.frecency =
                o.value(QLatin1String("frecency")).toInteger();
            m_rows.append(row);
        }
    }
    endResetModel();
}

int RustHistorySuggestModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.count();
}

int RustHistorySuggestModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 2;
}

QVariant RustHistorySuggestModel::data(const QModelIndex &index,
                                       int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.count())
        return QVariant();

    const Row &row = m_rows.at(index.row());
    // HistoryEntry::userTitle() parity — a missing title falls back
    // to the url path's filename, then the url itself.
    const QString userTitle = [&row]() {
        if (!row.title.isEmpty())
            return row.title;
        const QString page =
            QFileInfo(QUrl(row.url).path()).fileName();
        return page.isEmpty() ? row.url : page;
    }();

    switch (role) {
    case HistoryModel::DateTimeRole:
        return QDateTime::fromMSecsSinceEpoch(row.ts);
    case HistoryModel::DateRole:
        return QDateTime::fromMSecsSinceEpoch(row.ts).date();
    case HistoryModel::UrlRole:
        return QUrl(row.url);
    case HistoryModel::UrlStringRole:
    case Qt::ToolTipRole:
        return row.url;
    case HistoryModel::TitleRole:
        return userTitle;
    case HistoryFilterModel::FrecencyRole:
        // The raw sum — the completion proxy's lessThan applies the
        // word-boundary doubling itself, so the derived score matches
        // the store's ordering key.
        return row.frecency;
    case Qt::DisplayRole:
    case Qt::EditRole:
        return index.column() == 0 ? QVariant(row.url)
                                   : QVariant(userTitle);
    case Qt::DecorationRole:
        if (index.column() == 0)
            return HistoryManager::instance()->icon(QUrl(row.url));
        return QVariant();
    }
    return QVariant();
}
#endif // ARORA_RUSTCORE

OmniboxCompletionModel::OmniboxCompletionModel(
        HistoryCompletionModel *historyCompletionModel, QObject *parent)
    : QAbstractItemModel(parent)
    , m_history(historyCompletionModel)
{
    m_history->setParent(this);

    // The merged view is flat: every structural signal from the
    // history block is forwarded with the live suggestion count as the
    // row offset — but only while the history rows are actually
    // displayed.  In a bookmarks/tabs scope (SRCH06) the scoped rows
    // own the whole model and forwarding would report phantom rows.
    connect(m_history, &QAbstractItemModel::rowsAboutToBeInserted,
            this, [this](const QModelIndex &, int first, int last) {
        if (historyVisible())
            beginInsertRows(QModelIndex(), first + suggestionCount(),
                            last + suggestionCount());
    });
    connect(m_history, &QAbstractItemModel::rowsInserted,
            this, [this]() { if (historyVisible()) endInsertRows(); });
    connect(m_history, &QAbstractItemModel::rowsAboutToBeRemoved,
            this, [this](const QModelIndex &, int first, int last) {
        if (historyVisible())
            beginRemoveRows(QModelIndex(), first + suggestionCount(),
                            last + suggestionCount());
    });
    connect(m_history, &QAbstractItemModel::rowsRemoved,
            this, [this]() { if (historyVisible()) endRemoveRows(); });
    connect(m_history, &QAbstractItemModel::rowsAboutToBeMoved,
            this, [this](const QModelIndex &, int start, int end,
                         const QModelIndex &, int destination) {
        if (historyVisible())
            beginMoveRows(QModelIndex(), start + suggestionCount(),
                          end + suggestionCount(), QModelIndex(),
                          destination + suggestionCount());
    });
    connect(m_history, &QAbstractItemModel::rowsMoved,
            this, [this]() { if (historyVisible()) endMoveRows(); });
    connect(m_history, &QAbstractItemModel::modelAboutToBeReset,
            this, [this]() { if (historyVisible()) beginResetModel(); });
    connect(m_history, &QAbstractItemModel::modelReset,
            this, [this]() { if (historyVisible()) endResetModel(); });
    connect(m_history, &QAbstractItemModel::layoutAboutToBeChanged,
            this, [this]() {
        if (historyVisible())
            emit layoutAboutToBeChanged();
    });
    connect(m_history, &QAbstractItemModel::layoutChanged,
            this, [this]() {
        if (historyVisible())
            emit layoutChanged();
    });
    connect(m_history, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex &topLeft,
                         const QModelIndex &bottomRight,
                         const QList<int> &roles) {
        if (!historyVisible()) {
            // Scoped rows still read the inner model's validity flag
            // for their HistoryCompletionRole — re-report them so the
            // completer engine re-filters after setValid().
            if (rowCount() > 0)
                emit dataChanged(index(0, 0),
                                 index(rowCount() - 1,
                                       columnCount() - 1));
            return;
        }
        const int offset = suggestionCount();
        emit dataChanged(index(topLeft.row() + offset, topLeft.column()),
                         index(bottomRight.row() + offset,
                               bottomRight.column()), roles);
    });
}

HistoryCompletionModel *OmniboxCompletionModel::historyCompletionModel() const
{
    return m_history;
}

void OmniboxCompletionModel::setEngineName(const QString &name)
{
    if (name == m_engineName)
        return;
    m_engineName = name;
    if (suggestionCount() > 0)
        emit dataChanged(index(0, 1),
                         index(suggestionCount() - 1, 1));
}

QString OmniboxCompletionModel::engineName() const
{
    return m_engineName;
}

void OmniboxCompletionModel::setSuggestions(const QStringList &suggestions)
{
    // A short block: enough to serve the dropdown without burying the
    // history matches underneath.
    const QStringList capped = suggestions.mid(0, 5);
    if (capped == m_suggestions)
        return;

    const int oldCount = m_suggestions.count();
    const int newCount = capped.count();
    if (newCount > oldCount)
        beginInsertRows(QModelIndex(), oldCount, newCount - 1);
    else if (newCount < oldCount)
        beginRemoveRows(QModelIndex(), newCount, oldCount - 1);

    const int common = qMin(oldCount, newCount);
    const bool commonChanged = common > 0
        && capped.mid(0, common) != m_suggestions.mid(0, common);
    m_suggestions = capped;

    if (newCount > oldCount)
        endInsertRows();
    else if (newCount < oldCount)
        endRemoveRows();

    if (commonChanged)
        emit dataChanged(index(0, 0),
                         index(common - 1, columnCount() - 1));
}

QStringList OmniboxCompletionModel::suggestions() const
{
    return m_suggestions;
}

int OmniboxCompletionModel::suggestionCount() const
{
    return m_suggestions.count();
}

ScopeShortcuts::Scope OmniboxCompletionModel::scope() const
{
    return m_scope;
}

void OmniboxCompletionModel::setTabEntryProvider(
    std::function<QList<TabEntry>()> provider)
{
    m_tabEntryProvider = std::move(provider);
}

bool OmniboxCompletionModel::historyVisible() const
{
    return m_scope == ScopeShortcuts::NoScope
        || m_scope == ScopeShortcuts::HistoryScope;
}

void OmniboxCompletionModel::setSearchText(const QString &text)
{
    QString rest;
    const ScopeShortcuts::Scope scope = ScopeShortcuts::parse(text, &rest);

    if (scope != m_scope) {
        beginResetModel();
        m_scope = scope;
        // Scoped completions are app-side rows only — stale engine
        // suggestions must not leak into the scoped view.
        if (scope != ScopeShortcuts::NoScope)
            m_suggestions.clear();
        else
            m_scopedRows.clear();
        endResetModel();
    }

    switch (m_scope) {
    case ScopeShortcuts::BookmarksScope:
    case ScopeShortcuts::TabsScope:
        rebuildScopedRows(rest);
        break;
    case ScopeShortcuts::HistoryScope:
        m_history->setSearchString(rest);
        break;
    case ScopeShortcuts::NoScope:
        m_history->setSearchString(text);
        break;
    }
}

static void collectBookmarkRows(BookmarkNode *node, const QString &term,
                                QList<OmniboxCompletionModel::TabEntry> *rows)
{
    for (BookmarkNode *child : node->children()) {
        if (rows->count() >= 100)
            return;
        switch (child->type()) {
        case BookmarkNode::Folder:
            collectBookmarkRows(child, term, rows);
            break;
        case BookmarkNode::Bookmark:
            if (!term.isEmpty()
                && !child->title.contains(term, Qt::CaseInsensitive)
                && !child->url.contains(term, Qt::CaseInsensitive))
                break;
            {
                OmniboxCompletionModel::TabEntry entry;
                entry.title = child->title;
                entry.url = child->url;
                entry.icon = HistoryManager::instance()->icon(QUrl(child->url));
                rows->append(entry);
            }
            break;
        default:
            break;
        }
    }
}

void OmniboxCompletionModel::rebuildScopedRows(const QString &term)
{
    QList<TabEntry> rows;
    if (m_scope == ScopeShortcuts::BookmarksScope) {
        if (BookmarksManager *manager = BookmarksManager::instance())
            collectBookmarkRows(manager->bookmarks(), term, &rows);
    } else if (m_tabEntryProvider) {
        const QList<TabEntry> entries = m_tabEntryProvider();
        for (const TabEntry &entry : entries) {
            if (rows.count() >= 100)
                break;
            if (!term.isEmpty()
                && !entry.title.contains(term, Qt::CaseInsensitive)
                && !entry.url.contains(term, Qt::CaseInsensitive))
                continue;
            rows.append(entry);
        }
    }

    beginResetModel();
    m_scopedRows = rows;
    endResetModel();
}

QModelIndex OmniboxCompletionModel::historyIndex(int row, int column) const
{
    return m_history->index(row - suggestionCount(), column);
}

QModelIndex OmniboxCompletionModel::index(int row, int column,
                                        const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();
    return createIndex(row, column);
}

QModelIndex OmniboxCompletionModel::parent(const QModelIndex &) const
{
    return QModelIndex();
}

int OmniboxCompletionModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    if (!historyVisible())
        return m_scopedRows.count();
    return suggestionCount() + m_history->rowCount();
}

int OmniboxCompletionModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_history->columnCount();
}

QVariant OmniboxCompletionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid())
        return QVariant();

    if (!historyVisible()) {
        if (index.row() >= m_scopedRows.count())
            return QVariant();
        const TabEntry &row = m_scopedRows.at(index.row());
        switch (role) {
        case HistoryCompletionModel::HistoryCompletionRole:
            return m_history->isValid()
                ? QLatin1String("a") : QLatin1String("b");
        case TabIndexRole:
            return row.index >= 0 ? QVariant(row.index) : QVariant();
        case HistoryModel::UrlStringRole:
        case Qt::ToolTipRole:
            return row.url;
        case HistoryModel::TitleRole:
            return row.title;
        case Qt::DisplayRole:
            return index.column() == 0 ? QVariant(row.url)
                                       : QVariant(row.title);
        case Qt::DecorationRole:
            return index.column() == 0 ? QVariant(row.icon) : QVariant();
        case Qt::FontRole:
            if (index.column() == 1) {
                QFont font;
                font.setWeight(QFont::Light);
                return font;
            }
            return QVariant();
        default:
            return QVariant();
        }
    }

    if (index.row() < suggestionCount()) {
        const QString &suggestion = m_suggestions.at(index.row());
        switch (role) {
        case HistoryCompletionModel::HistoryCompletionRole:
            // Stay in step with the history block's matching hack:
            // the completer only ever sees these rows while it is
            // live anyway.
            return m_history->isValid()
                ? QLatin1String("a") : QLatin1String("b");
        case HistoryModel::UrlStringRole:
            // HistoryCompleter::pathFromIndex() reads this role —
            // returning the text routes activation back through
            // guessUrlFromString.
            return suggestion;
        case Qt::DisplayRole:
            if (index.column() == 0)
                return suggestion;
            return m_engineName.isEmpty()
                ? QVariant() : tr("Search %1").arg(m_engineName);
        case Qt::ToolTipRole:
            return suggestion;
        case Qt::FontRole:
            if (index.column() == 1) {
                QFont font;
                font.setWeight(QFont::Light);
                return font;
            }
            return QVariant();
        default:
            return QVariant();
        }
    }

    return m_history->data(historyIndex(index.row(), index.column()), role);
}

Qt::ItemFlags OmniboxCompletionModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (!historyVisible())
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.row() < suggestionCount())
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return m_history->flags(historyIndex(index.row(), index.column()));
}

HistoryCompletionModel *HistoryCompleter::historyCompletionModel() const
{
    // SRCH01: the location-bar completer is backed by an
    // OmniboxCompletionModel wrapping the history model.
    if (OmniboxCompletionModel *omnibox =
            qobject_cast<OmniboxCompletionModel*>(model()))
        return omnibox->historyCompletionModel();
    return qobject_cast<HistoryCompletionModel*>(model());
}

HistoryCompleter::HistoryCompleter(QObject *parent)
    : QCompleter(parent)
{
    init();
}

HistoryCompleter::HistoryCompleter(QAbstractItemModel *m, QObject *parent)
    : QCompleter(m, parent)
{
    init();
}

void HistoryCompleter::init()
{
    setPopup(new HistoryCompletionView());

    // we want to complete against our own faked role
    setCompletionRole(HistoryCompletionModel::HistoryCompletionRole);

    // and since we fake our completion role, we can take
    // advantage of the sorted-model optimizations in QCompleter
    setCaseSensitivity(Qt::CaseSensitive);
    setModelSorting(QCompleter::CaseSensitivelySortedModel);

    m_filterTimer.setSingleShot(true);
    connect(&m_filterTimer, &QTimer::timeout, this, &HistoryCompleter::updateFilter);
}

QString HistoryCompleter::pathFromIndex(const QModelIndex &index) const
{
    // we want to return the actual url from the history for the
    // data the QCompleter finally returns
    return model()->data(index, HistoryModel::UrlStringRole).toString();
}

QStringList HistoryCompleter::splitPath(const QString &path) const
{
    if (path == m_searchString)
        return QStringList() << QLatin1String("a");

    // queue an update to our search string
    // We will wait a bit so that if the user is quickly typing,
    // we don't try to complete until they pause.
    if (m_filterTimer.isActive())
        m_filterTimer.stop();
    m_filterTimer.start(150);

    // if the previous search results are not a superset of
    // the current search results, tell the model that it is not valid yet
    if (!path.startsWith(m_searchString)) {
        HistoryCompletionModel *completionModel = historyCompletionModel();
        Q_ASSERT(completionModel);
        completionModel->setValid(false);
    }

    m_searchString = path;

    // the actual filtering is done by the HistoryCompletionModel; we just
    // return a short dummy here so that QCompleter thinks we match everything
    return QStringList() << QLatin1String("a");
}

bool HistoryCompleter::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && popup()->isVisible()) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Tab) {
            QKeyEvent *newEvent = new QKeyEvent(QEvent::KeyPress,
                                                Qt::Key_Down,
                                                keyEvent->modifiers(),
                                                QString());

            if (!QCompleter::eventFilter(obj, newEvent))
                obj->event(newEvent);
            return true;
        } else if (keyEvent->key() == Qt::Key_Backtab) {
            QKeyEvent *newEvent = new QKeyEvent(QEvent::KeyPress,
                                                Qt::Key_Up,
                                                keyEvent->modifiers(),
                                                keyEvent->text(),
                                                keyEvent->isAutoRepeat(),
                                                keyEvent->count());

            if (!QCompleter::eventFilter(obj, newEvent))
                obj->event(newEvent);
            return true;
        } else if (keyEvent->key() == Qt::Key_Escape) {
            popup()->hide();
        }
    }
    return QCompleter::eventFilter(obj, event);
}

void HistoryCompleter::updateFilter()
{
    HistoryCompletionModel *completionModel = historyCompletionModel();
    Q_ASSERT(completionModel);

    // SRCH06: the omnibox layer parses shortcut-nickname prefixes
    // ("@history foo") and may swap the completion provider — in a
    // bookmarks/tabs scope the history block is not displayed at all,
    // so filtering and sorting it is skipped.
    bool sortHistory = true;
    if (OmniboxCompletionModel *omnibox =
            qobject_cast<OmniboxCompletionModel*>(model())) {
        omnibox->setSearchText(m_searchString);
        sortHistory = omnibox->historyVisible();
    } else {
        completionModel->setSearchString(m_searchString);
    }

    // sort the model
    if (sortHistory)
        completionModel->sort(0);

    // mark it valid
    completionModel->setValid(true);

    // and now update the QCompleter widget, but only if the user is still
    // typing a url
    if (widget() && widget()->hasFocus())
        complete();
}
