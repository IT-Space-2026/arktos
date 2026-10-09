/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */
#include "sidebarpanel.h"

#include "autosaver.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "bookmarknode.h"
#include "downloadmanager.h"
#include "history.h"
#include "historymanager.h"
#include "searchlineedit.h"
#include "utils/edittableview.h"
#include "utils/edittreeview.h"
#include "utils/treesortfilterproxymodel.h"

#include <qapplication.h>
#include <qboxlayout.h>
#include <qcursor.h>
#include <qdesktopservices.h>
#include <qfileinfo.h>
#include <qheaderview.h>
#include <qidentityproxymodel.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qtabwidget.h>

// The download dialog paints each row with a live DownloadItem index
// widget, so DownloadModel's DisplayRole is deliberately empty.  This
// read-only proxy maps the plain roles onto the standard view roles
// for the sidebar's compact list.
class SidebarDownloadsModel : public QIdentityProxyModel
{
public:
    SidebarDownloadsModel(QAbstractItemModel *source, QObject *parent = nullptr)
        : QIdentityProxyModel(parent)
    {
        setSourceModel(source);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::DisplayRole)
            return QIdentityProxyModel::data(index, DownloadModel::FileNameRole);
        if (role == Qt::ToolTipRole) {
            const QString tip = QIdentityProxyModel::data(index, Qt::ToolTipRole).toString();
            return tip.isEmpty()
                ? QIdentityProxyModel::data(index, DownloadModel::FileNameRole)
                : tip;
        }
        if (role == Qt::ForegroundRole
            && !QIdentityProxyModel::data(index, DownloadModel::CompletedRole).toBool())
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        return QIdentityProxyModel::data(index, role);
    }
};

