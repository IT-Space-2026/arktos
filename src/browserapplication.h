/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#ifndef BROWSERAPPLICATION_H
#define BROWSERAPPLICATION_H

#include "singleapplication.h"

#include <qpointer.h>
#include <qscopedpointer.h>
#include <qurl.h>
#include <qdatetime.h>

class AutoFillManager;
class BookmarksManager;
class BrowserMainWindow;
class CookieJar;
class DownloadManager;
class HistoryManager;
class NetworkAccessManager;
class LanguageManager;
class QLocalSocket;
class QTemporaryDir;
class TorManager;
class QWebEnginePage;
class QWebEngineProfile;
class BrowserApplication : public SingleApplication
{
    Q_OBJECT

public:
    BrowserApplication(int &argc, char **argv);
    ~BrowserApplication();
    static BrowserApplication *instance();
    // A run with any --option argument (smoke tests, --quit-after-load,
    // --help, ...) is standalone: it does not take part in the
    // single-instance handshake and always runs locally.
    bool isStandalone() const;
    void loadSettings();

    BrowserMainWindow *mainWindow();
    QList<BrowserMainWindow*> mainWindows();
    bool allowToCloseWindow(BrowserMainWindow *window);

    static QIcon icon(const QUrl &url);

    void saveSession();
    // SESS02: termination-signal entry point — flushes every window's
    // pending AutoSaver debounce plus a forced session write, after
    // which the caller re-raises the signal.  See
    // TerminationSignalHandler.  signalNumber lands in the
    // browser/lastExit breadcrumb.
    void flushSessionsForTermination(int signalNumber = 0);
    // SESS02: never leave a real browsing session windowless — a
    // session restore that produced zero windows (empty/corrupt blob,
    // a declined crash-guard prompt) or a window lost before
    // postLaunch() ran falls back to a normal window on the start
    // page; a live window whose tab strip came up empty gains a
    // new-tab page.  Returns true when a window had to be created.
    bool ensureSessionWindow();
    bool canRestoreSession() const;

    static HistoryManager *historyManager();
    static CookieJar *cookieJar();
    // The profile pages are created on: the ambient persona's named
    // profile normally, an off-the-record profile while private
    // browsing is enabled (MIG03).
    static QWebEngineProfile *webEngineProfile();
    // MPROF03: the persona-aware spelling — resolves the given profile
    // id through BrowserProfile::profileFor() and attaches the app
    // services via prepareProfile().  Mode profiles still win
    // outright: a tor process takes the dedicated tor profile and a
    // private process the shared OTR overlay regardless of the id —
    // OTR is memory-only, so one unnamed profile safely serves every
    // persona (documented decision: no per-id OTR clones; the app-side
    // stores have their own isOffTheRecord gates).
    static QWebEngineProfile *webEngineProfileFor(const QString &profileId);
    // PTAB01: the prepared off-the-record profile regardless of the
    // global private flag — a private TAB inside a normal window needs
    // it while webEngineProfile() still hands out the normal profile.
    static QWebEngineProfile *privateWebEngineProfile();
    // Attaches the app-level services to a profile the first time it
    // is seen: cookie jar, custom scheme handlers, persisted settings,
    // download manager, the adblock + privacy request interceptors and
    // extensions (with the tor-mode hardening pins instead in a tor
    // process).  Idempotent — webEngineProfile() runs it for the
    // normal/private/tor profiles, ContainerManager::profileFor() for
    // container profiles.
    static void prepareProfile(QWebEngineProfile *profile);
    static DownloadManager *downloadManager();
    static NetworkAccessManager *networkAccessManager();
    static BookmarksManager *bookmarksManager();
    static LanguageManager *languageManager();
    static AutoFillManager *autoFillManager();

    static QString installedDataDirectory();
    static QString dataFilePath(const QString &fileName);

    Qt::MouseButtons eventMouseButtons() const;
    Qt::KeyboardModifiers eventKeyboardModifiers() const;
    void setEventMouseButtons(Qt::MouseButtons buttons);
    void setEventKeyboardModifiers(Qt::KeyboardModifiers modifiers);

    static bool zoomTextOnly();

    static bool isPrivate();
    static void setPrivate(bool isPrivate);

