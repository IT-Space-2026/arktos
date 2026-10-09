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
#include "settings.h"
#include "utils/edittreeview.h"
#include "utils/treesortfilterproxymodel.h"

#include <qapplication.h>
#include <qboxlayout.h>
#include <qcombobox.h>
#include <qcursor.h>
#include <qdesktopservices.h>
#include <qfileinfo.h>
#include <qheaderview.h>
#include <qlabel.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qshortcut.h>
#include <qsortfilterproxymodel.h>
#include <qstyleditemdelegate.h>
#include <qstyle.h>
#include <qtabwidget.h>
#include <qtoolbutton.h>

// DownloadModel's DisplayRole is deliberately empty (the rows used to
// be painted by live DownloadItem index widgets).  This proxy maps the
// plain roles onto the standard view roles for the sidebar's compact
// list, and adds search filtering + sortable Date/Name/Size columns.
class SidebarDownloadsModel : public QSortFilterProxyModel
{
public:
    SidebarDownloadsModel(QAbstractItemModel *source, QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setSourceModel(source);
        setDynamicSortFilter(true);
    }

    void setSearchText(const QString &text)
    {
        if (m_searchText == text)
            return;
        m_searchText = text;
        invalidateFilter();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::DisplayRole)
            return QSortFilterProxyModel::data(index, DownloadModel::FileNameRole);
        if (role == Qt::ToolTipRole) {
            const QString tip = QSortFilterProxyModel::data(index, DownloadModel::InfoRole).toString();
            const QString name = QSortFilterProxyModel::data(index, DownloadModel::FileNameRole).toString();
            return tip.isEmpty() ? name : name + QLatin1Char('\n') + tip;
        }
        return QSortFilterProxyModel::data(index, role);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (m_searchText.isEmpty())
            return true;
        const QModelIndex index = sourceModel()->index(row, 0, parent);
        const auto matches = [this](const QVariant &text) {
            return text.toString().contains(m_searchText,
                                            Qt::CaseInsensitive);
        };
        return matches(index.data(DownloadModel::FileNameRole))
            || matches(index.data(DownloadModel::SourceUrlRole));
    }

private:
    QString m_searchText;
};

// DOWN02: one readable line per download — file-type icon, an
// elided-middle file name and the size right-aligned.  Palette roles
// only, so dark schemes and HighContrast read correctly (UIP01).
class SidebarDownloadDelegate : public QStyledItemDelegate
{
public:
    explicit SidebarDownloadDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QStyle *style = opt.widget ? opt.widget->style()
                                   : QApplication::style();
        // Background, hover and selection come from the style.
        opt.text.clear();
        opt.icon = QIcon();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter,
                           opt.widget);

        const int iconExtent = style->pixelMetric(QStyle::PM_SmallIconSize,
                                                  &opt, opt.widget);
        QRect rect = opt.rect.adjusted(4, 0, -4, 0);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        icon.paint(painter,
                   QRect(rect.left(),
                         rect.center().y() - iconExtent / 2,
                         iconExtent, iconExtent));
        rect.setLeft(rect.left() + iconExtent + 6);

        const bool completed =
            index.data(DownloadModel::CompletedRole).toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor textColor = selected
            ? opt.palette.color(QPalette::HighlightedText)
            : completed
                ? opt.palette.color(QPalette::Text)
                : opt.palette.color(QPalette::Disabled, QPalette::Text);

        const qint64 size = index.data(DownloadModel::SizeRole).toLongLong();
        const QString sizeText = size > 0
            ? DownloadManager::dataString(size) : QString();
        const QFontMetrics metrics(opt.font);
        QRect nameRect = rect;
        if (!sizeText.isEmpty()) {
            const int sizeWidth = metrics.horizontalAdvance(sizeText);
            nameRect.setRight(rect.right() - sizeWidth - 8);
            painter->setPen(selected
                ? opt.palette.color(QPalette::HighlightedText)
                : opt.palette.color(QPalette::PlaceholderText));
            painter->drawText(rect, Qt::AlignVCenter | Qt::AlignRight,
                              sizeText);
        }

        painter->setPen(textColor);
        painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                          metrics.elidedText(
                              index.data(DownloadModel::FileNameRole)
                                  .toString(),
                              Qt::ElideMiddle, nameRect.width()));
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QStyle *style = opt.widget ? opt.widget->style()
                                   : QApplication::style();
        const int iconExtent = style->pixelMetric(QStyle::PM_SmallIconSize,
                                                  &opt, opt.widget);
        return QSize(120, qMax(iconExtent,
                               QFontMetrics(opt.font).height()) + 8);
    }
};

