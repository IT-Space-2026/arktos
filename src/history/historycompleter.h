/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef HISTORYCOMPLETER_H
#define HISTORYCOMPLETER_H

#include "history.h"
#include "scopeshortcuts.h"

#include <qcompleter.h>
#include <qicon.h>
#include <qregularexpression.h>
#include <qsortfilterproxymodel.h>
#include <qtableview.h>
#include <qtimer.h>

#include <functional>

class QResizeEvent;
class HistoryCompletionView : public QTableView
{
public:
    HistoryCompletionView(QWidget *parent = nullptr);
    int sizeHintForRow(int row) const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
};

// These two classes constitute a dirty hack around QCompleter's inflexibility:
// QCompleter does not allow changing the matching algorithm; it is fixed
// at a simple QString::startsWith() comparison against the model's completionRole()
// data. (QCompleter also does not allow post-facto sorting of the completion results,
// the source model must already be completed.) To work around these limitations,
// we create a custom subclass of QCompleter which abuses the QCompleter::pathFromIndex()
// virtual override to tell the HistoryCompletionModel which string to look for,
// _before_ the QCompleter tries to do matching. Since the HistoryCompletionModel does
// its own filtering, we just lie to the QCompleter and tell it everything matches. We then
// abuse QCompleter::pathFromIndex() to return a url that does not start with what
// the user typed -- but is what they were looking for.

class HistoryCompletionModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchString READ searchString WRITE setSearchString)

public:
    HistoryCompletionModel(QObject *parent = nullptr);

    enum Roles { HistoryCompletionRole = HistoryFilterModel::MaxRole + 1 };

    QString searchString() const;
    void setSearchString(QString str);

    bool isValid() const;
    void setValid(bool b);

    virtual QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

protected:
    virtual bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
    virtual bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QString m_searchString;
    QRegularExpression m_wordMatcher;
    bool m_isValid;
};

#ifdef ARORA_RUSTCORE
// OMNI01: the rustcore history store serves the completer's rows —
// rc_history_suggest() returns them already filtered on the term and
// ranked by the summed frecency + word-boundary score, so this is a
// flat model behind the roles HistoryCompletionModel's filter/sort
// pipeline consumes (it re-applies the same predicate/comparator, so
// behavior is identical and the no-rust path is untouched).  The store
// is the authority: a visit landing mid-completion refreshes through
// the RustCoreBridge "history" change topic.
class RustHistorySuggestModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit RustHistorySuggestModel(QObject *parent = nullptr);

    void setTerm(const QString &term);
    QString term() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;

private:
    void refresh();

    struct Row {
        QString url;
        QString title;
        qint64 ts;
        qint64 frecency;
    };
    QString m_term;
    QList<Row> m_rows;
};
#endif // ARORA_RUSTCORE

// SRCH01: prepends live search-engine suggestions to the history
// completion so the location bar acts like a modern omnibox.  The
// model is flat: suggestion rows occupy the top, history rows are
// forwarded below them.  Suggestion rows report the suggestion text
// for the url roles, so activating one routes it back through
// TabWidget::guessUrlFromString — a url-shaped suggestion navigates,
// anything else searches the current engine.
//
// SRCH06: the model also serves the "shortcut nickname" scopes — a
// leading "@bookmarks"/"@history"/"@tabs " token swaps the completion
// provider for that input.  In a bookmarks/tabs scope the history
// block is replaced by rows from the matching provider (TabEntry
// rows; bookmarks carry index -1, tab rows carry the tab index in
// TabIndexRole so activation can switch to it instead of navigating).
class OmniboxCompletionModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    OmniboxCompletionModel(HistoryCompletionModel *historyCompletionModel,
                           QObject *parent = nullptr);

    enum ExtraRoles {
        TabIndexRole = HistoryCompletionModel::HistoryCompletionRole + 1
    };

    // A scoped-completion row: index is the target tab for @tabs
    // rows, -1 for bookmarks.
    struct TabEntry {
        int index = -1;
        QString title;
        QString url;
        QIcon icon;
    };

    HistoryCompletionModel *historyCompletionModel() const;

    void setEngineName(const QString &name);
    QString engineName() const;

    void setSuggestions(const QStringList &suggestions);
    QStringList suggestions() const;

    // Scope-aware entry point for the completer: parses a leading
    // shortcut token off the raw input, swaps providers and feeds the
    // stripped term (or the full text) to the history block.
    void setSearchText(const QString &text);
    ScopeShortcuts::Scope scope() const;
    // True while the history block is exposed (unscoped input or the
    // @history scope); false in the bookmarks/tabs scopes.
    bool historyVisible() const;

    // @tabs rows come from the owning TabWidget — supplied as a
    // callback so this model never depends on tabwidget.h.
    void setTabEntryProvider(std::function<QList<TabEntry>()> provider);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    int suggestionCount() const;
    QModelIndex historyIndex(int row, int column) const;
    void rebuildScopedRows(const QString &term);

    HistoryCompletionModel *m_history;
    QStringList m_suggestions;
    QString m_engineName;
    ScopeShortcuts::Scope m_scope = ScopeShortcuts::NoScope;
    QList<TabEntry> m_scopedRows;
    std::function<QList<TabEntry>()> m_tabEntryProvider;
};

class HistoryCompleter : public QCompleter
{
    Q_OBJECT

public:
    HistoryCompleter(QObject *parent = nullptr);
    HistoryCompleter(QAbstractItemModel *model, QObject *parent = nullptr);

    virtual QString pathFromIndex(const QModelIndex &index) const override;
    virtual QStringList splitPath(const QString &path) const override;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void updateFilter();

private:
    void init();
    HistoryCompletionModel *historyCompletionModel() const;
    mutable QString m_searchString;
    mutable QTimer m_filterTimer;
};

#endif
