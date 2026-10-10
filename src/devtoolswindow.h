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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef DEVTOOLSWINDOW_H
#define DEVTOOLSWINDOW_H

#include <qmainwindow.h>
#include <qpointer.h>
#include <qwebenginepage.h>

class QWebEngineView;

// Qt WebEngine has no built-in inspector window (Qt WebKit did):
// QWebEnginePage::InspectElement renders Chromium DevTools into the
// page bound via setDevToolsPage(), and silently does nothing until
// one is.  This top-level window is the dedicated host for that
// DevTools page — View > Development Tools > Chromium Dev Tool and
// the context menu's "Inspect Element" both route through
// inspectElement().
class DevToolsWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Binds the shared DevTools view to page (creating the host page on
    // page's own QWebEngineProfile), shows the window and triggers the
    // InspectElement action.  Re-invoking on a different page — a tab
    // switch — re-points the binding to the current page.  A nullptr
    // page is a no-op.
    static void inspectElement(QWebEnginePage *page);

    // The live inspector window, or nullptr when it has never been
    // opened (or has been closed again — the window deletes on close).
    static DevToolsWindow *instance();

    QWebEngineView *view() const { return m_view; }
    QWebEnginePage *inspectedPage() const { return m_inspectedPage; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    explicit DevToolsWindow(QWidget *parent = nullptr);
    void updateTitle();

    QWebEngineView *m_view;
    QPointer<QWebEnginePage> m_inspectedPage;
};

#endif // DEVTOOLSWINDOW_H
