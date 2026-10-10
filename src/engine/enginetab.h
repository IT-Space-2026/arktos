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

#ifndef ENGINETAB_H
#define ENGINETAB_H

// ENG05 — a tab-page host for non-WebEngine backends.
//
// TabWidget's strip expects a WebViewWithSearch child for browsing
// tabs; an EngineTab is the alternate-content equivalent: it owns an
// Engine::Page created through the backend plus the backend's painted
// view widget, and re-emits the page signals the tab chrome consumes
// (title/url/load state/icon).  Everything the page can't do on a
// degraded backend (context menus, find-in-page, downloads, script
// bridges) simply never fires — the same honest shape the PREFS01
// widget tabs use, since every TabWidget dereference site is already
// null-safe on webView().

#include <qpointer.h>
#include <qwidget.h>

namespace Engine {
class Backend;
class Page;
class Profile;
}

class EngineTab : public QWidget
{
    Q_OBJECT

public:
    EngineTab(Engine::Backend *backend, Engine::Profile *profile,
              QWidget *parent = nullptr);

    Engine::Backend *backend() const { return m_backend; }
    Engine::Page *page() const { return m_page; }

    QUrl url() const;
    QString title() const;
    int progress() const { return m_progress; }

    void load(const QUrl &url);
    void stop();
    void reload();

signals:
    void titleChanged(const QString &title);
    void urlChanged(const QUrl &url);
    void iconChanged();
    void loadStarted();
    void loadProgress(int progress);
    void loadFinished(bool ok);

private:
    Engine::Backend *m_backend;
    Engine::Page *m_page;
    QString m_title;
    int m_progress;
};

#endif // ENGINETAB_H
