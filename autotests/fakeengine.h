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

#ifndef FAKEENGINE_H
#define FAKEENGINE_H

// A do-nothing Engine::Backend for tests that need a foreign engine
// without a real servo-embed artifact — the EngineTab swap path,
// the registry and the default-engine setting all exercise it.

#include <engineinterface.h>

#include <qaction.h>
#include <qpointer.h>
#include <qwidget.h>

class FakeEnginePage : public Engine::Page
{
    Q_OBJECT

public:
    explicit FakeEnginePage(QObject *parent = nullptr)
        : Engine::Page(parent) {}

    void load(const QUrl &newUrl) override
    {
        m_url = newUrl;
        ++loads;
        emit urlChanged(newUrl);
        emit loadStarted();
        emit loadProgress(100);
        emit loadFinished(true);
    }
    void stop() override {}
    void reload() override { ++reloads; }
    QUrl url() const override { return m_url; }

    bool canGoBack() const override { return false; }
    bool canGoForward() const override { return false; }
    void back() override {}
    void forward() override {}

    int historyCount() const override { return m_url.isEmpty() ? 0 : 1; }
    int currentHistoryIndex() const override { return m_url.isEmpty() ? -1 : 0; }
    QList<Engine::HistoryEntry> historyItems() const override
    {
        if (m_url.isEmpty())
            return {};
        Engine::HistoryEntry entry;
        entry.url = m_url;
        entry.index = 0;
        return {entry};
    }
    QList<Engine::HistoryEntry> backItems(int) const override { return {}; }
    QList<Engine::HistoryEntry> forwardItems(int) const override { return {}; }
    void goToHistoryEntry(const Engine::HistoryEntry &) override {}

    void setZoomFactor(qreal factor) override { m_zoom = factor; }
    qreal zoomFactor() const override { return m_zoom; }

    void findText(const QString &, Engine::FindFlags) override
    {
        emit findTextFinished(Engine::FindResult());
    }

    void runJavaScript(const QString &,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        if (resultCallback)
            resultCallback(QVariant());
    }
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        runJavaScript(source, resultCallback);
    }
    void toHtml(const std::function<void(const QString &)> &resultCallback) override
    {
        if (resultCallback)
            resultCallback(QString());
    }

    QAction *action(Engine::StandardAction) override { return nullptr; }

    bool isLoading() const override { return false; }
    bool recentlyAudible() const override { return false; }
    bool isOffTheRecord() const override { return false; }

    void setPageAttribute(const QString &, bool) override {}
    void setLifecycleState(LifecycleState state) override { m_lifecycle = state; }
    LifecycleState lifecycleState() const override { return m_lifecycle; }
    qint64 renderProcessId() const override { return -1; }
    Engine::Page *createWindow(Engine::WebWindowType) override { return nullptr; }

    void insertScript(const Engine::Script &) override {}
    void removeScript(const QString &) override {}
    QList<Engine::Script> scripts() const override { return {}; }

    void download(const QUrl &) override {}
    QWidget *view() const override { return m_view; }

    QUrl m_url;
    qreal m_zoom = 1.0;
    int loads = 0;
    int reloads = 0;
    LifecycleState m_lifecycle = LifecycleState::Active;
    QWidget *m_view = nullptr;
};

class FakeEngineProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit FakeEngineProfile(QObject *parent = nullptr)
        : Engine::Profile(parent) {}

    bool isOffTheRecord() const override { return false; }
    QString storageName() const override { return QStringLiteral("fake"); }
    void setUserAgent(const QString &) override {}
    void setRequestPolicy(Engine::RequestPolicy *) override {}
    Engine::RequestPolicy *requestPolicy() const override { return nullptr; }
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &) override {}
    void insertScript(const Engine::Script &) override {}
    void removeScript(const QString &) override {}
    QList<Engine::Script> scripts() const override { return {}; }
    void clear(Engine::StorageAreas) override {}
    void setProfileAttribute(const QString &, const QVariant &) override {}
};

class FakeEngineBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit FakeEngineBackend(QObject *parent = nullptr)
        : Engine::Backend(parent) {}

    QString id() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("Fake Engine"); }
    Engine::Capabilities capabilities() const override
    {
        Engine::Capabilities caps;
        caps.downloads = false;
        return caps;
    }
    bool initialize() override { return true; }
    Engine::Profile *createProfile(const Engine::ProfileOptions &,
                                   QObject *parent = nullptr) override
    {
        return new FakeEngineProfile(parent);
    }
    Engine::Page *createPage(Engine::Profile *,
                             QObject *parent = nullptr) override
    {
        auto *page = new FakeEnginePage(parent);
        pages.append(page);
        return page;
    }
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override
    {
        auto *view = new QWidget(parent);
        if (auto *fakePage = qobject_cast<FakeEnginePage*>(page))
            fakePage->m_view = view;
        return view;
    }

    QList<QPointer<FakeEnginePage>> pages;
};

#endif // FAKEENGINE_H