    // TOR02: `arora --tor` runs the whole process on a dedicated
    // off-the-record profile behind a managed tor daemon.  The
    // application proxy is process-global — that is why Tor is a
    // separate process rather than a window mode in this one.
    static bool isTorMode();
    // Only meaningful before the constructor brings the profile up;
    // exists for AUTOTESTS builds where the argv scan is compiled out.
    static void setTorMode(bool torMode);
    // TOR09: the bundled start page url — the tor-branded variant in
    // a tor process, the standard one elsewhere.  Only the DEFAULT
    // home (about:home) resolves through here; a custom homepage
    // setting is always loaded as configured.  The no-arg pick uses
    // the ambient context — the default engine id and the app
    // privacy flag.
    static QUrl startPageUrl();
    // ENG13/PTAB02: the fully keyed pick — precedence
    // tor > private > servo > standard.  Tor still wins over every
    // other dimension: it is locked to Chromium (servo has no
    // SOCKS5) and a tor process never hosts clearnet private tabs,
    // so no flag may rebrand a tor page.  Private outranks servo —
    // a private tab must show SOMETHING branded even while the
    // servo backend cannot render qrc (ENG18 covers that).
    static QUrl startPageUrl(const QString &engineId,
                             bool privateContext = false);
    // PREF04: General-page Homepage + Startup modes — the General page
    // radios, the Home button, postLaunch, fileNew and torStartup all
    // read through these so the persisted keys and their migration
    // stay in one place.  Keys: MainWindow/homePageMode,
    // MainWindow/startupMode, MainWindow/startupPages.  The retired
    // MainWindow/startupBehavior int (0 home / 1 blank / 2 restore) is
    // folded in by startupMode() when startupMode was never written.
    enum HomePageMode {
        StartPageHome = 0,
        SpecificPageHome
    };
    enum StartupMode {
        LastSessionStartup = 0,
        HomepageStartup,
        StartPageStartup,
        SpecificPagesStartup
    };
    static int homePageMode();
    static int startupMode();
    static QStringList startupPages();
    // Opens the Specific Pages list into `window` — first entry in the
    // current tab, the rest as background tabs; an empty list leaves
    // the window's fresh tab alone.
    static void openStartupPages(BrowserMainWindow *window);
    // ENG13: the engine a new tab would run — ENG05's
    // MainWindow/defaultEngine key ("webengine" when unset).  This
    // branch has no EngineRegistry yet, so a non-webengine pick only
    // reaches the start-page branding; the merged registry supplies
    // the real per-tab backend through the same key.
    static QString defaultEngineId();
    // The managed daemon — nullptr unless tor mode is on.
    TorManager *torManager() const;
#ifdef AUTOTESTS
    // Test seam: only the --tor constructor path builds a manager —
    // the autotests inject one (test-owned; it is NOT reparented) so
    // the bootstrap navigation gate in WebView/WebPage has a daemon
    // to watch.
    void setTorManagerForTesting(TorManager *manager)
    {
        m_torManager = manager;
    }
#endif
    // Spawns an independent `arora --tor` process.  An optional start
    // url (CONT07's 'Open in New Tor Window') rides as its own argv
    // element — never a shell string — and is re-gated as untrusted
    // input on the receiving side (torStartup).
    static void openTorWindow(const QUrl &url = QUrl());
    // CONT08: the argument vector openTorWindow() spawns with — a
    // QStringList, so the url is a verbatim argv element no shell can
    // ever re-parse, and a refused scheme leaves this process as
    // nothing but the bare --tor switch.  Exposed for the
    // security-audit tests.
    static QStringList torWindowArguments(const QUrl &url);
    // The receiving-side gate torStartup() applies to an argv url
    // operand — the same page-link discipline the sender enforced.
    static bool isUrlAllowedOnTorArgv(const QString &operand);

    // TOR17: false from the moment the managed daemon's SOCKS endpoint
    // is applied to QNetworkProxy until a probe request has made the
    // WebEngine network service re-read it — Chromium only re-fetches
    // the config from a lazy poll inside proxy resolution, and the
    // request that triggers the poll still resolves on the STALE
    // endpoint (the ERR_PROXY_CONNECTION_FAILED the first post-ready
    // navigation used to die on).  True outside tor mode and before
    // the first endpoint discovery.  WebView::torMustQueue reads it.
    bool isTorProxyWarmed() const;

