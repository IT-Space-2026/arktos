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
#include "downloadmanager.h"
#include "history.h"
#include "historymanager.h"
#include "searchlineedit.h"
#include "settings.h"
#include "utils/aroraicon.h"
#include "utils/edittreeview.h"
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
#include <qlistview.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qshortcut.h>
#include <QSignalBlocker>
#include <qsortfilterproxymodel.h>
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
    if (m_notes)
        m_notes->setPlainText(
            settings.value(QLatin1String("sidebar/notes")).toString());
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
// Preferences Sidebar group maps to one of these switches (the dock
// title-bar close button and auto-close live in BrowserMainWindow's
// applySidebarSettings, which calls this).
void SidebarPanel::applyOptions()
{
    const QSettings settings;
    // 'Show Panel Toggle' — the rail's fold button.
    static_cast<SidebarRail *>(m_tabs->tabBar())->setToggleVisible(
        settings.value(QLatin1String("sidebar/showPanelToggle"), true)
            .toBool());
    // 'Show Close Button' — the page-header X (Downloads is the one
    // page that carries a header).
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
    QWidget *bookmarksPage = m_bookmarksPage;
    QVBoxLayout *bookmarksLayout = new QVBoxLayout(bookmarksPage);
    bookmarksLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    bookmarksLayout->setSpacing(AroraStyle::Spacing::Small);
    SearchLineEdit *bookmarksSearch = new SearchLineEdit(bookmarksPage);
    bookmarksLayout->addWidget(bookmarksSearch);
    m_bookmarksView = new EditTreeView(bookmarksPage);
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
    return bookmarksPage;
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
    SearchLineEdit *historySearch = new SearchLineEdit(historyPage);
    historyLayout->addWidget(historySearch);
    m_historyView = new EditTreeView(historyPage);
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
    m_historyProxy->setSourceModel(
        HistoryManager::instance()->historyTreeModel());
    m_historyView->setModel(m_historyProxy);
    m_historyView->setExpanded(m_historyProxy->index(0, 0), true);
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
    return m_downloadsPage;
}

// Notes — a per-profile scratch pad, debounce-persisted through
// the AutoSaver like every other app-side store.
QWidget *SidebarPanel::buildNotesPage()
{
    QWidget *notesPage = new QWidget(m_tabs);
    QVBoxLayout *notesLayout = new QVBoxLayout(notesPage);
    notesLayout->setContentsMargins(
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small,
        AroraStyle::Spacing::Small, AroraStyle::Spacing::Small);
    notesLayout->setSpacing(AroraStyle::Spacing::Small);
    m_notes = new QPlainTextEdit(notesPage);
    m_notes->setObjectName(QLatin1String("sidebarNotes"));
    m_notes->setAccessibleName(tr("Notes"));
    m_notes->setPlaceholderText(tr("Notes are saved automatically."));
    notesLayout->addWidget(m_notes);

    connect(m_notes, &QPlainTextEdit::textChanged,
            m_autoSaver, &AutoSaver::changeOccurred);
    // The panel may be re-enabled at runtime — a stored note picks
    // back up without waiting for a restart.
    m_notes->setPlainText(
        QSettings().value(QLatin1String("sidebar/notes")).toString());
    return notesPage;
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
    // A hidden Notes panel keeps its stored text — don't clobber the
    // key with an empty editor state.
    if (m_notes)
        settings.setValue(QLatin1String("sidebar/notes"),
                          m_notes->toPlainText());
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
    QAction *deleteAction = menu.addAction(tr("Delete"), m_bookmarksView,
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

void SidebarPanel::showBookmarks()
{
    showPanel(QByteArrayLiteral("bookmarks"));
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