SidebarPanel::SidebarPanel(QWidget *parent)
    : QWidget(parent)
    , m_tabs(new QTabWidget(this))
    , m_bookmarksView(new EditTreeView)
    , m_historyView(new EditTreeView)
    , m_downloadsView(new EditTableView)
    , m_bookmarksProxy(new TreeSortFilterProxyModel(m_bookmarksView))
    , m_historyProxy(new TreeSortFilterProxyModel(m_historyView))
    , m_downloadsProxy(new SidebarDownloadsModel(
          DownloadManager::instance()->model(), m_downloadsView))
    , m_notes(new QPlainTextEdit)
    , m_autoSaver(new AutoSaver(this))
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_tabs);

    m_tabs->setObjectName(QLatin1String("sidebarTabs"));
    m_tabs->setDocumentMode(true);

    // Bookmarks — the shared model behind the dialog/toolbar, with the
    // same search-line + proxy presentation the dialog uses.
    QWidget *bookmarksPage = new QWidget(m_tabs);
    QVBoxLayout *bookmarksLayout = new QVBoxLayout(bookmarksPage);
    bookmarksLayout->setContentsMargins(2, 2, 2, 2);
    bookmarksLayout->setSpacing(2);
    SearchLineEdit *bookmarksSearch = new SearchLineEdit(bookmarksPage);
    bookmarksLayout->addWidget(bookmarksSearch);
    m_bookmarksView->setObjectName(QLatin1String("sidebarBookmarksView"));
    m_bookmarksView->setAccessibleName(tr("Bookmarks"));
    m_bookmarksView->setUniformRowHeights(true);
    m_bookmarksView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bookmarksView->setTextElideMode(Qt::ElideMiddle);
    m_bookmarksView->setHeaderHidden(true);
    m_bookmarksProxy->setFilterKeyColumn(-1);
    connect(bookmarksSearch, &SearchLineEdit::textChanged,
            m_bookmarksProxy, &QSortFilterProxyModel::setFilterFixedString);
    m_bookmarksProxy->setSourceModel(
        BookmarksManager::instance()->bookmarksModel());
    m_bookmarksView->setModel(m_bookmarksProxy);
    m_bookmarksView->setExpanded(m_bookmarksProxy->index(0, 0), true);
    connect(m_bookmarksView, &EditTreeView::activated,
            this, [this](const QModelIndex &index) {
        openBookmark(index, TabWidget::CurrentTab);
    });
    m_bookmarksView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_bookmarksView, &EditTreeView::customContextMenuRequested,
            this, &SidebarPanel::bookmarksContextMenu);
    bookmarksLayout->addWidget(m_bookmarksView);
    m_tabs->addTab(bookmarksPage, tr("Bookmarks"));

    // History — the day-grouped tree model the History dialog uses.
    QWidget *historyPage = new QWidget(m_tabs);
    QVBoxLayout *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(2, 2, 2, 2);
    historyLayout->setSpacing(2);
    SearchLineEdit *historySearch = new SearchLineEdit(historyPage);
    historyLayout->addWidget(historySearch);
    m_historyView->setObjectName(QLatin1String("sidebarHistoryView"));
    m_historyView->setAccessibleName(tr("History"));
    m_historyView->setUniformRowHeights(true);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setTextElideMode(Qt::ElideMiddle);
    m_historyView->setHeaderHidden(true);
    m_historyProxy->setSortRole(HistoryModel::DateTimeRole);
    m_historyProxy->setFilterKeyColumn(-1);
    connect(historySearch, &SearchLineEdit::textChanged,
            m_historyProxy, &QSortFilterProxyModel::setFilterFixedString);
    m_historyProxy->setSourceModel(
        HistoryManager::instance()->historyTreeModel());
    m_historyView->setModel(m_historyProxy);
    m_historyView->setExpanded(m_historyProxy->index(0, 0), true);
    connect(m_historyView, &EditTreeView::activated,
            this, [this](const QModelIndex &index) {
        openHistoryEntry(index, TabWidget::CurrentTab);
    });
    m_historyView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_historyView, &EditTreeView::customContextMenuRequested,
            this, &SidebarPanel::historyContextMenu);
    historyLayout->addWidget(m_historyView);
    m_tabs->addTab(historyPage, tr("History"));

    // Downloads — a compact read-only list over the manager's model;
    // activating a finished row opens the file, anything else brings
    // up the full manager dialog.
    QWidget *downloadsPage = new QWidget(m_tabs);
    QVBoxLayout *downloadsLayout = new QVBoxLayout(downloadsPage);
    downloadsLayout->setContentsMargins(2, 2, 2, 2);
    downloadsLayout->setSpacing(2);
    m_downloadsView->setObjectName(QLatin1String("sidebarDownloadsView"));
    m_downloadsView->setAccessibleName(tr("Downloads"));
    m_downloadsView->setTextElideMode(Qt::ElideMiddle);
    m_downloadsView->horizontalHeader()->hide();
    m_downloadsView->verticalHeader()->hide();
    m_downloadsView->setModel(m_downloadsProxy);
    connect(m_downloadsView, &EditTableView::activated,
            this, &SidebarPanel::openDownload);
    downloadsLayout->addWidget(m_downloadsView);
    QPushButton *manageButton = new QPushButton(tr("Download Manager..."),
                                                downloadsPage);
    connect(manageButton, &QPushButton::clicked,
            this, []() { DownloadManager::instance()->show(); });
    downloadsLayout->addWidget(manageButton);
    m_tabs->addTab(downloadsPage, tr("Downloads"));

    // Notes — a per-profile scratch pad, debounce-persisted through
    // the AutoSaver like every other app-side store.
    QWidget *notesPage = new QWidget(m_tabs);
    QVBoxLayout *notesLayout = new QVBoxLayout(notesPage);
    notesLayout->setContentsMargins(2, 2, 2, 2);
    notesLayout->setSpacing(2);
    m_notes->setObjectName(QLatin1String("sidebarNotes"));
    m_notes->setAccessibleName(tr("Notes"));
    m_notes->setPlaceholderText(tr("Notes are saved automatically."));
    notesLayout->addWidget(m_notes);
    m_tabs->addTab(notesPage, tr("Notes"));

    connect(m_notes, &QPlainTextEdit::textChanged,
            m_autoSaver, &AutoSaver::changeOccurred);
    connect(m_tabs, &QTabWidget::currentChanged,
            m_autoSaver, &AutoSaver::changeOccurred);

    QSettings settings;
    m_notes->setPlainText(
        settings.value(QLatin1String("sidebar/notes")).toString());
    const int tab = settings.value(QLatin1String("sidebar/currentTab"), 0)
                        .toInt();
    if (tab >= 0 && tab < m_tabs->count())
        m_tabs->setCurrentIndex(tab);
}

