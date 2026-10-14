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

#include "arorastyle.h"
#include "autosaver.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "bookmarknode.h"
#include "browserapplication.h"
#include "downloadmanager.h"
#include "history.h"
#include "historymanager.h"
#include "notesmodel.h"
#include "searchlineedit.h"
#include "settings.h"
#include "utils/aroraicon.h"
#include "utils/edittreeview.h"
#include "utils/flowlayout.h"
#include "utils/treesortfilterproxymodel.h"

#include <qapplication.h>
#include <qboxlayout.h>
#include <qcombobox.h>
#include <qcursor.h>
#include <qdesktopservices.h>
#include <qevent.h>
#include <qfileinfo.h>
#include <qheaderview.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qshortcut.h>
#include <QSignalBlocker>
#include <qsortfilterproxymodel.h>
#include <qstackedwidget.h>
#include <qstyleditemdelegate.h>
#include <qstyle.h>
#include <qstyleoption.h>
#include <qtabbar.h>
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
        beginFilterChange();
        endFilterChange();
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
            ? AroraStyle::selectionTextColor(opt.palette)
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
                ? AroraStyle::selectionTextColor(opt.palette)
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

// SIDE09 'Show Folder Counters': real folder rows carry their child
// count as a " (n)" suffix on DisplayRole only — EditRole stays the
// clean title so the rename editor never sees the suffix, the flag
// is consulted at call time so toggling the option only needs a
// repaint (applyOptions pokes the viewport), and the two structural
// root rows (Bookmarks Menu/Toolbar, children of the invisible root)
// stay untagged.
class SidebarBookmarksModel : public TreeSortFilterProxyModel
{
public:
    explicit SidebarBookmarksModel(QObject *parent = nullptr)
        : TreeSortFilterProxyModel(parent)
    {
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const QVariant value =
            TreeSortFilterProxyModel::data(index, role);
        if (role != Qt::DisplayRole || index.column() != 0
            || !SidebarPanel::showsFolderCounters())
            return value;
        QAbstractItemModel *model = sourceModel();
        if (!model)
            return value;
        const BookmarkNode *node = static_cast<BookmarksModel *>(model)
            ->node(mapToSource(index));
        if (node && node->type() == BookmarkNode::Folder
            && node->parent()
            && node->parent()->type() != BookmarkNode::Root
            && !node->children().isEmpty())
            return value.toString()
                + QStringLiteral(" (%1)").arg(node->children().count());
        return value;
    }
};

// SIDE02: the dock's section switcher — a vertical icon rail like
// Vivaldi's.  A stock West-shaped QTabBar would rotate each entry's
// text AND icon; this bar keeps the tab text (tooltips and the a11y
// name still read it) but paints just the unrotated glyph inside a
// square cell.
class SidebarRail : public QTabBar
{
public:
    explicit SidebarRail(QWidget *parent = nullptr)
        : QTabBar(parent)
        , m_toggle(new QToolButton(this))
    {
        setShape(RoundedWest);
        setExpanding(false);
        setAccessibleName(SidebarPanel::tr("Sidebar panels"));
        const int extent =
            style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        setIconSize(QSize(extent, extent));

        // SIDE09 'Show Panel Toggle': the fold button at the rail's
        // foot, like Vivaldi's panel toggle — clicking it closes the
        // sidebar (the rail only exists while the dock is up).
        m_toggle->setObjectName(QLatin1String("sidebarPanelToggle"));
        m_toggle->setAutoRaise(true);
        m_toggle->setIcon(SidebarPanel::icon(this));
        m_toggle->setToolTip(SidebarPanel::tr("Hide sidebar"));
        m_toggle->setAccessibleName(SidebarPanel::tr("Hide sidebar"));
        m_toggle->setFixedSize(extent + 8, extent + 8);
    }

    QToolButton *toggleButton() const
    {
        return m_toggle;
    }

    void setToggleVisible(bool visible)
    {
        m_toggle->setVisible(visible);
    }

    QSize tabSizeHint(int index) const override
    {
        Q_UNUSED(index);
        const int extent = iconSize().height() + 12;
        return QSize(extent, extent);
    }

    QSize minimumTabSizeHint(int index) const override
    {
        return tabSizeHint(index);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QTabBar::resizeEvent(event);
        // The cells are top-packed (setExpanding false), so the rail's
        // dead bottom band hosts the toggle — centered across the
        // strip whichever edge it sits on.
        m_toggle->move(qMax(0, (width() - m_toggle->width()) / 2),
                       qMax(0, height() - m_toggle->height() - 2));
    }

private:
    QToolButton *m_toggle;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < count(); ++i) {
            QStyleOptionTab option;
            initStyleOption(&option, i);
            const QRect cell = option.rect.adjusted(3, 3, -3, -3);
            if (option.state & QStyle::State_Selected) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(palette().color(QPalette::Highlight));
                painter.drawRoundedRect(cell, 4, 4);
            } else if (option.state & QStyle::State_MouseOver) {
                QColor hover = palette().color(QPalette::Highlight);
                hover.setAlpha(64);
                painter.setPen(Qt::NoPen);
                painter.setBrush(hover);
                painter.drawRoundedRect(cell, 4, 4);
            }
            QRect iconRect(QPoint(0, 0), iconSize());
            iconRect.moveCenter(cell.center());
            tabIcon(i).paint(&painter, iconRect, Qt::AlignCenter,
                             (option.state & QStyle::State_Selected)
                                 ? QIcon::Selected : QIcon::Normal);
            if (option.state & QStyle::State_HasFocus) {
                QStyleOptionFocusRect focus;
                focus.initFrom(this);
                focus.rect = cell.adjusted(1, 1, -1, -1);
                style()->drawPrimitive(QStyle::PE_FrameFocusRect,
                                       &focus, &painter, this);
            }
        }
    }
};

// QTabWidget::setTabBar() is protected — a shim is the only way to
// install the rail before the first insertTab().
class SidebarTabWidget : public QTabWidget
{
public:
    explicit SidebarTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
    {
        setTabPosition(West);
        setTabBar(new SidebarRail(this));
    }
};

bool SidebarPanel::s_folderCounters = true;

bool SidebarPanel::showsFolderCounters()
{
    return s_folderCounters;
}

