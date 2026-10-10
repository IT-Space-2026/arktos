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

#ifndef SERVOBACKEND_H
#define SERVOBACKEND_H

// ENG05 — the Servo backend for the Engine interface.
//
// Compiled only under `qmake servo=1` (ARORA_SERVO) and useful only
// when the servo-embed cdylib resolves at runtime — the artifact is a
// ~1.1GB out-of-tree build (spikes/servo/servo-embed, pinned libservo
// v0.7.0 family per the ENG03 verdict) that cannot be rebuilt inside
// a task-run budget, so the backend dlopens it and reports itself
// unavailable when it is absent; chrome hides the dead option rather
// than shipping a guaranteed-fail menu entry.
//
// Honest capability surface per .devin/SERVO-SPIKE.md: no request
// interception enforcement (loads are observable only), no SOCKS5
// (Tor windows stay Chromium — the swap path refuses), no per-webview
// settings, no find-in-page, no download delegate, no cert-error
// delegate, one-way JS eval.  Everything the backend cannot do is
// reported through capabilities()/no-op overrides, never faked.
//
// Rendering is the spike's software path — se_frame_copy blits an
// RGBA image into a QImage on frame_ready.  The C ABI exposes no GL
// surface handoff, so the QOpenGLWidget/surfman upgrade needs a shim
// extension first (noted in the spike doc).

#include "engineinterface.h"

#include <qhash.h>
#include <qimage.h>
#include <qpointer.h>
#include <qurl.h>
#include <qwidget.h>

class ServoPage;

// QWidget hosting one libservo WebView through the servo-embed C ABI
// (the ENG03 spike harness's ServoView, grown signal coverage for the
// Engine::Page contract).  Owns the SeInstance: painting, input
// forwarding and the event-loop wake marshaling all live here.
class ServoView : public QWidget
{
    Q_OBJECT

public:
    explicit ServoView(QWidget *parent = nullptr);
    ~ServoView() override;

    bool isReady() const;
    void load(const QUrl &url);
    void spinOnce();              // one se_spin — GUI thread
    void goBack();
    void setZoom(qreal zoom);
    void evaluateJavaScript(const QString &source);

    QUrl url() const { return m_url; }
    QString title() const { return m_title; }
    bool isLoading() const { return m_loading; }

signals:
    void urlChanged(const QUrl &url);
    void titleChanged(const QString &title);
    void loadStarted();
    void loadProgress(int progress);
    void loadFinished(bool ok);
    void statusChanged(const QString &status);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void grabFrame();
    void setUrl(const QUrl &url);
    void setTitle(const QString &title);
    void onLoadStatus(int status);

    // SeCallbacks trampolines — static members so the C ABI reaches
    // private state without friend pollution.
    static void cbWake(void *userdata);
    static void cbFrameReady(void *userdata);
    static void cbUrlChanged(void *userdata, const char *url);
    static void cbTitleChanged(void *userdata, const char *title);
    static void cbLoadStatus(void *userdata, int status);
    static void cbConsole(void *userdata, int level, const char *message);
    static void cbNavigation(void *userdata, const char *url);
    static void cbResourceLoad(void *userdata, const char *request);

    void *m_instance;
    QImage m_image;
    QUrl m_url;
    QString m_title;
    bool m_loading;

    friend class ServoPage;
};

class ServoPage : public Engine::Page
{
    Q_OBJECT

public:
    explicit ServoPage(Engine::Profile *profile, QObject *parent = nullptr);

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

    void findText(const QString &subString, Engine::FindFlags options) override;

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;
    void toHtml(const std::function<void(const QString &)> &resultCallback) override;

    QAction *action(Engine::StandardAction action) override;

    bool isLoading() const override;
    bool recentlyAudible() const override;
    bool isOffTheRecord() const override;

    void setPageAttribute(const QString &name, bool on) override;
    void setLifecycleState(LifecycleState state) override;
    LifecycleState lifecycleState() const override;
    qint64 renderProcessId() const override;
    Engine::Page *createWindow(Engine::WebWindowType type) override;

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void download(const QUrl &url) override;
    QWidget *view() const override;

    // The backend binds the widget the page renders into — loads
    // issued before it lands queue behind the attach.
    void attachView(ServoView *view);

private:
    void recordNavigation(const QUrl &url);

    QPointer<ServoView> m_view;
    QUrl m_pendingUrl;
    QList<Engine::HistoryEntry> m_entries;
    int m_current;
    qreal m_zoom;
    bool m_loading;
    LifecycleState m_lifecycle;
    Engine::Profile *m_profile;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, bool> m_attributes;
};

class ServoProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit ServoProfile(const QString &storageName,
                          QObject *parent = nullptr);

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
    QString m_storageName;
    QString m_userAgent;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_cookieFilter;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, QVariant> m_attributes;
};

class ServoBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit ServoBackend(QObject *parent = nullptr);

    // The process backend, or nullptr when the servo-embed artifact
    // is absent/unresolvable — callers must treat null as "Servo is
    // not installed" and hide the choice.
    static ServoBackend *instance();
    // Filesystem+symbol resolution only; does not start servo.
    static bool artifactAvailable();
    static QString artifactPath();

    QString id() const override;
    QString displayName() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;

    // The servo config dir shared by every webview on a profile —
    // under the app's data path so cookie/storage state lands where
    // the rest of the profile lives.
    static QString configDir();

private:
    QPointer<ServoProfile> m_defaultProfile;
};

#endif // SERVOBACKEND_H