    // TRUST06: master switch for the site-trust surface (Preferences
    // > Privacy > Site Trust).  The toolbar badge and every
    // per-signal lookup gate on this — consumers read it live so a
    // Preferences flip applies without a restart.  Default on.
    static bool siteTrustEnabled();

    // TRUST02: per-signal gates inside the same group — the
    // lookalike check and the threat-list probe each read their key
    // live so a Preferences flip applies on the next navigation.
    // Both default on.
    static bool siteTrustLookalikeEnabled();
    static bool siteTrustThreatlistEnabled();

#if defined(Q_OS_MACOS)
    bool event(QEvent *event);
#endif

    // CONT02: like newMainWindow(), but the window's first tab is
    // bound to the given container's profile — a window spawned from
    // a container tab (window.open, ctrl+alt+click) stays in the
    // opener's container.  The default id, private browsing and tor
    // mode just produce a normal window.  MPROF03: profileId binds the
    // window's persona (the opener's); an empty id inherits the
    // ambient persona like newMainWindow().
    BrowserMainWindow *newMainWindowInContainer(const QString &containerId,
                                                const QString &profileId = QString());

public slots:
    // MPROF03: profileId binds the window to a registry persona —
    // every WebView it creates browses on that persona's named
    // QWebEngineProfile and its app-side stores follow.  An empty id
    // inherits the ambient persona (the focused window's — a File>New
    // Window stays on the opener's profile); an id the registry does
    // not carry falls back to "default".
    BrowserMainWindow *newMainWindow(const QString &profileId = QString());
    bool restoreLastSession();
#if defined(Q_OS_MACOS)
    void lastWindowClosed();
#endif
    void quitBrowser();

    static void setZoomTextOnly(bool textOnly);

    void askDesktopToOpenUrl(const QUrl &url);

private slots:
    void retranslate();
    void messageReceived(QLocalSocket *socket);
    void postLaunch();
    void openUrl(const QUrl &url);
    // SESS02: BrowserMainWindow::destroyed hook — see implementation.
    // VSEC01: carries the closing window's persona (captured at
    // construction) — auto-lock must fire on the profile that lost a
    // window, not the ambient ProfileStore::activeProfileId().
    void windowDestroyed(const QString &profileId);

signals:
    void zoomTextOnlyChanged(bool textOnly);
    void privacyChanged(bool isPrivate);
    // TOR17: the warm-up request settled — held navigations may flush.
    void torProxyWarmed();

private:
    QString parseArgumentUrl(const QString &string) const;
    QString argumentUrl() const;
    void clean();
    void torStartup(const QString &url);
    // TOR17: fires the sacrificial first request through the tor
    // profile so the network service's lazy proxy poll refreshes the
    // config before any user navigation reaches it; markTorProxyWarmed
    // releases the navigation gate on the probe's settle or a bounded
    // fallback.
    void warmTorProxy();
    void markTorProxyWarmed();
    // PRIV01: opt-in full private-data wipe at quit — runs SEC12's
    // categories (history, cookies, site data, caches) and drops the
    // deferred sentinels so Chromium's on-disk state goes away at the
    // next start even when async deletes don't flush before exit.
    // Idempotent; called from quitBrowser() and ~BrowserApplication().
    void clearPrivateDataOnExit();

    QList<QPointer<BrowserMainWindow> > m_mainWindows;
    QByteArray m_lastSession;
    bool m_standalone;
    bool quitting;

    // TOR02: owned daemon + scratch DataDirectory (auto-removed); both
    // null outside tor mode.  Declared before anything that could run
    // a page so teardown stops the daemon before the dir vanishes.
    TorManager *m_torManager;
    QScopedPointer<QTemporaryDir> m_torDataDir;
    // TOR17: warm-up state for the process-global SOCKS switch — see
    // isTorProxyWarmed()/warmTorProxy().  m_torProxyWarmSeq invalidates
    // stale probe completions when a daemon restart re-warms.
    bool m_torProxyWarmed;
    qint64 m_torProxyWarmSeq;
    QWebEnginePage *m_torProxyProbe;

    Qt::MouseButtons m_eventMouseButtons;
    Qt::KeyboardModifiers m_eventKeyboardModifiers;

    QUrl m_lastAskedUrl;
    QDateTime m_lastAskedUrlDateTime;
};

#endif // BROWSERAPPLICATION_H