SidebarPanel::SidebarPanel(QWidget *parent)
    : QWidget(parent)
    , m_tabs(new SidebarTabWidget(this))
    , m_autoSaver(new AutoSaver(this))
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_tabs);

    m_tabs->setObjectName(QLatin1String("sidebarTabs"));
    m_tabs->setDocumentMode(true);

    // Every rail entry is an icon-only tab — the title rides the
    // tooltip + accessible name instead of rendering as rotated text.
    // The option reads run before the population so lazy load can
    // park placeholders (s_folderCounters syncs for the model's
    // first fetch too).
    const QSettings bootSettings;
    m_lazyLoad = bootSettings.value(QLatin1String("sidebar/lazyLoad"),
                                    false).toBool();
    s_folderCounters = bootSettings.value(
        QLatin1String("sidebar/folderCounters"), true).toBool();
    {
        // Blocked so inserting the first page can't select (and under
        // lazy load eagerly build) index 0 ahead of the stored
        // currentTab restore below.
        const QSignalBlocker blocker(m_tabs);
        applyPanelVisibility();
    }

    connect(m_tabs, &QTabWidget::currentChanged,
            this, [this](int index) { buildPendingTab(index); });
    connect(m_tabs, &QTabWidget::currentChanged,
            m_autoSaver, &AutoSaver::changeOccurred);
    // The rail's fold button lands on the same signal as the page
    // header's close X — the host folds the dock.
    connect(static_cast<SidebarRail *>(m_tabs->tabBar())->toggleButton(),
            &QToolButton::clicked, this, &SidebarPanel::closeRequested);

    QSettings settings;
    const int tab = settings.value(QLatin1String("sidebar/currentTab"), 0)
                        .toInt();
    if (tab >= 0 && tab < m_tabs->count())
        m_tabs->setCurrentIndex(tab);
    // Lazy load: the restored (or first) page materializes on demand —
    // when the stored index already is current nothing emitted, so
    // kick the swap explicitly.
    buildPendingTab(m_tabs->currentIndex());
    applyOptions();
}

SidebarPanel::~SidebarPanel()
{
    // A hosted download card is only BORROWED from DownloadManager —
    // its row pointer in m_downloads must never dangle, so the card
    // goes back before this widget tree deletes it (window close with
    // a row selected).
    if (m_detailItem) {
        m_detailItem->setParent(DownloadManager::instance());
        m_detailItem->hide();
        m_detailItem.clear();
    }
}

// The registered rail entries, in display order.  Built-ins resolve
// lazily so registerPanel() calls made before the first panel (or
// without one, e.g. the Preferences checklist) still append to the
// same list.
QList<SidebarPanel::Panel> &SidebarPanel::registry()
{
    static QList<Panel> panels;
    if (panels.isEmpty()) {
        panels.append({ QByteArrayLiteral("bookmarks"), tr("Bookmarks"),
                        QByteArrayLiteral("user-bookmarks"),
                        [](SidebarPanel *p) { return p->buildBookmarksPage(); } });
        panels.append({ QByteArrayLiteral("history"), tr("History"),
                        QByteArrayLiteral("document-revert"),
                        [](SidebarPanel *p) { return p->buildHistoryPage(); } });
        panels.append({ QByteArrayLiteral("downloads"), tr("Downloads"),
                        QByteArrayLiteral("emblem-downloads"),
                        [](SidebarPanel *p) { return p->buildDownloadsPage(); } });
        panels.append({ QByteArrayLiteral("notes"), tr("Notes"),
                        QByteArrayLiteral("edit-paste"),
                        [](SidebarPanel *p) { return p->buildNotesPage(); } });
    }
    return panels;
}

QList<SidebarPanel::Panel> SidebarPanel::panels()
{
    return registry();
}

void SidebarPanel::registerPanel(const Panel &panel)
{
    for (Panel &entry : registry()) {
        if (entry.id == panel.id) {
            entry = panel;
            return;
        }
    }
    registry().append(panel);
}

static QString sidebarPanelKey(const QByteArray &id)
{
    return QLatin1String("sidebar/panels/") + QLatin1String(id);
}

bool SidebarPanel::isPanelVisible(const QByteArray &id)
{
    return QSettings().value(sidebarPanelKey(id), true).toBool();
}

void SidebarPanel::setPanelVisible(const QByteArray &id, bool visible)
{
    QSettings().setValue(sidebarPanelKey(id), visible);
}

QStringList SidebarPanel::panelOrder()
{
    return QSettings().value(QLatin1String("sidebar/panelOrder"))
        .toStringList();
}

void SidebarPanel::setPanelOrder(const QStringList &order)
{
    QSettings().setValue(QLatin1String("sidebar/panelOrder"), order);
}

// Registered panels re-sequenced by the persisted order — the rail
// and the Preferences checklist both read this, and registry() order
// remains the fallback for anything the key doesn't mention.
QList<SidebarPanel::Panel> SidebarPanel::orderedPanels()
{
    QList<Panel> rest = registry();
    QList<Panel> ordered;
    for (const QString &id : panelOrder()) {
        const QByteArray key = id.toLatin1();
        for (int i = 0; i < rest.size(); ++i) {
            if (rest.at(i).id == key) {
                ordered.append(rest.takeAt(i));
                break;
            }
        }
    }
    ordered += rest;
    return ordered;
}

int SidebarPanel::indexOfPanel(const QByteArray &id) const
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->widget(i)
                ->property("sidebarPanelId").toByteArray() == id)
            return i;
    }
    return -1;
}

void SidebarPanel::applyPanelVisibility()
{
    // SIDE09: the persisted order (sidebar/panelOrder) is the rail
    // order — the registry only seeds it.
    const QList<Panel> ordered = orderedPanels();
    for (const Panel &panel : ordered) {
        const int index = indexOfPanel(panel.id);
        if (isPanelVisible(panel.id)) {
            if (index >= 0)
                continue;
            QWidget *page = nullptr;
            // SIDE09 lazy load: a placeholder holds the rail slot
            // until the section is first selected (buildPendingTab
            // swaps the real page in on currentChanged).
            if (m_lazyLoad) {
                page = new QWidget(m_tabs);
                page->setProperty("sidebarPanelPending", true);
            } else if (panel.create) {
                page = panel.create(this);
            }
            if (!page)
                continue;
            page->setProperty("sidebarPanelId", panel.id);
            // Persisted order is the rail order — slot the page
            // behind the enabled panels listed before it.
            int position = 0;
            for (const Panel &earlier : ordered) {
                if (earlier.id == panel.id)
                    break;
                if (indexOfPanel(earlier.id) >= 0)
                    ++position;
            }
            const int at = m_tabs->insertTab(position, page,
                AroraIcon::getChrome(QLatin1String(panel.iconName)),
                panel.title);
            m_tabs->setTabToolTip(at, panel.title);
        } else if (index >= 0) {
            QWidget *page = m_tabs->widget(index);
            // The downloads detail pane may be hosting a card owned by
            // DownloadManager — park it back before the page dies.
            if (page == m_downloadsPage && m_detailItem) {
                m_detailItem->setParent(DownloadManager::instance());
                m_detailItem->hide();
                m_detailItem.clear();
            }
            m_tabs->removeTab(index);
            page->deleteLater();
        }
    }
    // Reorder pass — moveTab fix-ups for entries that moved relative
    // to the persisted order; a no-op when the rail already matches.
    int want = 0;
    for (const Panel &panel : ordered) {
        const int at = indexOfPanel(panel.id);
        if (at < 0)
            continue;
        if (at != want)
            m_tabs->tabBar()->moveTab(at, want);
        ++want;
    }
}

