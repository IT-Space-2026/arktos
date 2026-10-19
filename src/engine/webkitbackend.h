/*
 * Copyright 2026 The Arktos Authors
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

#ifndef WEBKITBACKEND_H
#define WEBKITBACKEND_H

// WK06/WK07 — the WPE WebKit backend.
//
// The class is in the build whenever engine.pri resolves the webkit
// CONFIG — mirroring servobackend's compile-but-inert contract rather
// than conditional source exclusion: a `qmake webkit=1` build on a
// host whose pkg-config cannot resolve wpe-webkit-2.0 +
// wpe-platform-2.0 still compiles this TU, and the backend simply
// reports the engine unavailable (librariesLinked() == false,
// initialize() returns false so the registration probe drops it,
// pageUnavailableReason() explains) — the no-libs honesty half of
// the gate.
//
// ARKTOS_WEBKIT_LINKED marks the builds where the packages did
// resolve: the TU then carries real libWPEWebKit references (the
// version probe keeps the DT_NEEDED honest) and every binary linking
// it carries the ~/wpe sideloaded-glibc recipe from
// .devin/webkit-env.sh — sid ld.so interp + transitive DT_RPATH +
// the g_sort_array compat shim.
//
// WK07 landed the registry probe (static-init EngineRegistry::addProbe,
// the servo shape — no engineregistry.cpp edit), the "WebKit x.y.z"
// display name off webkit_get_*_version(), and the conservative
// capabilities table (.devin/WEBKIT-SPIKE.md — only what WK01-WK04
// proved or the 2.54 headers pin down).  The WebkitProfile/
// WebkitPage/WebkitView skeletons below compile but stay unreachable:
// a linked backend registers yet cannot host a page until WK08, so
// createPage()/createView() refuse and the switcher offers the entry
// disabled-with-reason (the ENG19 pageUnavailableReason path) rather
// than a guaranteed-dead swap.

#include "engineinterface.h"

#include <qhash.h>
#include <qpointer.h>
#include <qurl.h>
#include <qvariant.h>
#include <qwidget.h>

class WebkitView;

// WK07 skeleton — the widget that will present a WebkitPage once
// WK08 wires the real WPEView + "buffer-rendered" raster path (the
// WK03 harness code, productized: wpe_buffer_import_to_pixels ->
// QImage::Format_ARGB32_Premultiplied -> paintEvent blit).  The
// backend never hands one out while pages are unavailable.
class WebkitView : public QWidget
{
    Q_OBJECT

public:
    explicit WebkitView(QWidget *parent = nullptr);
};

// WK07 skeleton — an honest inert Engine::Page.  The WebKitWebView +
// WPEView it wraps land in WK08 (created on the WK02-decided
// GUI-thread QPAEventDispatcherGlib model — never a worker GMainLoop).
// Until then nothing here reaches an engine: members are recorded
// no-ops and the page's signals simply never fire.
class WebkitPage : public Engine::Page
{
    Q_OBJECT

public:
    explicit WebkitPage(Engine::Profile *profile,
                        QObject *parent = nullptr);

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

    void findText(const QString &subString,
                  Engine::FindFlags options) override;

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>(),
                       quint32 worldId = 0) override;
    void toHtml(const std::function<void(const QString &)> &resultCallback) override;

    QAction *action(Engine::StandardAction action) override;

    bool isLoading() const override;
    bool recentlyAudible() const override;
    bool isOffTheRecord() const override;

    void setPageAttribute(const QString &name, bool on) override;
    void setLifecycleState(LifecycleState state) override;
    LifecycleState lifecycleState() const override;
    qint64 renderProcessId() const override;

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void download(const QUrl &url) override;
    QWidget *view() const override;

private:
    Engine::Profile *m_profile;
    QPointer<WebkitView> m_view;
    QUrl m_url;
    qreal m_zoom = 1.0;
    LifecycleState m_lifecycle = LifecycleState::Active;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, bool> m_attributes;
};

// WK07 skeleton — carries the ProfileOptions and the interface's
// per-profile bags (policy, cookie filter, scripts, attributes).
// WK11 roots a real WebKitNetworkSession under the profile's storage
// tree; until then this is the honest in-memory shape (the same
// contract ServoProfile serves).
class WebkitProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit WebkitProfile(const Engine::ProfileOptions &options,
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

    const Engine::ProfileOptions &options() const { return m_options; }

private:
    Engine::ProfileOptions m_options;
    QString m_userAgent;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_cookieFilter;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, QVariant> m_attributes;
};

class WebkitBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit WebkitBackend(QObject *parent = nullptr);

    // Configure-time pkg-config resolution — false on a webkit=1
    // build where the WPE packages were absent (the inert half of
    // compile-but-inert).
    static bool librariesLinked();
    // "x.y.z" from webkit_get_*_version — empty when not linked.
    static QString wpeVersion();

    QString id() const override;
    QString displayName() const override;
    QString engineNote() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QString pageUnavailableReason() const override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;
};

#endif // WEBKITBACKEND_H