QIcon SidebarPanel::icon(const QWidget *forPalette)
{
    const qreal dpr = forPalette ? forPalette->devicePixelRatioF() : 1.0;
    QPixmap pixmap(16 * dpr, 16 * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = forPalette
        ? forPalette->palette().color(QPalette::ButtonText)
        : QColor(Qt::black);
    painter.setPen(QPen(color, 1.4));
    painter.drawRoundedRect(QRectF(1.2, 2.2, 13.6, 11.6), 2, 2);
    painter.drawLine(QPointF(5.8, 2.9), QPointF(5.8, 13.1));
    return QIcon(pixmap);
}

QTabWidget *SidebarPanel::tabs() const
{
    return m_tabs;
}

QPlainTextEdit *SidebarPanel::notes() const
{
    return m_notes;
}

void SidebarPanel::save()
{
    QSettings settings;
    settings.setValue(QLatin1String("sidebar/notes"), m_notes->toPlainText());
    settings.setValue(QLatin1String("sidebar/currentTab"), m_tabs->currentIndex());
}

void SidebarPanel::openBookmark(const QModelIndex &index, TabWidget::OpenUrlIn tab)
{
    if (!index.isValid())
        return;
    QModelIndex sourceIndex = m_bookmarksProxy->mapToSource(index);
    const BookmarkNode *node =
        BookmarksManager::instance()->bookmarksModel()->node(sourceIndex);
    if (!index.parent().isValid() || !node
        || node->type() != BookmarkNode::Bookmark)
        return;
    emit openUrl(index.data(BookmarksModel::UrlRole).toUrl(), tab,
                 index.data(Qt::DisplayRole).toString());
}

void SidebarPanel::bookmarksContextMenu(const QPoint &pos)
{
    QModelIndex index = m_bookmarksView->indexAt(pos);
    index = index.siblingAtColumn(0);
    QModelIndex sourceIndex = m_bookmarksProxy->mapToSource(index);
    const BookmarkNode *node =
        BookmarksManager::instance()->bookmarksModel()->node(sourceIndex);
    if (!index.isValid() || !node || node->type() != BookmarkNode::Bookmark)
        return;
    QMenu menu;
    menu.addAction(tr("Open"), this, [this, index]() {
        openBookmark(index, TabWidget::CurrentTab);
    });
    menu.addAction(tr("Open in New Tab"), this, [this, index]() {
        openBookmark(index, TabWidget::NewSelectedTab);
    });
    menu.exec(QCursor::pos());
}

void SidebarPanel::openHistoryEntry(const QModelIndex &index,
                                    TabWidget::OpenUrlIn tab)
{
    if (!index.isValid() || !index.parent().isValid())
        return;
    emit openUrl(index.data(HistoryModel::UrlRole).toUrl(), tab,
                 index.data(HistoryModel::TitleRole).toString());
}

void SidebarPanel::historyContextMenu(const QPoint &pos)
{
    QModelIndex index = m_historyView->indexAt(pos);
    if (!index.isValid() || !index.parent().isValid())
        return;
    QMenu menu;
    menu.addAction(tr("Open"), this, [this, index]() {
        openHistoryEntry(index, TabWidget::CurrentTab);
    });
    menu.addAction(tr("Open in New Tab"), this, [this, index]() {
        openHistoryEntry(index, TabWidget::NewSelectedTab);
    });
    menu.addSeparator();
    menu.addAction(tr("Remove Entry"), this, [this, index]() {
        m_historyProxy->removeRow(index.row(), index.parent());
    });
    menu.exec(QCursor::pos());
}

void SidebarPanel::openDownload(const QModelIndex &index)
{
    const QModelIndex source = m_downloadsProxy->mapToSource(index);
    const QString path = source.data(DownloadModel::OutputFileRole).toString();
    if (source.data(DownloadModel::CompletedRole).toBool()
        && QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else {
        DownloadManager::instance()->show();
    }
}