// SIDE09 lazy load — the placeholder carries the rail slot until the
// page is first selected; the real page swaps in at the same index,
// keeping the icon, title and tooltip the slot already had.
void SidebarPanel::buildPendingTab(int index)
{
    QWidget *pending = index >= 0 ? m_tabs->widget(index) : nullptr;
    if (!pending || !pending->property("sidebarPanelPending").toBool())
        return;
    const QByteArray id =
        pending->property("sidebarPanelId").toByteArray();
    const Panel *desc = nullptr;
    const QList<Panel> all = panels();
    for (const Panel &panel : all) {
        if (panel.id == id) {
            desc = &panel;
            break;
        }
    }
    if (!desc || !desc->create)
        return;
    QWidget *page = desc->create(this);
    if (!page)
        return;
    page->setProperty("sidebarPanelId", id);
    const QIcon icon = m_tabs->tabIcon(index);
    // Blocked: the remove/insert swap shouldn't fire currentChanged
    // (and the autosaver) for a purely mechanical page swap.  Only
    // re-point the current index when the placeholder itself was
    // current — building a background slot must not steal selection.
    const bool wasCurrent = m_tabs->currentIndex() == index;
    const QSignalBlocker blocker(m_tabs);
    m_tabs->removeTab(index);
    pending->deleteLater();
    const int at = m_tabs->insertTab(index, page, icon, desc->title);
    m_tabs->setTabToolTip(at, desc->title);
    if (wasCurrent)
        m_tabs->setCurrentIndex(at);
}

// SIDE09: apply the working panel options — every checkbox in the
// Preferences Sidebar group maps to one of these switches (the
// floating dock's title-bar close button and auto-close live in
// BrowserMainWindow, which calls this).
void SidebarPanel::applyOptions()
{
    const QSettings settings;
    // 'Show Panel Toggle' — the rail's fold button.
    static_cast<SidebarRail *>(m_tabs->tabBar())->setToggleVisible(
        settings.value(QLatin1String("sidebar/showPanelToggle"), true)
            .toBool());
    // 'Show Close Button' — the page-header X (Downloads is the one
    // page that carries a header).  SIDE11 audit: no other page
    // (Bookmarks/History/Notes) hosts an in-panel X, and while docked
    // the dock's own title-bar X stays off — the header X and the
    // rail's fold button are the sidebar's only close controls, all
    // on the same closeRequested path.
    if (m_downloadsPage) {
        if (QWidget *closeX = m_downloadsPage->findChild<QWidget *>(
                QLatin1String("sidebarCloseButton")))
            closeX->setVisible(settings.value(
                QLatin1String("sidebar/showCloseButton"), true).toBool());
    }
    // 'Show Folder Counters' — flag only; the bookmarks model
    // consults it on fetch, so a repaint is the whole apply.
    s_folderCounters = settings.value(
        QLatin1String("sidebar/folderCounters"), true).toBool();
    if (m_bookmarksView)
        m_bookmarksView->viewport()->update();
    // 'Lazy Load' — affects what applyPanelVisibility inserts.
    // Turning it off materializes every still-pending page at once.
    const bool lazy = settings.value(QLatin1String("sidebar/lazyLoad"),
                                     false).toBool();
    if (m_lazyLoad && !lazy) {
        m_lazyLoad = false;
        for (int i = 0; i < m_tabs->count(); ++i)
            buildPendingTab(i);
    }
    m_lazyLoad = lazy;
}

// Bookmarks — the shared model behind the dialog/toolbar, with the
// same search-line + proxy presentation the dialog uses.
QWidget *SidebarPanel::buildBookmarksPage()
{
    m_bookmarksPage = new QWidget(m_tabs);
    QVBoxLayout *bookmarksLayout = new QVBoxLayout(m_bookmarksPage);
    bookmarksLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    bookmarksLayout->setSpacing(AroraStyle::Spacing::Small);
    SearchLineEdit *bookmarksSearch = new SearchLineEdit(m_bookmarksPage);
    bookmarksLayout->addWidget(bookmarksSearch);
    m_bookmarksView = new EditTreeView(m_bookmarksPage);
    // SIDE09: 'Show Folder Counters' — a proxy that appends " (n)"
    // to folder rows on DisplayRole only.
    m_bookmarksProxy = new SidebarBookmarksModel(m_bookmarksView);
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
    // SIDE06: name-only tree — the header is hidden but the Address
    // column still claims the slack (QTreeView stretches the last
    // section by default), pinning titles at ~100px ('Bo...ar' with
    // free space beside it).  Stretch column 0 across the viewport so
    // names elide only at the edge, and hide column 1; the context
    // menu's Edit Address unhides it for the duration of the edit.
    m_bookmarksView->setColumnHidden(1, true);
    m_bookmarksView->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_bookmarksView->header()->setStretchLastSection(false);
    connect(m_bookmarksView->itemDelegate(),
            &QAbstractItemDelegate::closeEditor, this, [this]() {
        m_bookmarksView->setColumnHidden(1, true);
    });
    connect(m_bookmarksView, &EditTreeView::activated,
            this, [this](const QModelIndex &index) {
        openBookmark(index, TabWidget::CurrentTab);
    });
    m_bookmarksView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_bookmarksView, &EditTreeView::customContextMenuRequested,
            this, &SidebarPanel::bookmarksContextMenu);
    bookmarksLayout->addWidget(m_bookmarksView);

    // SIDE04: this rail is the bookmarks manager now — carry the old
    // dialog's Add/Remove actions on a compact row under the tree.
    // Add is handled by the window (it owns the current tab); Remove
    // works on the shared model straight from here.
    // DOWN04: a wrapping row — at the dock's floor the buttons stack
    // instead of holding the whole panel above ~180px.
    FlowLayout *bookmarksActions = new FlowLayout(0, -1, 2);
    QPushButton *addButton = new QPushButton(tr("Add..."), m_bookmarksPage);
    addButton->setObjectName(QLatin1String("sidebarAddBookmark"));
    addButton->setAccessibleName(tr("Add bookmark"));
    connect(addButton, &QPushButton::clicked, this, [this]() {
        emit addBookmarkRequested(selectedBookmarkFolder());
    });
    bookmarksActions->addWidget(addButton);
    m_bookmarksRemoveButton =
        new QPushButton(tr("Remove"), m_bookmarksPage);
    m_bookmarksRemoveButton->setObjectName(
        QLatin1String("sidebarRemoveBookmark"));
    m_bookmarksRemoveButton->setAccessibleName(tr("Remove bookmark"));
    connect(m_bookmarksRemoveButton, &QPushButton::clicked,
            this, &SidebarPanel::removeSelectedBookmark);
    bookmarksActions->addWidget(m_bookmarksRemoveButton);
    bookmarksLayout->addLayout(bookmarksActions);
    connect(m_bookmarksView->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, &SidebarPanel::updateBookmarkRemoveState);
    connect(m_bookmarksProxy, &QAbstractItemModel::rowsRemoved,
            this, &SidebarPanel::updateBookmarkRemoveState);
    connect(m_bookmarksProxy, &QAbstractItemModel::modelReset,
            this, &SidebarPanel::updateBookmarkRemoveState);
    updateBookmarkRemoveState();
    return m_bookmarksPage;
}

