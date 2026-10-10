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
#include <qpointer.h>
#include <qwidget.h>

class AutoSaver;
class DownloadItem;
class EditTreeView;
class QComboBox;
class QFrame;
class QListView;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QTabWidget;
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
    static bool isPanelVisible(const QByteArray &id);
    static void setPanelVisible(const QByteArray &id, bool visible);

    explicit SidebarPanel(QWidget *parent = nullptr);

    // Palette-following toggle glyph — the bundled icon themes ship no
    // sidebar pictogram and new artwork files stay out per the
    // icon-theme constraint.
    static QIcon icon(const QWidget *forPalette);

    QTabWidget *tabs() const;
    QPlainTextEdit *notes() const;
    QWidget *downloadsPage() const;

public slots:
    // AutoSaver target — flushes the notes text and the selected tab.
    void save();
    // DOWN02: Ctrl+Y / Tools > Downloads lands here — the dock is the
    // only downloads surface now.
    void showDownloads();
    // SIDE02: syncs the rail with the persisted per-panel visibility —
    // newly enabled panels build on demand, disabled ones are dropped.
    void applyPanelVisibility();

private slots:
    void openBookmark(const QModelIndex &index, TabWidget::OpenUrlIn tab);
    void bookmarksContextMenu(const QPoint &pos);
    void openHistoryEntry(const QModelIndex &index, TabWidget::OpenUrlIn tab);
    void historyContextMenu(const QPoint &pos);
    void openDownload(const QModelIndex &index);
    void downloadsContextMenu(const QPoint &pos);
    void downloadSelectionChanged(const QModelIndex &current,
                                  const QModelIndex &previous);
    void applyDownloadSort();

private:
    static QList<Panel> &registry();

    // Registered factories — each builds its page on demand and fills
    // in the matching members (they stay null while the panel is
    // hidden, and QPointer clears them again when it is dropped).
    QWidget *buildBookmarksPage();
    QWidget *buildHistoryPage();
    QWidget *buildDownloadsPage();
    QWidget *buildNotesPage();
    int indexOfPanel(const QByteArray &id) const;

    QTabWidget *m_tabs;
    QPointer<EditTreeView> m_bookmarksView;
    QPointer<EditTreeView> m_historyView;
    QPointer<QListView> m_downloadsView;
    QPointer<TreeSortFilterProxyModel> m_bookmarksProxy;
    QPointer<TreeSortFilterProxyModel> m_historyProxy;
    QPointer<QSortFilterProxyModel> m_downloadsProxy;
    QPointer<QWidget> m_downloadsPage;
    QPointer<QComboBox> m_downloadsSort;
    QPointer<QFrame> m_downloadDetail;
    // The DownloadItem card currently hosted in the detail pane.
    QPointer<QWidget> m_detailItem;
    QPointer<QPlainTextEdit> m_notes;
    AutoSaver *m_autoSaver;
};

#endif // SIDEBARPANEL_H
