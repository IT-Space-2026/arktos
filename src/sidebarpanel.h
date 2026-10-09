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
    QTabWidget *m_tabs;
    EditTreeView *m_bookmarksView;
    EditTreeView *m_historyView;
    QListView *m_downloadsView;
    TreeSortFilterProxyModel *m_bookmarksProxy;
    TreeSortFilterProxyModel *m_historyProxy;
    QSortFilterProxyModel *m_downloadsProxy;
    QWidget *m_downloadsPage;
    QComboBox *m_downloadsSort;
    QFrame *m_downloadDetail;
    // The DownloadItem card currently hosted in the detail pane.
    QPointer<QWidget> m_detailItem;
    QPlainTextEdit *m_notes;
    AutoSaver *m_autoSaver;
};

#endif // SIDEBARPANEL_H