// History — the day-grouped tree model the History dialog uses.
QWidget *SidebarPanel::buildHistoryPage()
{
    QWidget *historyPage = new QWidget(m_tabs);
    QVBoxLayout *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    historyLayout->setSpacing(AroraStyle::Spacing::Small);

    // HIST03: the reference's header controls — a period combo
    // narrowing the day groups plus the shared clear-history
    // confirmation on an icon button.  Both take the downloads
    // panel's compact treatment (Ignored policy, autoRaise glyph)
    // so the dock's ~160px floor stays reachable.
    QHBoxLayout *historyHeader = new QHBoxLayout;
    historyHeader->setSpacing(AroraStyle::Spacing::Small);
    SearchLineEdit *historySearch = new SearchLineEdit(historyPage);
    historySearch->setSizePolicy(QSizePolicy::Ignored,
                                 QSizePolicy::Fixed);
    historyHeader->addWidget(historySearch, 2);
    m_historyPeriod = new QComboBox(historyPage);
    m_historyPeriod->setObjectName(QLatin1String("sidebarHistoryPeriod"));
    m_historyPeriod->setAccessibleName(tr("Filter history by period"));
    m_historyPeriod->setToolTip(tr("Filter history by period"));
    m_historyPeriod->setSizePolicy(QSizePolicy::Ignored,
                                   QSizePolicy::Fixed);
    m_historyPeriod->addItem(tr("Today"),
                             HistoryPeriodFilterModel::Today);
    m_historyPeriod->addItem(tr("This Week"),
                             HistoryPeriodFilterModel::ThisWeek);
    m_historyPeriod->addItem(tr("This Month"),
                             HistoryPeriodFilterModel::ThisMonth);
    m_historyPeriod->addItem(tr("All"),
                             HistoryPeriodFilterModel::All);
    // Both header widgets are Ignored, so a stretch of 0 would leave
    // the combo at 0px — split the row 2:1 and let it elide.
    historyHeader->addWidget(m_historyPeriod, 1);
    QToolButton *clearButton = new QToolButton(historyPage);
    clearButton->setObjectName(QLatin1String("sidebarHistoryClear"));
    clearButton->setAccessibleName(tr("Clear history"));
    clearButton->setToolTip(tr("Clear history..."));
    clearButton->setAutoRaise(true);
    clearButton->setIcon(
        AroraIcon::getChrome(QLatin1String("edit-clear")));
    connect(clearButton, &QToolButton::clicked,
            this, [historyPage]() {
        HistoryManager::instance()->confirmClear(historyPage);
    });
    historyHeader->addWidget(clearButton);
    historyLayout->addLayout(historyHeader);

    m_historyView = new EditTreeView(historyPage);
    m_historyPeriodFilter = new HistoryPeriodFilterModel(m_historyView);
    m_historyPeriodFilter->setSourceModel(
        HistoryManager::instance()->historyTreeModel());
    m_historyProxy = new TreeSortFilterProxyModel(m_historyView);
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
    connect(m_historyPeriod, &QComboBox::activated,
            this, [this](int index) {
        const HistoryPeriodFilterModel::Period period =
            static_cast<HistoryPeriodFilterModel::Period>(
                m_historyPeriod->itemData(index).toInt());
        m_historyPeriodFilter->setPeriod(period);
        QSettings().setValue(QLatin1String("sidebar/historyPeriod"),
                             int(period));
        // A narrower range can hide the day group holding the view's
        // current row — the first (newest) group is always in range.
        m_historyView->setExpanded(m_historyProxy->index(0, 0), true);
    });
    m_historyProxy->setSourceModel(m_historyPeriodFilter);
    m_historyView->setModel(m_historyProxy);
    m_historyView->setExpanded(m_historyProxy->index(0, 0), true);
    // Persisted like the rest of the panel state; the default is All
    // so a first run hides nothing.
    const int savedPeriod = QSettings().value(
        QLatin1String("sidebar/historyPeriod"),
        int(HistoryPeriodFilterModel::All)).toInt();
    int periodRow = m_historyPeriod->findData(savedPeriod);
    if (periodRow < 0)
        periodRow = m_historyPeriod->findData(
            int(HistoryPeriodFilterModel::All));
    m_historyPeriod->setCurrentIndex(periodRow);
    m_historyPeriodFilter->setPeriod(
        static_cast<HistoryPeriodFilterModel::Period>(
            m_historyPeriod->itemData(periodRow).toInt()));
    // SIDE06: same name-only treatment — column 1 (address / item
    // count) hoarded the slack; hide it so titles fill the panel.
    m_historyView->setColumnHidden(1, true);
    m_historyView->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_historyView->header()->setStretchLastSection(false);
    connect(m_historyView, &EditTreeView::activated,
            this, [this](const QModelIndex &index) {
        openHistoryEntry(index, TabWidget::CurrentTab);
    });
    m_historyView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_historyView, &EditTreeView::customContextMenuRequested,
            this, &SidebarPanel::historyContextMenu);
    historyLayout->addWidget(m_historyView);
    return historyPage;
}