SidebarPanel::SidebarPanel(QWidget *parent)
    : QWidget(parent)
    , m_tabs(new QTabWidget(this))
    , m_bookmarksView(new EditTreeView)
    , m_historyView(new EditTreeView)
    , m_downloadsView(new QListView)
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

    // DOWN02: downloads — the standalone manager window is gone; this
    // panel is the download surface.  Header close X, a filter field,
    // a sort combo, the item list and a bottom detail pane that hosts
    // the selected row's real DownloadItem card (DOWN01).
    m_downloadsPage = new QWidget(m_tabs);
    QVBoxLayout *downloadsLayout = new QVBoxLayout(m_downloadsPage);
    downloadsLayout->setContentsMargins(2, 2, 2, 2);
    downloadsLayout->setSpacing(2);

    QHBoxLayout *downloadsHeader = new QHBoxLayout;
    QLabel *downloadsTitle = new QLabel(tr("Downloads"), m_downloadsPage);
    QFont titleFont = downloadsTitle->font();
    titleFont.setBold(true);
    downloadsTitle->setFont(titleFont);
    downloadsHeader->addWidget(downloadsTitle, 1);
    QToolButton *closeButton = new QToolButton(m_downloadsPage);
    closeButton->setObjectName(QLatin1String("sidebarCloseButton"));
    closeButton->setAccessibleName(tr("Close sidebar"));
    closeButton->setToolTip(tr("Close sidebar"));
    closeButton->setAutoRaise(true);
    closeButton->setIcon(
        m_downloadsPage->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    connect(closeButton, &QToolButton::clicked,
            this, &SidebarPanel::closeRequested);
    downloadsHeader->addWidget(closeButton);
    downloadsLayout->addLayout(downloadsHeader);

    SearchLineEdit *downloadsSearch = new SearchLineEdit(m_downloadsPage);
    downloadsSearch->setObjectName(QLatin1String("sidebarDownloadsSearch"));
    downloadsSearch->setAccessibleName(tr("Filter downloads"));
    connect(downloadsSearch, &SearchLineEdit::textChanged,
            this, [this](const QString &text) {
        static_cast<SidebarDownloadsModel*>(m_downloadsProxy)
            ->setSearchText(text);
    });
    downloadsLayout->addWidget(downloadsSearch);

    m_downloadsSort = new QComboBox(m_downloadsPage);
    m_downloadsSort->setObjectName(QLatin1String("sidebarDownloadsSort"));
    m_downloadsSort->setAccessibleName(tr("Sort downloads"));
    m_downloadsSort->addItem(tr("Sort by Date"), DownloadModel::StartedTimeRole);
    m_downloadsSort->addItem(tr("Sort by Name"), DownloadModel::FileNameRole);
    m_downloadsSort->addItem(tr("Sort by Size"), DownloadModel::SizeRole);
    connect(m_downloadsSort, &QComboBox::activated,
            this, [this](int) { applyDownloadSort(); });
    downloadsLayout->addWidget(m_downloadsSort);

    m_downloadsView->setObjectName(QLatin1String("sidebarDownloadsView"));
    m_downloadsView->setAccessibleName(tr("Downloads"));
    m_downloadsView->setUniformItemSizes(true);
    m_downloadsView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_downloadsView->setItemDelegate(
        new SidebarDownloadDelegate(m_downloadsView));
    m_downloadsView->setModel(m_downloadsProxy);
    // Default sort — newest first.
    m_downloadsProxy->setSortRole(DownloadModel::StartedTimeRole);
    m_downloadsProxy->sort(0, Qt::DescendingOrder);
    connect(m_downloadsView, &QListView::activated,
            this, &SidebarPanel::openDownload);
    m_downloadsView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_downloadsView, &QListView::customContextMenuRequested,
            this, &SidebarPanel::downloadsContextMenu);
    connect(m_downloadsView->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, &SidebarPanel::downloadSelectionChanged);
    // Del removes finished/cancelled rows — same semantics the old
    // EditTableView gave this list.
    QShortcut *removeShortcut =
        new QShortcut(QKeySequence(Qt::Key_Delete), m_downloadsView);
    connect(removeShortcut, &QShortcut::activated, this, [this]() {
        const QModelIndex index = m_downloadsProxy->mapToSource(
            m_downloadsView->currentIndex());
        if (index.isValid())
            m_downloadsProxy->sourceModel()->removeRow(index.row());
    });
    downloadsLayout->addWidget(m_downloadsView);

    // Bottom detail pane — the selected item's DownloadItem card is
    // reparented in here, so the pane literally IS the card (live
    // progress, Restart and Show in File Manager included).
    m_downloadDetail = new QFrame(m_downloadsPage);
    m_downloadDetail->setObjectName(QLatin1String("sidebarDownloadDetail"));
    m_downloadDetail->setAccessibleName(tr("Download details"));
    m_downloadDetail->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout *detailLayout = new QVBoxLayout(m_downloadDetail);
    detailLayout->setContentsMargins(4, 4, 4, 4);
    m_downloadDetail->setVisible(false);
    downloadsLayout->addWidget(m_downloadDetail);
    // Removing/cleaning rows deletes the hosted card — fold the pane
    // before the pending deleteLater runs.
    connect(DownloadManager::instance()->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &, int first, int last) {
        if (!m_detailItem)
            return;
        for (int row = first; row <= last; ++row) {
            if (DownloadManager::instance()->itemAt(row) == m_detailItem) {
                m_detailItem.clear();
                m_downloadDetail->setVisible(false);
                break;
            }
        }
    });

    QPushButton *settingsButton = new QPushButton(tr("Download Settings..."),
                                                m_downloadsPage);
    settingsButton->setObjectName(QLatin1String("sidebarDownloadSettings"));
    connect(settingsButton, &QPushButton::clicked, this, [this]() {
        SettingsDialog::openPage(this, SettingsDialog::DownloadsPage);
    });
    downloadsLayout->addWidget(settingsButton);
    m_tabs->addTab(m_downloadsPage, tr("Downloads"));

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

