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

// ENG06 — THE reference Engine::Backend for tests and future ports:
// a pure-Qt, no-web-engine implementation covering the full neutral
// surface (page navigation/history/zoom/find/scripts/lifecycle/pid,
// profile policy+storage, downloads, views).  Every member is a
// plain field so a test can stage exactly the behavior it probes;
// defaults model the "degraded backend" contract the chrome must
// survive (no script gate, no downloads, no per-page process).
//
// Consumers: tst_engineswitch (swap path, registry), tst_engineadapter
// (full-surface parity vs the WebEngine adapter), tst_tabwidget's
// engine legs.

#include <engineinterface.h>

#include <qaction.h>
#include <qhash.h>
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
    void stop() override { ++stops; }
    void reload() override { ++reloads; }
    QUrl url() const override { return m_url; }

    bool canGoBack() const override { return m_canBack; }
    bool canGoForward() const override { return m_canForward; }
    void back() override { m_canForward = true; m_canBack = false; }
    void forward() override { m_canBack = true; m_canForward = false; }

    int historyCount() const override { return m_entries.count(); }
    int currentHistoryIndex() const override { return m_current; }
    QList<Engine::HistoryEntry> historyItems() const override
    {
        return m_entries;
    }
    QList<Engine::HistoryEntry> backItems(int maxItems) const override
    {
        return m_entries.mid(qMax(0, m_current - maxItems),
                             qMin(maxItems, m_current));
    }
    QList<Engine::HistoryEntry> forwardItems(int maxItems) const override
    {
        return m_entries.mid(m_current + 1, maxItems);
    }
    void goToHistoryEntry(const Engine::HistoryEntry &entry) override
    {
        lastGoTo = entry.index;
    }

    void setZoomFactor(qreal factor) override { m_zoom = factor; }
    qreal zoomFactor() const override { return m_zoom; }

    void findText(const QString &subString, Engine::FindFlags options) override
    {
        lastFind = subString;
        lastFindFlags = options;
        Engine::FindResult result;
        result.numberOfMatches = subString.isEmpty() ? 0 : 2;
        result.activeMatch = 1;
        emit findTextFinished(result);
    }

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        lastScript = source;
        if (resultCallback)
            resultCallback(QVariant(QStringLiteral("fake-result")));
    }

    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>(),
                       quint32 worldId = 0) override
    {
        Q_UNUSED(worldId);
        ++liftedCalls;
        lastScript = source;
        if (resultCallback)
            resultCallback(QVariant(QStringLiteral("fake-lifted")));
    }

    void toHtml(const std::function<void(const QString &)> &resultCallback) override
    {
        if (resultCallback)
            resultCallback(m_markup);
    }

    QAction *action(Engine::StandardAction which) override
    {
        return actions.value(which);
    }

    bool isLoading() const override { return m_loading; }
    bool recentlyAudible() const override { return m_audible; }
    bool isOffTheRecord() const override { return m_otr; }

    void setPageAttribute(const QString &name, bool on) override
    {
        attributes.insert(name, on);
    }

    void setLifecycleState(LifecycleState state) override { m_lifecycle = state; }
    LifecycleState lifecycleState() const override { return m_lifecycle; }

    qint64 renderProcessId() const override { return m_pid; }
    void setRenderProcessId(qint64 pid) { m_pid = pid; }

    // ENG26: -1 defers to the base (renderProcessId -> /proc);
    // a staged value models an in-process engine reporting the
    // shared host figure.
    qint64 residentMemoryKb() const override
    {
        return m_residentKb >= 0 ? m_residentKb
                               : Engine::Page::residentMemoryKb();
    }
    bool memoryIsShared() const override { return m_memoryShared; }

    void insertScript(const Engine::Script &script) override
    {
        m_scripts.insert(script.name, script);
    }
    void removeScript(const QString &name) override { m_scripts.remove(name); }
    QList<Engine::Script> scripts() const override { return m_scripts.values(); }

    void download(const QUrl &url) override { lastDownload = url; }
    QWidget *view() const override { return m_view; }

    QUrl m_url;
    qreal m_zoom = 1.0;
    bool m_canBack = false;
    bool m_canForward = false;
    bool m_loading = false;
    bool m_audible = false;
    bool m_otr = false;
    QHash<Engine::StandardAction, QAction*> actions;
    QList<Engine::HistoryEntry> m_entries;
    int m_current = -1;
    int lastGoTo = -1;
    LifecycleState m_lifecycle = LifecycleState::Active;
    qint64 m_pid = -1;
    qint64 m_residentKb = -1;
    bool m_memoryShared = false;
    QString lastFind;
    Engine::FindFlags lastFindFlags;
    QString lastScript;
    int liftedCalls = 0;
    QString m_markup;
    QHash<QString, bool> attributes;
    int loads = 0;
    int stops = 0;
    int reloads = 0;
    QHash<QString, Engine::Script> m_scripts;
    QUrl lastDownload;
    QWidget *m_view = nullptr;
};