// DOWN04: no bundled set ships a pause glyph (media-playback-pause
// isn't covered) — paint two bars in ButtonText like the rail icon so
// the action row stays monochrome under every palette, dark included.
static QIcon downloadPauseIcon(const QWidget *forPalette)
{
    const qreal dpr = forPalette ? forPalette->devicePixelRatioF() : 1.0;
    QPixmap pixmap(16 * dpr, 16 * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(forPalette
        ? forPalette->palette().color(QPalette::ButtonText)
        : QColor(Qt::black));
    painter.drawRoundedRect(QRectF(4.4, 3.4, 3.0, 9.2), 1.0, 1.0);
    painter.drawRoundedRect(QRectF(9.0, 3.4, 3.0, 9.2), 1.0, 1.0);
    return QIcon(pixmap);
}

// DOWN02: downloads — the standalone manager window is gone; this
// panel is the download surface.  Header close X, a filter field,
// a sort combo, the item list and a bottom detail pane that hosts
// the selected row's real DownloadItem card (DOWN01).
QWidget *SidebarPanel::buildDownloadsPage()
{
    m_downloadsPage = new QWidget(m_tabs);
    QVBoxLayout *downloadsLayout = new QVBoxLayout(m_downloadsPage);
    downloadsLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    downloadsLayout->setSpacing(AroraStyle::Spacing::Small);

    QHBoxLayout *downloadsHeader = new QHBoxLayout;
    QLabel *downloadsTitle = new QLabel(tr("Downloads"), m_downloadsPage);
    QFont titleFont = downloadsTitle->font();
    // THEM03: header rows share the section-title weight the chrome
    // style gives group boxes (DemiBold, not full Bold).
    titleFont.setWeight(QFont::DemiBold);
    downloadsTitle->setFont(titleFont);
    // DOWN04: the title yields first at narrow widths so the close
    // button is never pushed off the row's right edge.
    downloadsTitle->setSizePolicy(QSizePolicy::Ignored,
                                  QSizePolicy::Preferred);
    downloadsHeader->addWidget(downloadsTitle, 1);
    QToolButton *closeButton = new QToolButton(m_downloadsPage);
    closeButton->setObjectName(QLatin1String("sidebarCloseButton"));
    closeButton->setAccessibleName(tr("Close sidebar"));
    closeButton->setToolTip(tr("Close sidebar"));
    closeButton->setAutoRaise(true);
    // ICONS09: house close glyph — palette-tinted chrome like every
    // other in-panel button instead of a style-pixmap outlier.
    closeButton->setIcon(
        AroraIcon::getChrome(QLatin1String("window-close")));
    connect(closeButton, &QToolButton::clicked,
            this, &SidebarPanel::closeRequested);
    downloadsHeader->addWidget(closeButton);
    downloadsLayout->addLayout(downloadsHeader);

    SearchLineEdit *downloadsSearch = new SearchLineEdit(m_downloadsPage);
    downloadsSearch->setObjectName(QLatin1String("sidebarDownloadsSearch"));
    downloadsSearch->setAccessibleName(tr("Filter downloads"));
    connect(downloadsSearch, &SearchLineEdit::textChanged,
            this, [this](const QString &text) {
        static_cast<SidebarDownloadsModel*>(m_downloadsProxy.data())
            ->setSearchText(text);
    });
    downloadsLayout->addWidget(downloadsSearch);

    m_downloadsSort = new QComboBox(m_downloadsPage);
    m_downloadsSort->setObjectName(QLatin1String("sidebarDownloadsSort"));
    m_downloadsSort->setAccessibleName(tr("Sort downloads"));
    // DOWN04: compress (eliding the text) instead of forcing the dock
    // wider when the sidebar is at its narrowest.
    m_downloadsSort->setSizePolicy(QSizePolicy::Ignored,
                                   QSizePolicy::Fixed);
    m_downloadsSort->addItem(tr("Sort by Date"), DownloadModel::StartedTimeRole);
    m_downloadsSort->addItem(tr("Sort by Name"), DownloadModel::FileNameRole);
    m_downloadsSort->addItem(tr("Sort by Size"), DownloadModel::SizeRole);
    connect(m_downloadsSort, &QComboBox::activated,
            this, [this](int) { applyDownloadSort(); });
    downloadsLayout->addWidget(m_downloadsSort);

    m_downloadsView = new QListView(m_downloadsPage);
    m_downloadsProxy = new SidebarDownloadsModel(
        DownloadManager::instance()->model(), m_downloadsView);
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

    // DLUX01: a started download surfaces itself — select its row so
    // the detail pane below hosts the live card, and keep it scrolled
    // into view.  No window-raise: the dock stays put (UIAUDIT02).
    connect(DownloadManager::instance(), &DownloadManager::itemAdded,
            this, [this](DownloadItem *item) {
        if (!m_downloadsView || !m_downloadsProxy)
            return;
        const int row = DownloadManager::instance()->rowForItem(item);
        if (row < 0)
            return;
        const QModelIndex index = m_downloadsProxy->mapFromSource(
            m_downloadsProxy->sourceModel()->index(row, 0));
        if (!index.isValid())
            return;
        m_downloadsView->setCurrentIndex(index);
        m_downloadsView->scrollTo(index, QAbstractItemView::EnsureVisible);
    });

    // Bottom detail pane — the selected item's DownloadItem card is
    // reparented in here, so the pane literally IS the card (live
    // progress, Restart and Show in Folder included).
    m_downloadDetail = new QFrame(m_downloadsPage);
    m_downloadDetail->setObjectName(QLatin1String("sidebarDownloadDetail"));
    m_downloadDetail->setAccessibleName(tr("Download details"));
    m_downloadDetail->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout *detailLayout = new QVBoxLayout(m_downloadDetail);
    detailLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    // The card keeps its full height — the scrolling list above
    // absorbs vertical deficits, the hosted rows never clip.
    m_downloadDetail->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Minimum);
    m_downloadDetail->setVisible(false);
    downloadsLayout->addWidget(m_downloadDetail);
    // Removing/cleaning rows deletes the hosted card — fold the pane
    // before the pending deleteLater runs.
    connect(DownloadManager::instance()->model(),
            &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &, int first, int last) {
        if (!m_detailItem || !m_downloadDetail)
            return;
        for (int row = first; row <= last; ++row) {
            if (DownloadManager::instance()->itemAt(row) == m_detailItem) {
                m_detailItem.clear();
                m_downloadDetail->setVisible(false);
                break;
            }
        }
    });

    // DOWN04: the action row the user's reference grab shows —
    // pause/resume, retry, remove and show-in-folder on icon-sized
    // buttons, plus a details toggle for the card's speed graph.
    // Every button acts on the SELECTED list row and mirrors the
    // hosted card's own enablement (can't pause a finished download,
    // can't retry a running one, can't remove an in-flight row), so
    // the row and the card never disagree.  Glyphs come through
    // AroraIcon::getChrome — the same palette-tinted monochrome chrome
    // as the header X — staying legible on dark and tor palettes.
    // A FlowLayout keeps the dock's 160px floor reachable — the
    // buttons reflow onto a second row instead of forcing a ~180px
    // minimum width (same treatment as the card's own button rows).
    FlowLayout *actionsRow = new FlowLayout(0, 2, 2);
    const auto actionButton = [this, actionsRow](const char *objectName,
                                                 const QString &toolTip) {
        QToolButton *button = new QToolButton(m_downloadsPage);
        button->setObjectName(QLatin1String(objectName));
        button->setAutoRaise(true);
        button->setToolTip(toolTip);
        button->setAccessibleName(toolTip);
        button->setEnabled(false);
        actionsRow->addWidget(button);
        return button;
    };
    m_downloadPauseButton = actionButton("sidebarDownloadPause",
                                         tr("Pause download"));
    m_downloadPauseButton->setIcon(downloadPauseIcon(m_downloadsPage));
    connect(m_downloadPauseButton, &QToolButton::clicked,
            this, &SidebarPanel::pauseSelectedDownload);
    m_downloadRetryButton = actionButton("sidebarDownloadRetry",
                                         tr("Restart download"));
    m_downloadRetryButton->setIcon(
        AroraIcon::getChrome(QLatin1String("view-refresh")));
    connect(m_downloadRetryButton, &QToolButton::clicked,
            this, &SidebarPanel::retrySelectedDownload);
    m_downloadRemoveButton = actionButton("sidebarDownloadRemove",
                                          tr("Remove from list"));
    m_downloadRemoveButton->setIcon(
        AroraIcon::getChrome(QLatin1String("edit-clear")));
    connect(m_downloadRemoveButton, &QToolButton::clicked,
            this, &SidebarPanel::removeSelectedDownload);
    m_downloadFolderButton = actionButton("sidebarDownloadFolder",
                                          tr("Show in File Manager"));
    m_downloadFolderButton->setIcon(
        AroraIcon::getChrome(QLatin1String("document-open")));
    connect(m_downloadFolderButton, &QToolButton::clicked,
            this, &SidebarPanel::revealSelectedDownload);
    m_downloadDetailsButton = actionButton("sidebarDownloadDetails",
                                           tr("Show download details"));
    m_downloadDetailsButton->setCheckable(true);
    m_downloadDetailsButton->setIcon(
        AroraIcon::getChrome(QLatin1String("go-down")));
    connect(m_downloadDetailsButton, &QToolButton::toggled,
            this, &SidebarPanel::setSelectedDownloadExpanded);
    downloadsLayout->addLayout(actionsRow);

    QPushButton *settingsButton = new QPushButton(tr("Download Settings..."),
                                                m_downloadsPage);
    settingsButton->setObjectName(QLatin1String("sidebarDownloadSettings"));
    // DOWN04: same treatment — a wide push-button minimum must not
    // hold the whole dock above its 160px floor.
    settingsButton->setSizePolicy(QSizePolicy::Ignored,
                                  QSizePolicy::Fixed);
    connect(settingsButton, &QPushButton::clicked, this, [this]() {
        SettingsDialog::openPage(this, SettingsDialog::DownloadsPage);
    });
    downloadsLayout->addWidget(settingsButton);
    // A row may already be selected (the view auto-selects on a
    // pre-populated model) — bring the action row in sync now that
    // the buttons exist.
    updateDownloadActions();
    return m_downloadsPage;
}