QWidget *SidebarPanel::downloadsPage() const
{
    return m_downloadsPage;
}

void SidebarPanel::showDownloads()
{
    m_tabs->setCurrentWidget(m_downloadsPage);
}

void SidebarPanel::applyDownloadSort()
{
    const int role = m_downloadsSort->currentData().toInt();
    m_downloadsProxy->setSortRole(role);
    // Newest/largest first; names alphabetical.
    m_downloadsProxy->sort(0, role == DownloadModel::FileNameRole
                               ? Qt::AscendingOrder
                               : Qt::DescendingOrder);
}

void SidebarPanel::downloadSelectionChanged(const QModelIndex &current,
                                            const QModelIndex &)
{
    // The pane hosts the item's real card — park the previous card
    // back on the (hidden) manager so it keeps its owner and stays
    // out of sight.
    if (m_detailItem) {
        m_detailItem->setParent(DownloadManager::instance());
        m_detailItem->hide();
    }
    DownloadItem *item = nullptr;
    const QModelIndex source = m_downloadsProxy->mapToSource(current);
    if (source.isValid())
        item = DownloadManager::instance()->itemAt(source.row());
    m_detailItem = item;
    if (item) {
        item->setParent(m_downloadDetail);
        m_downloadDetail->layout()->addWidget(item);
        item->setExpanded(true);
        // The card's cached size hint predates the expansion — refresh
        // it so the pane's layout allocates the card's full height.
        item->adjustSize();
        item->show();
    }
    m_downloadDetail->setVisible(item != nullptr);
}

void SidebarPanel::downloadsContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_downloadsView->indexAt(pos);
    const QModelIndex source = m_downloadsProxy->mapToSource(index);
    if (!source.isValid())
        return;
    const QString path = source.data(DownloadModel::OutputFileRole).toString();
    const bool openable = QFileInfo::exists(path)
        && source.data(DownloadModel::CompletedRole).toBool();
    DownloadItem *item = DownloadManager::instance()->itemAt(source.row());
    QMenu menu;
    QAction *openAction = menu.addAction(tr("Open"), this,
                                         [this, index]() {
        openDownload(index);
    });
    openAction->setEnabled(openable);
    QAction *revealAction = menu.addAction(tr("Open Location"), this,
                                           [item]() {
        if (item)
            QMetaObject::invokeMethod(item, "showInFolder");
    });
    revealAction->setEnabled(!path.isEmpty());
    menu.exec(QCursor::pos());
}

void SidebarPanel::openDownload(const QModelIndex &index)
{
    const QModelIndex source = m_downloadsProxy->mapToSource(index);
    const QString path = source.data(DownloadModel::OutputFileRole).toString();
    if (source.data(DownloadModel::CompletedRole).toBool()
        && QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
    // An unfinished row's actions (Stop/Try Again/Restart) live on its
    // detail card — selecting the row already surfaced the pane.
}
