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

#include "devtoolswindow.h"

#include <qevent.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

DevToolsWindow::DevToolsWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_view(new QWebEngineView(this))
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setCentralWidget(m_view);
    updateTitle();
    resize(900, 640);
}

// One shared host window — binding is per-page so a single inspector
// serves every window; each inspectElement() re-points it.
static QPointer<DevToolsWindow> s_window;

DevToolsWindow *DevToolsWindow::instance()
{
    return s_window;
}

void DevToolsWindow::inspectElement(QWebEnginePage *page)
{
    if (!page)
        return;

    if (!s_window)
        s_window = new DevToolsWindow;
    DevToolsWindow *window = s_window;

    // The DevTools page must live on the inspected page's own profile —
    // private and Tor windows therefore get their own.  The view's
    // initial page is replaced on first use (and whenever the profile
    // differs); the old page is a view child and dies with setPage().
    QWebEnginePage *devToolsPage = window->m_view->page();
    if (!devToolsPage || devToolsPage->profile() != page->profile()) {
        devToolsPage = new QWebEnginePage(page->profile(), window->m_view);
        window->m_view->setPage(devToolsPage);
    }
    // DevTools is itself a web application — the Safest security tier
    // may have switched JavascriptEnabled off profile-wide, but the
    // inspector must render regardless.
    devToolsPage->settings()->setAttribute(
            QWebEngineSettings::JavascriptEnabled, true);

    if (window->m_inspectedPage && window->m_inspectedPage != page)
        window->m_inspectedPage->setDevToolsPage(nullptr);
    page->setDevToolsPage(devToolsPage);
    window->m_inspectedPage = page;
    connect(page, &QObject::destroyed,
            window, &QWidget::close, Qt::UniqueConnection);
    connect(page, &QWebEnginePage::titleChanged,
            window, &DevToolsWindow::updateTitle, Qt::UniqueConnection);

    window->updateTitle();
    window->show();
    window->raise();
    window->activateWindow();
    page->triggerAction(QWebEnginePage::InspectElement);
}

void DevToolsWindow::closeEvent(QCloseEvent *event)
{
    if (m_inspectedPage) {
        disconnect(m_inspectedPage, nullptr, this, nullptr);
        m_inspectedPage->setDevToolsPage(nullptr);
        m_inspectedPage = nullptr;
    }
    QMainWindow::closeEvent(event);
}

void DevToolsWindow::updateTitle()
{
    const QString title = m_inspectedPage ? m_inspectedPage->title() : QString();
    setWindowTitle(title.isEmpty()
                   ? tr("Web Inspector")
                   : tr("Web Inspector — %1").arg(title));
}