// NOTE02: Notes — a per-profile multi-note store (NotesModel) behind
// a three-state stack: the "Write your first note" empty state, the
// notes list, and the editor a selected note opens in.  Persistence
// is the model's debounced autosave; the page carries no save button.
QWidget *SidebarPanel::buildNotesPage()
{
    QWidget *notesPage = new QWidget(m_tabs);
    QVBoxLayout *notesLayout = new QVBoxLayout(notesPage);
    notesLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    notesLayout->setSpacing(AroraStyle::Spacing::Small);

    // NOTE02: the window's browsing profile owns the store — the
    // shared normal-profile model in a regular window, a session-only
    // volatile one while private/tor (an off-the-record profile's
    // notes never reach disk).  SidebarPanel itself stays
    // engine-free: it only ever sees the model.
    m_notesModel = NotesModel::instance(BrowserApplication::webEngineProfile());
    // A rebuilt page (panel hidden then re-enabled) starts at the
    // list — the old editor's binding dies with the old widgets.
    m_editingNote = QPersistentModelIndex();

    // Header row — the add/delete buttons sit beside the spot
    // NOTE03's search field will take.
    QHBoxLayout *notesHeader = new QHBoxLayout;
    notesHeader->setSpacing(AroraStyle::Spacing::Small);
    QLabel *notesTitle = new QLabel(tr("Notes"), notesPage);
    QFont titleFont = notesTitle->font();
    titleFont.setBold(true);
    notesTitle->setFont(titleFont);
    notesTitle->setSizePolicy(QSizePolicy::Ignored,
                              QSizePolicy::Preferred);
    notesHeader->addWidget(notesTitle, 1);
    const auto noteButton = [this, notesPage, notesHeader](
            const char *objectName, const QString &label,
            const char *iconName) {
        QToolButton *button = new QToolButton(notesPage);
        button->setObjectName(QLatin1String(objectName));
        button->setAutoRaise(true);
        button->setIcon(AroraIcon::getChrome(QLatin1String(iconName)));
        button->setToolTip(label);
        button->setAccessibleName(label);
        notesHeader->addWidget(button);
        return button;
    };
    QToolButton *addButton = noteButton("sidebarNotesAdd",
                                        tr("New note"), "list-add");
    connect(addButton, &QToolButton::clicked,
            this, &SidebarPanel::addNote);
    m_notesRemoveButton = noteButton("sidebarNotesRemove",
                                     tr("Delete note"), "edit-clear");
    connect(m_notesRemoveButton, &QToolButton::clicked,
            this, &SidebarPanel::removeSelectedNote);
    notesLayout->addLayout(notesHeader);

    m_notesStack = new QStackedWidget(notesPage);
    m_notesStack->setObjectName(QLatin1String("sidebarNotesStack"));

    // Empty state — zero notes.
    QLabel *emptyLabel = new QLabel(
        tr("Write your first note."), m_notesStack);
    emptyLabel->setObjectName(QLatin1String("sidebarNotesEmpty"));
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setWordWrap(true);
    emptyLabel->setForegroundRole(QPalette::PlaceholderText);
    m_notesStack->addWidget(emptyLabel);

    // The notes list — a click opens the note in the editor, F2
    // renames the title in place.
    m_notesList = new QListView(m_notesStack);
    m_notesList->setObjectName(QLatin1String("sidebarNotesList"));
    m_notesList->setAccessibleName(tr("Notes"));
    m_notesList->setUniformItemSizes(true);
    m_notesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_notesList->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_notesList->setModel(m_notesModel);
    connect(m_notesList, &QListView::activated,
            this, &SidebarPanel::openNote);
    connect(m_notesList, &QListView::clicked,
            this, &SidebarPanel::openNote);
    m_notesStack->addWidget(m_notesList);

    // The editor — back button, the note's title (renaming edits the
    // same row the list shows), then the body.
    QWidget *editorPage = new QWidget(m_notesStack);
    QVBoxLayout *editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(AroraStyle::Spacing::Small);
    QHBoxLayout *editorHeader = new QHBoxLayout;
    editorHeader->setSpacing(AroraStyle::Spacing::Small);
    QToolButton *backButton = new QToolButton(editorPage);
    backButton->setObjectName(QLatin1String("sidebarNotesBack"));
    backButton->setAutoRaise(true);
    backButton->setIcon(AroraIcon::getChrome(QLatin1String("go-previous")));
    backButton->setToolTip(tr("Back to notes"));
    backButton->setAccessibleName(tr("Back to notes"));
    connect(backButton, &QToolButton::clicked,
            this, &SidebarPanel::closeNoteEditor);
    editorHeader->addWidget(backButton);
    m_noteTitle = new QLineEdit(editorPage);
    m_noteTitle->setObjectName(QLatin1String("sidebarNoteTitle"));
    m_noteTitle->setAccessibleName(tr("Note title"));
    m_noteTitle->setPlaceholderText(tr("Title"));
    editorHeader->addWidget(m_noteTitle, 1);
    editorLayout->addLayout(editorHeader);
    m_notes = new QPlainTextEdit(editorPage);
    m_notes->setObjectName(QLatin1String("sidebarNotes"));
    m_notes->setAccessibleName(tr("Note text"));
    m_notes->setPlaceholderText(tr("Notes are saved automatically."));
    editorLayout->addWidget(m_notes);
    m_notesStack->addWidget(editorPage);
    notesLayout->addWidget(m_notesStack);

    // Editor -> model.  Loading a note into the widgets blocks these
    // so a page switch never reads as an edit.
    connect(m_notes, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_notesModel && m_editingNote.isValid())
            m_notesModel->setData(m_editingNote, m_notes->toPlainText(),
                                  NotesModel::BodyRole);
    });
    connect(m_noteTitle, &QLineEdit::textChanged, this,
            [this](const QString &title) {
        if (m_notesModel && m_editingNote.isValid())
            m_notesModel->setData(m_editingNote, title, Qt::EditRole);
    });
    // A removal that takes out the open note folds the editor back to
    // whichever list state applies.
    if (m_notesModel) {
        connect(m_notesModel, &QAbstractItemModel::rowsRemoved,
                this, &SidebarPanel::updateNotesView);
        connect(m_notesModel, &QAbstractItemModel::modelReset,
                this, &SidebarPanel::updateNotesView);
    }
    updateNotesView();
    return notesPage;
}