class FakeEngineDownloadRequest : public Engine::DownloadRequest
{
    Q_OBJECT

public:
    explicit FakeEngineDownloadRequest(QObject *parent = nullptr)
        : Engine::DownloadRequest(parent) {}

    QUrl url() const override { return m_url; }
    QString suggestedFileName() const override { return m_name; }
    QString mimeType() const override { return m_mime; }
    void accept(const QString &filePath) override
    {
        acceptedPath = filePath;
        setState(State::InProgress);
    }
    void cancel() override { setState(State::Cancelled); }
    void pause() override { ++pauses; m_paused = true; }
    void resume() override { ++resumes; m_paused = false; }
    bool isPaused() const override { return m_paused; }
    qint64 receivedBytes() const override { return m_received; }
    qint64 totalBytes() const override { return m_total; }
    State state() const override { return m_state; }
    bool isFinished() const override
    {
        return m_state == State::Completed || m_state == State::Cancelled;
    }
    QString interruptReasonString() const override { return m_interrupt; }
    void setDownloadDirectory(const QString &directory) override
    {
        m_dir = directory;
    }
    void setDownloadFileName(const QString &fileName) override
    {
        m_fileName = fileName;
    }
    QString downloadDirectory() const override { return m_dir; }
    QString downloadFileName() const override { return m_fileName; }
    Engine::Page *page() const override { return m_page; }

    void setState(State state)
    {
        m_state = state;
        emit stateChanged(state);
    }
    void bump(qint64 received, qint64 total)
    {
        m_received = received;
        emit receivedBytesChanged();
        if (total != m_total) {
            m_total = total;
            emit totalBytesChanged();
        }
    }

    QUrl m_url;
    QString m_name;
    QString m_mime;
    State m_state = State::Requested;
    qint64 m_received = 0;
    qint64 m_total = -1;
    QString m_interrupt;
    QString m_dir;
    QString m_fileName;
    QString acceptedPath;
    int pauses = 0;
    int resumes = 0;
    bool m_paused = false;
    Engine::Page *m_page = nullptr;
};

class FakeEngineProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit FakeEngineProfile(QObject *parent = nullptr)
        : Engine::Profile(parent) {}

    bool isOffTheRecord() const override { return m_otr; }
    QString storageName() const override { return m_name; }
    void setUserAgent(const QString &userAgent) override { m_ua = userAgent; }
    void setRequestPolicy(Engine::RequestPolicy *policy) override { m_policy = policy; }
    Engine::RequestPolicy *requestPolicy() const override { return m_policy; }
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &filter) override
    {
        m_filter = filter;
    }
    void insertScript(const Engine::Script &script) override
    {
        m_scripts.insert(script.name, script);
    }
    void removeScript(const QString &name) override { m_scripts.remove(name); }
    QList<Engine::Script> scripts() const override { return m_scripts.values(); }
    void clear(Engine::StorageAreas areas) override { m_cleared |= areas; }
    void setProfileAttribute(const QString &name, const QVariant &value) override
    {
        m_attributes.insert(name, value);
    }

    bool m_otr = false;
    QString m_name;
    QString m_ua;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_filter;
    QHash<QString, Engine::Script> m_scripts;
    Engine::StorageAreas m_cleared;
    QHash<QString, QVariant> m_attributes;
};

class FakeEngineBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit FakeEngineBackend(QObject *parent = nullptr)
        : Engine::Backend(parent)
    {
        // The degraded contract: scripts evaluate but there is no
        // request hook, no injection, no cookie gate and no download
        // handoff — the same shape the Servo backend reports.
        m_caps.scriptEvaluation = true;
        m_caps.downloads = false;
    }

    QString id() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("Fake Engine"); }
    Engine::Capabilities capabilities() const override { return m_caps; }
    bool initialize() override { return true; }
    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override
    {
        auto *profile = new FakeEngineProfile(parent);
        profile->m_otr = options.offTheRecord;
        profile->m_name = options.storageName;
        return profile;
    }
    Engine::Page *createPage(Engine::Profile *,
                             QObject *parent = nullptr) override
    {
        if (!m_unavailableReason.isEmpty())
            return nullptr;
        auto *page = new FakeEnginePage(parent);
        pages.append(page);
        return page;
    }
    // ENG19: staged unavailability — a set reason makes createPage
    // refuse, mirroring Servo's spent single-view slot.
    QString pageUnavailableReason() const override
    {
        return m_unavailableReason;
    }
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override
    {
        auto *view = new QWidget(parent);
        if (auto *fakePage = qobject_cast<FakeEnginePage*>(page))
            fakePage->m_view = view;
        return view;
    }

    Engine::Capabilities m_caps;
    QList<QPointer<FakeEnginePage>> pages;
    QString m_unavailableReason;
};

#endif // FAKEENGINE_H
