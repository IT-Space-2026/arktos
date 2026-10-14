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
#ifndef SIDEBARPANEL_H
#define SIDEBARPANEL_H

#include "tabwidget.h"

#include <functional>

#include <qlist.h>
#include <QModelIndex>
#include <qpointer.h>
#include <qstringlist.h>
#include <qwidget.h>

class AutoSaver;
class DownloadItem;
class EditTreeView;
class QComboBox;
class QFrame;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTabWidget;
class QToolButton;
class TreeSortFilterProxyModel;

// SIDE01: the content widget of the optional Vivaldi-style sidebar
// dock.  Four switchable sections — bookmarks, history, downloads and
// a scratch-notes editor — over the existing shared models.
class SidebarPanel : public QWidget
{
    Q_OBJECT

signals:
    void openUrl(const QUrl &url, TabWidget::OpenUrlIn tab,
                 const QString &title);
    // DOWN02: the Downloads page header's X asks the host window to
    // fold the whole dock away.
    void closeRequested();
    // SIDE04: the bookmarks rail's Add button — the host window owns
    // the current tab so it fills the dialog; folder is the source
    // model index to preselect (invalid = the bookmarks menu root).
    void addBookmarkRequested(const QModelIndex &folder);

public:
    // SIDE02: one entry on the rail — a panel registers (id, icon,
    // title, factory) instead of the constructor knowing every
    // section.  The id doubles as the sidebar/panels/<id> visibility
    // key; hidden panels are never constructed.
    struct Panel {
        QByteArray id;
        QString title;
        QByteArray iconName;    // freedesktop-style AroraIcon name
        std::function<QWidget *(SidebarPanel *)> create;
    };

    static QList<Panel> panels();
    static void registerPanel(const Panel &panel);
    // SIDE09: the rail order — persisted under sidebar/panelOrder as
    // a list of panel ids.  orderedPanels() re-sequences the registry
    // by it; ids the key doesn't mention (fresh registrations) append
    // in registry order, and stale ids in the key are ignored.
    static QStringList panelOrder();
    static void setPanelOrder(const QStringList &order);
    static QList<Panel> orderedPanels();
    static bool isPanelVisible(const QByteArray &id);
    static void setPanelVisible(const QByteArray &id, bool visible);
    // 'Show Folder Counters' (sidebar/folderCounters) — read live by
    // the bookmarks list model; applyOptions() syncs it.
    static bool showsFolderCounters();

    explicit SidebarPanel(QWidget *parent = nullptr);
    ~SidebarPanel() override;

    // Palette-following toggle glyph — the bundled icon themes ship no
    // sidebar pictogram and new artwork files stay out per the
    // icon-theme constraint.
    static QIcon icon(const QWidget *forPalette);