// The stack page for the current model state — the empty state when
// no notes exist, the list otherwise (the editor is only ever shown
// by openNote / left by closeNoteEditor or a removal).  The − button
// stays live in the editor too — it deletes the open note.
void SidebarPanel::updateNotesView()
{
    if (!m_notesStack)
        return;
    const bool hasNote = m_editingNote.isValid()
        || (m_notesModel && m_notesModel->rowCount() > 0);
    if (m_notesRemoveButton)
        m_notesRemoveButton->setEnabled(hasNote);
    if (m_editingNote.isValid())
        return;
    m_notesStack->setCurrentIndex(hasNote ? 1 : 0);
}

void SidebarPanel::openNote(const QModelIndex &index)
{
    if (!m_notesModel || !index.isValid())
        return;
    m_editingNote = index;
    // Select in the list too so the − button and a later delete land
    // on the same row the editor is showing.
    m_notesList->setCurrentIndex(index);
    {
        const QSignalBlocker titleBlocker(m_noteTitle);
        const QSignalBlocker bodyBlocker(m_notes);
        m_noteTitle->setText(index.data(Qt::EditRole).toString());
        m_notes->setPlainText(index.data(NotesModel::BodyRole).toString());
    }
    m_notesStack->setCurrentIndex(2);
    updateNotesView();
    m_notes->setFocus();
}

void SidebarPanel::closeNoteEditor()
{
    m_editingNote = QPersistentModelIndex();
    updateNotesView();
}

void SidebarPanel::addNote()
{
    if (!m_notesModel)
        return;
    const QModelIndex index = m_notesModel->addNote();
    openNote(index);
    // A fresh note is renamed by typing — preselect the empty title
    // field so the first keystrokes name it.
    m_noteTitle->setFocus();
}

