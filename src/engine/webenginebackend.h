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

#ifndef WEBENGINEBACKEND_H
#define WEBENGINEBACKEND_H

// ENG04 — the QtWebEngine backend for the Engine interface.
//
// Each adapter is a thin QObject wrapper: every Engine virtual
// forwards to the wrapped QWebEngine object and the engine's own
// signals are re-emitted translated onto the interface's engine-
// neutral types.  Nothing here adds policy — policy lives above the
// boundary (the app's interceptors, re-expressed as Engine::
// RequestPolicy, are a later touchpoint group).
//
// webEnginePage()/webEngineProfile() are the migration escape hatch:
// call sites still on engine types can reach the wrapped object while
// the rest of the tree moves to the interface.

#include "engineinterface.h"

#include <qhash.h>

class QWebEngineDownloadRequest;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineUrlRequestInterceptor;

class WebEnginePageAdapter : public Engine::Page
{
    Q_OBJECT

public:
    explicit WebEnginePageAdapter(QWebEnginePage *page,
                                  QObject *parent = nullptr);

    // The wrapped engine object — the escape hatch for call sites not
    // yet on the interface.
    QWebEnginePage *webEnginePage() const;
    // Escape-hatch downcast — nullptr on a foreign backend's adapter.
    static WebEnginePageAdapter *of(Engine::Page *page);
    // The canonical adapter for an engine page: the one already
    // parented to it (WebView::enginePage), or a fresh one parented
    // to the page so every caller sees the same Engine::Page object —
    // required wherever identity is compared (the download surface's
    // hasActiveDownloadForPage).
    static WebEnginePageAdapter *forPage(QWebEnginePage *page);

    void load(const QUrl &url) override;
    void stop() override;
    void reload() override;
    QUrl url() const override;

    bool canGoBack() const override;
    bool canGoForward() const override;
    void back() override;
    void forward() override;

    int historyCount() const override;
    int currentHistoryIndex() const override;
    QList<Engine::HistoryEntry> historyItems() const override;
    QList<Engine::HistoryEntry> backItems(int maxItems) const override;
    QList<Engine::HistoryEntry> forwardItems(int maxItems) const override;
    void goToHistoryEntry(const Engine::HistoryEntry &entry) override;

    void setZoomFactor(qreal factor) override;
    qreal zoomFactor() const override;
    QPointF scrollPosition() const override;

    void findText(const QString &subString, Engine::FindFlags options) override;

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;

    void toHtml(
            const std::function<void(const QString &)> &resultCallback) override;

    QAction *action(Engine::StandardAction action) override;

    bool isLoading() const override;
    bool recentlyAudible() const override;
    bool isOffTheRecord() const override;

    void setPageAttribute(const QString &name, bool on) override;

    void setLifecycleState(LifecycleState state) override;
    LifecycleState lifecycleState() const override;

    qint64 renderProcessId() const override;

    Engine::Page *createWindow(Engine::WebWindowType type) override;
    // Window requests keep routing through the app's own chain
    // (WebPage::createWindow) — a page owner that wants the created
    // window back as an adapted page installs this handler.
    void setCreateWindowHandler(
            const std::function<Engine::Page *(Engine::WebWindowType)> &handler);

    void runJavaScriptLifted(const QString &source,
            const std::function<void(const QVariant &)> &resultCallback
                = std::function<void(const QVariant &)>()) override;
    // The same lift on a raw engine page — escape hatch for callers
    // still holding QWebEnginePage*/QWebEngineView* (the
    // clear-private-data sweep walks views, not WebView tabs).
    static void runJavaScriptLiftedOn(QWebEnginePage *page,
            const QString &source,
            const std::function<void(const QVariant &)> &resultCallback
                = std::function<void(const QVariant &)>());

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void download(const QUrl &url) override;
    QWidget *view() const override;

private:
    QWebEnginePage *m_page;
    std::function<Engine::Page *(Engine::WebWindowType)> m_createWindow;
};

class WebEngineDownloadRequest : public Engine::DownloadRequest
{
    Q_OBJECT

public:
    explicit WebEngineDownloadRequest(QWebEngineDownloadRequest *request,
                                      QObject *parent = nullptr);

    QUrl url() const override;
    QString suggestedFileName() const override;
    QString mimeType() const override;
    void accept(const QString &filePath) override;
    void cancel() override;
    void pause() override;
    void resume() override;
    qint64 receivedBytes() const override;
    qint64 totalBytes() const override;
    State state() const override;
    bool isFinished() const override;
    QString interruptReasonString() const override;
    void setDownloadDirectory(const QString &directory) override;
    void setDownloadFileName(const QString &fileName) override;
    QString downloadDirectory() const override;
    QString downloadFileName() const override;
    Engine::Page *page() const override;

private:
    QWebEngineDownloadRequest *m_request;
};

class WebEngineProfileAdapter : public Engine::Profile
{
    Q_OBJECT

public:
    // Never owns the profile — Qt parentage decides its lifetime
    // (createProfile() parents a fresh profile to the adapter).
    explicit WebEngineProfileAdapter(QWebEngineProfile *profile,
                                     QObject *parent = nullptr);

    QWebEngineProfile *webEngineProfile() const;
    // Escape-hatch downcast — nullptr on a foreign backend's adapter.
    static WebEngineProfileAdapter *of(Engine::Profile *profile);
    // The canonical adapter for an engine profile (same identity rule
    // as forPage): reuse the one already parented to the profile or
    // create one parented to it.
    static WebEngineProfileAdapter *forProfile(QWebEngineProfile *profile);

    bool isOffTheRecord() const override;
    QString storageName() const override;
    void setUserAgent(const QString &userAgent) override;

    void setRequestPolicy(Engine::RequestPolicy *policy) override;
    Engine::RequestPolicy *requestPolicy() const override;

    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &filter) override;

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void clear(Engine::StorageAreas areas) override;

    void setProfileAttribute(const QString &name, const QVariant &value) override;

private:
    QWebEngineProfile *m_profile;
    Engine::RequestPolicy *m_policy = nullptr;
    QWebEngineUrlRequestInterceptor *m_policyInterceptor = nullptr;
    QHash<QString, QVariant> m_attributes;
};

class WebEngineBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit WebEngineBackend(QObject *parent = nullptr);

    // The process-wide backend — WebEngine is the only backend until
    // a second one lands (ENG05); the chooser goes here.
    static WebEngineBackend *instance();

    QString id() const override;
    QString displayName() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;
};

#endif // WEBENGINEBACKEND_H