    QTabWidget *tabs() const;
    QPlainTextEdit *notes() const;
    QWidget *downloadsPage() const;
    QWidget *bookmarksPage() const;

public slots:
    // AutoSaver target — flushes the notes text and the selected tab.
    void save();
    // DOWN02: Ctrl+Y / Tools > Downloads lands here — the dock is the
    // only downloads surface now.
    void showDownloads();
    // SIDE04: Ctrl+Shift+B / Bookmarks > Show All Bookmarks lands
    // here — the dock is the bookmarks manager surface now.
    // SIDE07: Bookmarks > Show All Bookmarks selects the Bookmarks
    // section the same way Ctrl+Y selects Downloads.
    void showBookmarks();
    // SIDE02: syncs the rail with the persisted per-panel visibility —
    // newly enabled panels build on demand, disabled ones are dropped.
    void applyPanelVisibility();
    // SIDE06: mirrors the icon rail onto the dock's outer edge —
    // left rail when docked left, right rail when docked right.
    void setDockSide(Qt::DockWidgetArea area);
    // SIDE09: re-reads the sidebar/* option keys (panel toggle, close
    // button, folder counters, lazy load) and applies them live —
    // called by BrowserMainWindow::applySidebarSettings, once in the
    // ctor, and on every settings save.
    void applyOptions();

private slots:
    void openBookmark(const QModelIndex &index, TabWidget::OpenUrlIn tab);
    void bookmarksContextMenu(const QPoint &pos);
    void removeSelectedBookmark();
    void updateBookmarkRemoveState();
    void newBookmarkFolder(const QModelIndex &index);
    void openHistoryEntry(const QModelIndex &index, TabWidget::OpenUrlIn tab);
    void historyContextMenu(const QPoint &pos);
    void openDownload(const QModelIndex &index);
    void downloadsContextMenu(const QPoint &pos);
    void downloadSelectionChanged(const QModelIndex &current,
                                  const QModelIndex &previous);
    void applyDownloadSort();
    // DOWN04: the compact per-download action row — pause/resume,
    // retry, remove, show-in-folder and the details (speed graph)
    // toggle all act on the selected list row.
    void updateDownloadActions();
    void pauseSelectedDownload();
    void retrySelectedDownload();
    void removeSelectedDownload();
    void revealSelectedDownload();
    void setSelectedDownloadExpanded(bool expanded);

private:
    static QList<Panel> &registry();

    // SIDE09: selects a rail entry by registered id, materializing a
    // lazy placeholder first if needed (a panel unchecked in
    // Preferences simply isn't there — the dock still raises).
    void showPanel(const QByteArray &id);
    // SIDE09 lazy load: swaps a placeholder page for the real one the
    // first time its tab is selected.  No-op on a built page.
    void buildPendingTab(int index);

    // Registered factories — each builds its page on demand and fills
    // in the matching members (they stay null while the panel is
    // hidden, and QPointer clears them again when it is dropped).
    QWidget *buildBookmarksPage();
    QWidget *buildHistoryPage();
    QWidget *buildDownloadsPage();
    QWidget *buildNotesPage();
    int indexOfPanel(const QByteArray &id) const;
    QModelIndex selectedBookmarkFolder() const;
    // DOWN04: the item behind the list's current row (nullptr when
    // nothing is selected) — the action row's target.
    DownloadItem *selectedDownloadItem() const;

    QTabWidget *m_tabs;
    QPointer<QWidget> m_bookmarksPage;
    QPointer<QPushButton> m_bookmarksRemoveButton;
    QPointer<EditTreeView> m_bookmarksView;
    QPointer<EditTreeView> m_historyView;
    QPointer<QListView> m_downloadsView;
    QPointer<TreeSortFilterProxyModel> m_bookmarksProxy;
    QPointer<TreeSortFilterProxyModel> m_historyProxy;
    QPointer<QSortFilterProxyModel> m_downloadsProxy;
    QPointer<QWidget> m_downloadsPage;
    QPointer<QComboBox> m_downloadsSort;
    QPointer<QFrame> m_downloadDetail;
    // DOWN04: the compact action row under the detail pane — every
    // button's enablement mirrors the hosted card's own logic.
    QPointer<QToolButton> m_downloadPauseButton;
    QPointer<QToolButton> m_downloadRetryButton;
    QPointer<QToolButton> m_downloadRemoveButton;
    QPointer<QToolButton> m_downloadFolderButton;
    QPointer<QToolButton> m_downloadDetailsButton;
    // The DownloadItem card currently hosted in the detail pane.
    QPointer<QWidget> m_detailItem;
    QPointer<QPlainTextEdit> m_notes;
    AutoSaver *m_autoSaver;
    // SIDE09: sidebar/lazyLoad — visible panels sit behind a
    // placeholder page until first selected.
    bool m_lazyLoad;
    // sidebar/folderCounters — consulted by the bookmarks model at
    // paint/fetch time; applyOptions() keeps it in sync.
    static bool s_folderCounters;
};

#endif // SIDEBARPANEL_H