void SidebarPanel::removeSelectedNote()
{
    if (!m_notesModel)
        return;
    QModelIndex index = m_editingNote.isValid()
        ? QModelIndex(m_editingNote)
        : m_notesList->currentIndex();
    if (!index.isValid())
        return;
    const QString title = index.data(Qt::DisplayRole).toString();
    const QString body = index.data(NotesModel::BodyRole).toString();
    if (!title.isEmpty() || !body.isEmpty()) {
        if (QMessageBox::question(m_notesStack, tr("Delete Note"),
                tr("Delete the note \"%1\"?").arg(title),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    // The persistent index invalidates on removal — clear the editing
    // binding first so updateNotesView() can pick the right state.
    m_editingNote = QPersistentModelIndex();
    m_notesModel->removeRow(index.row());
    updateNotesView();
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

NotesModel *SidebarPanel::notesModel() const
{
    return m_notesModel;
}

void SidebarPanel::save()
{
    QSettings settings;
    // NOTE02: the notes text is the NotesModel's own file-backed
    // store now — flush it here so callers that force a save (window
    // teardown, tests) can't strand a pending debounce.
    if (m_notesModel)
        m_notesModel->save();
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
    if (!index.isValid() || !node)
        return;
    QMenu menu;
    // SIDE07: the panel is the "Show All Bookmarks" surface now, so
    // the menu carries the standalone dialog's management actions,
    // not just the opens.
    if (node->type() != BookmarkNode::Folder) {
        menu.addAction(tr("Open"), this, [this, index]() {
            openBookmark(index, TabWidget::CurrentTab);
        });
        menu.addAction(tr("Open in New Tab"), this, [this, index]() {
            openBookmark(index, TabWidget::NewSelectedTab);
        });
        menu.addSeparator();
    }
    menu.addAction(tr("New Folder"), this, [this, index]() {
        newBookmarkFolder(index);
    });
    menu.addSeparator();
    QAction *renameAction = menu.addAction(tr("Edit Name"), this,
                                           [this, index]() {
        m_bookmarksView->edit(index.siblingAtColumn(0));
    });
    renameAction->setEnabled(index.flags() & Qt::ItemIsEditable);
    if (node->type() != BookmarkNode::Folder) {
        menu.addAction(tr("Edit Address"), this, [this, index]() {
            // SIDE06: the Address column is hidden — unhide it long
            // enough to host the editor; the delegate's closeEditor
            // hook folds it back on commit or cancel.
            m_bookmarksView->setColumnHidden(1, false);
            m_bookmarksView->edit(index.siblingAtColumn(1));
        });
    }
    menu.addSeparator();
    // SIDE04/SIDE07: same wording as the history rail's entry so the
    // remove capability is discoverable in both places.
    QAction *deleteAction = menu.addAction(tr("Remove Entry"), m_bookmarksView,
                                           &EditTreeView::removeSelected);
    deleteAction->setEnabled(index.flags() & Qt::ItemIsDragEnabled);
    menu.exec(QCursor::pos());
}

// Same placement rule the standalone BookmarksDialog::newFolder uses:
// a folder selected rows appends inside it, a leaf's row lands right
// below it in the parent folder.
void SidebarPanel::newBookmarkFolder(const QModelIndex &index)
{
    QModelIndex idx = index;
    QModelIndex sourceIndex = m_bookmarksProxy->mapToSource(idx);
    const BookmarkNode *sourceNode =
        BookmarksManager::instance()->bookmarksModel()->node(sourceIndex);
    int row = -1; // append inside the selected folder
    if (sourceNode && sourceNode->type() != BookmarkNode::Folder) {
        idx = idx.parent();
        row = index.row() + 1;
    }
    if (!idx.isValid())
        idx = m_bookmarksProxy->index(1, 0);
    idx = m_bookmarksProxy->mapToSource(idx);
    BookmarkNode *parent =
        BookmarksManager::instance()->bookmarksModel()->node(idx);
    BookmarkNode *node = new BookmarkNode(BookmarkNode::Folder);
    node->title = tr("New Folder");
    BookmarksManager::instance()->addBookmark(parent, node, row);
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

QWidget *SidebarPanel::bookmarksPage() const
{
    return m_bookmarksPage;
}

// SIDE09: id-keyed select — a lazy placeholder materializes first,
// and a panel unchecked in Preferences simply isn't on the rail (the
// dock still raises, nothing selects).
void SidebarPanel::showPanel(const QByteArray &id)
{
    const int index = indexOfPanel(id);
    if (index < 0)
        return;
    buildPendingTab(index);
    m_tabs->setCurrentIndex(index);
}

void SidebarPanel::showDownloads()
{
    showPanel(QByteArrayLiteral("downloads"));
}

// SIDE06: the rail hugs the dock's outer edge — icons on the left
// when the dock is on the left, on the right when it is docked right
// (or floating).  Re-applied by the host window on every dock move.
void SidebarPanel::setDockSide(Qt::DockWidgetArea area)
{
    m_tabs->setTabPosition(area == Qt::RightDockWidgetArea
                           ? QTabWidget::East : QTabWidget::West);
}

// SIDE04: Ctrl+Shift+B / Bookmarks > Show All Bookmarks lands here —
// the caller raises the dock; this picks the rail entry and focuses
// the tree.
void SidebarPanel::showBookmarks()
{
    showPanel(QByteArrayLiteral("bookmarks"));
    if (m_bookmarksView)
        m_bookmarksView->setFocus();
}

// The folder the Add dialog should open on: the selected folder, a
// bookmark's parent folder, or invalid (the window falls back to the
// bookmarks menu root).
QModelIndex SidebarPanel::selectedBookmarkFolder() const
{
    if (!m_bookmarksView || !m_bookmarksProxy)
        return QModelIndex();
    const QModelIndex source = m_bookmarksProxy->mapToSource(
        m_bookmarksView->currentIndex());
    const BookmarkNode *node = source.isValid()
        ? BookmarksManager::instance()->bookmarksModel()->node(source)
        : nullptr;
    if (!node)
        return QModelIndex();
    if (node->type() == BookmarkNode::Folder)
        return source;
    return source.parent();
}

void SidebarPanel::removeSelectedBookmark()
{
    const QModelIndex index = m_bookmarksView->currentIndex();
    const QModelIndex source = m_bookmarksProxy->mapToSource(index);
    const BookmarkNode *node = source.isValid()
        ? BookmarksManager::instance()->bookmarksModel()->node(source)
        : nullptr;
    if (!node || node->type() != BookmarkNode::Bookmark)
        return;
    // Same path as the view's Del key — removeRow through the proxy
    // maps onto the shared model's undoable removal.
    m_bookmarksProxy->removeRow(index.row(), index.parent());
}

void SidebarPanel::updateBookmarkRemoveState()
{
    bool canRemove = false;
    if (m_bookmarksView && m_bookmarksProxy) {
        const QModelIndex index = m_bookmarksView->currentIndex();
        const QModelIndex source = m_bookmarksProxy->mapToSource(index);
        const BookmarkNode *node = source.isValid()
            ? BookmarksManager::instance()->bookmarksModel()->node(source)
            : nullptr;
        canRemove = node && node->type() == BookmarkNode::Bookmark;
    }
    if (m_bookmarksRemoveButton)
        m_bookmarksRemoveButton->setEnabled(canRemove);
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
        // DOWN04: a hosted card must never auto-collapse — the pane's
        // only content IS the card, so the finish/stop fold would hide
        // every action the pane exists for (the reported "buttons
        // entirely absent" grab).
        item->pinExpanded();
        // The action row tracks the card's live state: statusChanged
        // covers state flips and pause/resume, the card's own chevron
        // keeps the details toggle honest when the user folds the
        // card inline.
        connect(item, &DownloadItem::statusChanged,
                this, &SidebarPanel::updateDownloadActions,
                Qt::UniqueConnection);
        connect(item, &DownloadItem::expandedChanged,
                this, &SidebarPanel::updateDownloadActions,
                Qt::UniqueConnection);
        // The card's cached size hint predates the expansion — refresh
        // it so the pane's layout allocates the card's full height.
        item->adjustSize();
        item->show();
    }
    m_downloadDetail->setVisible(item != nullptr);
    updateDownloadActions();
}

// DOWN04: the compact action row.  selectedDownloadItem() re-reads the
// list's current row on every call so the buttons can never act on a
// stale card after a re-sort or a removal.
DownloadItem *SidebarPanel::selectedDownloadItem() const
{
    if (!m_downloadsView || !m_downloadsProxy)
        return nullptr;
    const QModelIndex source = m_downloadsProxy->mapToSource(
        m_downloadsView->currentIndex());
    return source.isValid()
        ? DownloadManager::instance()->itemAt(source.row())
        : nullptr;
}

void SidebarPanel::updateDownloadActions()
{
    if (!m_downloadPauseButton)
        return;
    DownloadItem *item = selectedDownloadItem();
    const bool paused = item && item->isPaused();
    m_downloadPauseButton->setEnabled(item && item->canPause()
                                      && item->downloading());
    m_downloadPauseButton->setIcon(paused
        ? AroraIcon::getChrome(QLatin1String("system-run"))
        : downloadPauseIcon(m_downloadsPage));
    const QString pauseLabel = paused ? tr("Resume download")
                                      : tr("Pause download");
    m_downloadPauseButton->setToolTip(pauseLabel);
    m_downloadPauseButton->setAccessibleName(pauseLabel);

    // One button covers both card verbs — Try Again (failed/stopped)
    // and Restart (finished) run the same restartDownload() path.
    m_downloadRetryButton->setEnabled(item && item->canRestart());
    const QString retryLabel = item && !item->downloadedSuccessfully()
        ? tr("Try Again") : tr("Restart download");
    m_downloadRetryButton->setToolTip(retryLabel);
    m_downloadRetryButton->setAccessibleName(retryLabel);

    m_downloadRemoveButton->setEnabled(item && item->isRemovable());
    m_downloadFolderButton->setEnabled(item
                                       && !item->m_outputFileName.isEmpty());

    const bool expanded = item && item->isExpanded();
    m_downloadDetailsButton->setEnabled(item != nullptr);
    m_downloadDetailsButton->setChecked(expanded);
    m_downloadDetailsButton->setIcon(AroraIcon::getChrome(
        expanded ? QLatin1String("go-up") : QLatin1String("go-down")));
    const QString detailsLabel = expanded ? tr("Hide download details")
                                          : tr("Show download details");
    m_downloadDetailsButton->setToolTip(detailsLabel);
    m_downloadDetailsButton->setAccessibleName(detailsLabel);
}

void SidebarPanel::pauseSelectedDownload()
{
    if (DownloadItem *item = selectedDownloadItem())
        QMetaObject::invokeMethod(item, "pauseOrResume");
    updateDownloadActions();
}

void SidebarPanel::retrySelectedDownload()
{
    if (DownloadItem *item = selectedDownloadItem())
        QMetaObject::invokeMethod(item, "restart");
    updateDownloadActions();
}

void SidebarPanel::removeSelectedDownload()
{
    if (!m_downloadsView || !m_downloadsProxy)
        return;
    const QModelIndex source = m_downloadsProxy->mapToSource(
        m_downloadsView->currentIndex());
    if (source.isValid())
        m_downloadsProxy->sourceModel()->removeRow(source.row());
    updateDownloadActions();
}

void SidebarPanel::revealSelectedDownload()
{
    if (DownloadItem *item = selectedDownloadItem())
        QMetaObject::invokeMethod(item, "showInFolder");
}

void SidebarPanel::setSelectedDownloadExpanded(bool expanded)
{
    if (DownloadItem *item = selectedDownloadItem())
        item->setExpanded(expanded);
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
