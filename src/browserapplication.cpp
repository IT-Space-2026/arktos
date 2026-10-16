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

#include "browserapplication.h"

#include "acceptlanguagedialog.h"
#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "arorastyle.h"
#include "autosaver.h"
#include "autofillmanager.h"
#include "bookmarksmanager.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "browsermainwindow.h"
#include "browsertheme.h"
#include "clearprivatedata.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "domainblocklist.h"
#include "downloadmanager.h"
#include "energysaver.h"
#include "extensionmanager.h"
#include "historymanager.h"
#include "languagemanager.h"
#include "networkaccessmanager.h"
#include "pdfsupport.h"
#include "privacyrequestinterceptor.h"
#include "webenginebackend.h"
#include "schemeaccesshandler.h"
#include "securestore.h"
#include "startupprofile.h"
#include "tabwidget.h"
#include "tormanager.h"
#include "torrequestinterceptor.h"
#include "trancolist.h"
#include "webview.h"

#include <qjsonarray.h>

#ifdef ARORA_RUSTCORE
#include "rustcore.h"
#include "rustcorebridge.h"

#include <qjsondocument.h>
#include <qjsonobject.h>
#endif

#include <qbuffer.h>
#include <qdesktopservices.h>
#include <qdir.h>
#include <qevent.h>
#include <qhash.h>
#include <qlibraryinfo.h>
#include <qlocalsocket.h>
#include <qmessagebox.h>
#include <qnetworkproxy.h>
#include <qprocess.h>
#include <qset.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qstatusbar.h>
#include <qstylehints.h>
#include <qtemporarydir.h>
#include <qtimer.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#include <qdebug.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// #define BROWSERAPPLICATION_DEBUG

// MIG03: private browsing is a profile property under Qt WebEngine —
// while enabled, new windows/pages are created on an off-the-record
// profile instead of the default one.
static bool s_isPrivate = false;

// TOR02: set by the constructor's argv scan (or setTorMode() in
// AUTOTESTS builds).  A tor process browses exclusively on
// BrowserProfile::torProfile() — an unnamed, off-the-record profile —
// and routes every request through the managed daemon's SOCKS5
// listener via the process-global QNetworkProxy::setApplicationProxy.
static bool s_torMode = false;

// Profiles that already carry the application-level services — cookie
// jar, custom scheme handlers, download manager, adblock interceptor —
// so prepareProfile() never installs them twice.  The off-the-record
// profile is created lazily, hence prepared on first use (MIG15: this
// resolves the "install on the OTR profile too" TODOs left by
// MIG03/MIG05/MIG09).
static QSet<QWebEngineProfile *> s_preparedProfiles;

void BrowserApplication::prepareProfile(QWebEngineProfile *profile)
{
    if (!profile || s_preparedProfiles.contains(profile))
        return;
    s_preparedProfiles.insert(profile);
    CookieJar::instance(profile);
    SchemeAccessHandler::installAll(profile, qApp);
    BrowserProfile::applySettings(profile);
    if (BrowserApplication::isTorMode()) {
        // TOR02 hardening, applied on top of the user preferences:
        // a configured UA or DNS prefetch would fingerprint or leak
        // the tor session — the vanilla UA always applies, and no
        // lookup may precede the proxied request.
        profile->setHttpUserAgent(BrowserProfile::defaultHttpUserAgent());
        BrowserProfile::applyClientHints(profile);
        profile->settings()->setAttribute(
            QWebEngineSettings::DnsPrefetchEnabled, false);
        // PRIV02: Accept-Language is normalized unconditionally on the
        // tor profile — a localized list next to the vanilla UA is a
        // per-user fingerprint regardless of the opt-in toggle.
        profile->setHttpAcceptLanguage(QString::fromUtf8(
            AcceptLanguageDialog::httpString(
                AcceptLanguageDialog::normalizedAcceptLanguages())));
    }
    DownloadManager::instance()->installOnProfile(
        WebEngineProfileAdapter::forProfile(profile));
    if (!BrowserApplication::isTorMode()) {
        // BADSSL03: TLS client certificates are strong user identity —
        // re-register the user's installed certs on every prepared
        // profile except tor's, where they must never be offered.
        BrowserProfile::loadClientCertificates(profile);
    }
    // PERF03: install the interceptor with the matcher's first rules
    // snapshot deferred — parsing every subscribed list here delayed
    // the first window.  postLaunch() queues the rebuild once the
    // startup navigation is underway; requests made in between see an
    // empty ruleset (allow-all).
    AdBlockManager::instance()->installOnProfile(profile, true);
    if (BrowserApplication::isTorMode()) {
        // A profile accepts exactly one request interceptor — the
        // HTTPS-first upgrade wraps the adblock matcher inside
        // TorRequestInterceptor.
        profile->setUrlRequestInterceptor(new TorRequestInterceptor(
            AdBlockManager::instance()->network(), profile));
    } else {
        // PRIV01: the adblock-only interceptor installOnProfile set is
        // replaced by the composite that also runs the privacy stages
        // (HTTPS-first navigation upgrade + cross-site referrer trim).
        profile->setUrlRequestInterceptor(new PrivacyRequestInterceptor(
            AdBlockManager::instance()->network(), profile));
    }
    if (!BrowserApplication::isTorMode()) {
        // QWebEngineExtensionManager wiring + user-scripts injection.
        // The OTR profile is skipped for extensions — Qt 6.12 refuses
        // OTR loads outright ("Can't load in off-the-record mode",
        // EXT04) — but still receives user scripts.  The tor profile
        // gets neither — extensions and injected scripts are
        // fingerprintable surface; keep it that way (hard refusal).
        ExtensionManager::instance()->installOnProfile(profile);
    }
}

BrowserApplication::BrowserApplication(int &argc, char **argv)
    : SingleApplication(argc, argv)
    , m_standalone(false)
    , quitting(false)
    , m_torManager(nullptr)
    , m_torProxyWarmed(true)
    , m_torProxyWarmSeq(0)
    , m_torProxyProbe(nullptr)
{
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setOrganizationDomain(QLatin1String("arora-browser.org"));
    QCoreApplication::setApplicationName(QLatin1String("Arora"));
    QCoreApplication::setApplicationVersion(QLatin1String("0.12"
#ifdef GITVERSION
    " (Git: " GITCHANGENUMBER " " GITVERSION ")"
#endif
    ));

#ifndef AUTOTESTS
    // Any option-style argument makes the run standalone: smoke tests
    // and --quit-after-load must always run locally, and --help /
    // --version should not be forwarded to a running instance.
    const QStringList args = QCoreApplication::arguments();
    for (const QString &arg : args) {
        if (arg.startsWith(QLatin1String("--"))) {
            m_standalone = true;
            break;
        }
    }
    if (args.contains(QLatin1String("--tor"))
            || args.contains(QLatin1String("--tor-window-smoke")))
        s_torMode = true;

    if (!m_standalone) {
        connect(this, &SingleApplication::messageReceived,
                this, &BrowserApplication::messageReceived);

        const QString url = argumentUrl();
        if (!url.isEmpty())
            sendMessage(url.toUtf8());
        // If we could connect to another Arora then exit
        QString message = QString(QLatin1String("aroramessage://getwinid"));
        if (sendMessage(message.toUtf8(), 500))
            return;

#ifdef BROWSERAPPLICATION_DEBUG
        qDebug() << "BrowserApplication::" << __FUNCTION__ << "I am the only arora";
#endif

        // not sure what else to do...
        if (!startSingleServer())
            return;
    }
#endif

    // UIP01: follow the desktop light/dark preference.  Platform
    // themes that react to the scheme swap the palette themselves —
    // applyColorScheme() only steps in when the palette disagrees with
    // the reported scheme, and re-applies on live scheme changes.
    BrowserTheme::applyColorScheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [](Qt::ColorScheme) { BrowserTheme::applyColorScheme(); });

    // UIP02: uniform modern button metrics in every dialog.
    BrowserTheme::installDialogButtonPolish();

    // THEME01: the central chrome polish — spacing/flat-chrome/section
    // typography from one source.  Install after applyColorScheme() so
    // the proxy wraps the post-scheme style; the hook re-wraps and
    // re-resolves on every later flip.
    AroraStyle::install();

#if defined(Q_OS_MACOS)
    QApplication::setQuitOnLastWindowClosed(false);
#else
    QApplication::setQuitOnLastWindowClosed(true);
#endif

    QDesktopServices::setUrlHandler(QLatin1String("http"), this, "openUrl");

    if (s_torMode) {
        // TOR02: fail closed.  Until the daemon reports its SOCKS
        // listener the application proxy points at a guaranteed-dead
        // loopback port — a navigation that somehow races bootstrap
        // errors out instead of leaking onto clearnet.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(
            QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 1));

        m_torDataDir.reset(new QTemporaryDir(
            QDir::temp().filePath(QLatin1String("arora-tor-XXXXXX"))));
        m_torManager = new TorManager(this);
        if (m_torDataDir->isValid())
            m_torManager->setDataDirectory(m_torDataDir->path());
        // TOR17: re-point the application proxy the moment the control
        // channel learns the SOCKS endpoint — the listener is bound at
        // daemon startup, long before ready().  A request that does
        // reach the listener early just dies inside tor — fail-closed
        // is preserved either way.  Chromium's network service,
        // though, re-reads QNetworkProxy only from a lazy poll inside
        // proxy resolution: the first request after every switch
        // still resolves on the STALE endpoint (the live-trace
        // signature was a first-search ERR_PROXY_CONNECTION_FAILED on
        // the fail-closed port at ready+0.5s).  warmTorProxy() feeds
        // that stale read a sacrificial probe instead of the user's
        // search.
        connect(m_torManager, &TorManager::socksListenerDiscovered,
                this, [this](const QNetworkProxy &proxy) {
            QNetworkProxy::setApplicationProxy(proxy);
            warmTorProxy();
        });
        connect(m_torManager, &TorManager::ready,
                this, [this](const QNetworkProxy &proxy) {
            QNetworkProxy::setApplicationProxy(proxy);
            const QList<BrowserMainWindow*> windows = mainWindows();
            for (BrowserMainWindow *window : windows)
                window->statusBar()->showMessage(
                    tr("Connected to the Tor network"), 5000);
        });
        connect(m_torManager, &TorManager::bootstrapProgressChanged,
                this, [this](int progress, const QString &) {
            const QList<BrowserMainWindow*> windows = mainWindows();
            for (BrowserMainWindow *window : windows)
                window->statusBar()->showMessage(
                    tr("Connecting to the Tor network — %1%").arg(progress));
        });
        connect(m_torManager, &TorManager::failed,
                this, [this](const QString &reason) {
            qWarning() << "tor:" << reason;
            const QList<BrowserMainWindow*> windows = mainWindows();
            for (BrowserMainWindow *window : windows)
                window->statusBar()->showMessage(
                    tr("Tor connection failed: %1").arg(reason));
        });
        // TOR17: surface the daemon's own notice-level output on the
        // real window path too — without it a launch failure is only
        // visible as dead tabs on a stderr-attached run.
        connect(m_torManager, &TorManager::logLine,
                this, [](const QString &line) {
            qWarning().noquote() << "tor:" << line;
        });
        m_torManager->start();
    }

    // Chromium defaults to 16 too, but keep the explicit value the
    // WebKit port forced.  Applied to whichever browsing profile the
    // process uses — QWebEngineProfile::defaultProfile() is itself
    // off-the-record in Qt6, so it is the wrong target (MIG06).
    QWebEngineSettings *engineSettings = webEngineProfile()->settings();
    engineSettings->setFontSize(QWebEngineSettings::DefaultFontSize, 16);
    engineSettings->setFontSize(QWebEngineSettings::DefaultFixedFontSize, 16);

    // Bring up the normal browsing profile and attach the per-profile
    // services (prepareProfile applies the persisted settings over the
    // baseline above).  The private profile is prepared lazily by
    // webEngineProfile() the first time private browsing hands it out.
    webEngineProfile();
    StartupProfile::mark("ctor: browsing profile + services prepared");

    QSettings settings;
    settings.beginGroup(QLatin1String("sessions"));
    m_lastSession = settings.value(QLatin1String("lastSession")).toByteArray();
    settings.endGroup();

#if defined(Q_OS_MACOS)
    connect(this, &QApplication::lastWindowClosed,
            this, &BrowserApplication::lastWindowClosed);
#endif

    // setting this in the postLaunch actually takes a lot more time
    // because the event has to be propagated to everyone.
    setWindowIcon(QIcon(QLatin1String(":128x128/arora.png")));

#ifndef AUTOTESTS
    QTimer::singleShot(0, this, &BrowserApplication::postLaunch);
#endif

    // Installs the translators for the persisted/system language.
    connect(languageManager(), &LanguageManager::languageChanged,
            this, &BrowserApplication::retranslate);
    // The app-side fetch manager swaps to a fresh volatile cookie jar
    // and marks its disk cache private while private browsing is on.
    connect(this, &BrowserApplication::privacyChanged,
            networkAccessManager(), &NetworkAccessManager::privacyChanged);
    if (s_torMode) {
        // setPrivate() is never toggled in a tor process, so the
        // signal above never fires — flag the app-side fetch manager
        // private directly: volatile cookie jar, disk cache disabled.
        networkAccessManager()->privacyChanged(true);
    }
    StartupProfile::mark("ctor: done");
}

BrowserApplication::~BrowserApplication()
{
    quitting = true;
    clearPrivateDataOnExit();
    // SEC13: drop the passphrase-derived key and any cached file key.
    SecureStore::lock();
    qDeleteAll(m_mainWindows);
    if (m_torManager) {
        // SHUTDOWN over the control channel, bounded escalate inside;
        // TAKEOWNERSHIP also kills tor if the process dies abruptly.
        m_torManager->stop();
    }
}

#if defined(Q_OS_MACOS)
void BrowserApplication::lastWindowClosed()
{
    clean();
    BrowserMainWindow *mw = new BrowserMainWindow;
    mw->goHome();
    m_mainWindows.prepend(mw);
}
#endif

BrowserApplication *BrowserApplication::instance()
{
    // dynamic_cast so callers get nullptr (not a bad pointer) when qApp
    // is a plain QApplication, e.g. in autotests.
    return dynamic_cast<BrowserApplication*>(QCoreApplication::instance());
}

bool BrowserApplication::isStandalone() const
{
    return m_standalone;
}

void BrowserApplication::retranslate()
{
    bookmarksManager()->retranslate();
    networkAccessManager()->loadSettings();
}

// The only special property of an argument url is that the file's
// can be local, they don't have to be absolute.
QString BrowserApplication::parseArgumentUrl(const QString &string) const
{
    if (QFile::exists(string)) {
        QFileInfo info(string);
        return info.canonicalFilePath();
    }
    return string;
}

// The url operand is the last argument that is not an option — the
// QCommandLineParser in main() treats all --flags as options.
QString BrowserApplication::argumentUrl() const
{
    const QStringList args = QCoreApplication::arguments();
    for (int i = args.count() - 1; i > 0; --i) {
        if (!args.at(i).startsWith(QLatin1Char('-')))
            return parseArgumentUrl(args.at(i));
    }
    return QString();
}

void BrowserApplication::messageReceived(QLocalSocket *socket)
{
    QString message;
    QTextStream stream(socket);
    stream >> message;
#ifdef BROWSERAPPLICATION_DEBUG
    qDebug() << "BrowserApplication::" << __FUNCTION__ << message;
#endif
    if (message.isEmpty())
        return;

    // Got a normal url
    if (!message.startsWith(QLatin1String("aroramessage://"))) {
        // SEC02/SEC09: a forwarded url must never become script
        // execution — the single-instance socket is reachable by
        // anything running as this user.  The javascript: check moved
        // into loadStringFromUntrustedSource, which gates the
        // *resolved* url so normalization tricks are still caught.
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        TabWidget::OpenUrlIn tab = TabWidget::OpenUrlIn(settings.value(QLatin1String("openLinksFromAppsIn"), TabWidget::NewSelectedTab).toInt());
        settings.endGroup();
        if (QUrl(message) == m_lastAskedUrl
                && m_lastAskedUrlDateTime.addSecs(10) > QDateTime::currentDateTime()) {
            qWarning() << "Possible recursive openUrl called, ignoring url:" << m_lastAskedUrl;
            return;
        }
        mainWindow()->tabWidget()->loadStringFromUntrustedSource(message, tab);
        return;
    }

    if (message.startsWith(QLatin1String("aroramessage://getwinid"))) {
#ifdef Q_OS_WIN
        QString winid = QString(QLatin1String("%1")).arg((qlonglong)mainWindow()->winId());
#else
        mainWindow()->show();
        mainWindow()->setFocus();
        mainWindow()->raise();
        mainWindow()->activateWindow();
        alert(mainWindow());
        QString winid;
#endif
#ifdef BROWSERAPPLICATION_DEBUG
        qDebug() << "BrowserApplication::" << __FUNCTION__ << "sending win id" << winid << mainWindow()->winId();
#endif
        QString message = QLatin1String("aroramessage://winid/") + winid;
        socket->write(message.toUtf8());
        socket->waitForBytesWritten(2000);
        return;
    }

    if (message.startsWith(QLatin1String("aroramessage://winid"))) {
        QString winid = message.mid(21);
#ifdef BROWSERAPPLICATION_DEBUG
        qDebug() << "BrowserApplication::" << __FUNCTION__ << "got win id:" << winid;
#endif
#ifdef Q_OS_WIN
        WId wid = (WId)winid.toLongLong();
        SetForegroundWindow(reinterpret_cast<HWND>(wid));
#endif
        return;
    }
}

void BrowserApplication::quitBrowser()
{
    if (!downloadManager()->allowQuit())
        return;

    // PREF04: 'Show Exit Confirmation Dialog' (tabs/confirmExit)
    // guards the explicit quit path.  When the key was never written
    // it inherits the migrated close-window confirm's value, so a user
    // who turned that off is not surprised by a new prompt — while the
    // old default (on) keeps the quit warning alive.
    QSettings settings;
    const bool confirmWindow = settings.value(
        QLatin1String("tabs/confirmClosingMultipleTabs"), true).toBool();
    clean();
    if (settings.value(QLatin1String("tabs/confirmExit"),
                       confirmWindow).toBool()
            && !m_mainWindows.isEmpty()) {
        int tabCount = 0;
        for (int i = 0; i < m_mainWindows.count(); ++i) {
            // CONT06: include tabs hidden in filtered container levels.
            tabCount += m_mainWindows.at(i)->tabWidget()->totalTabCount();
        }

        QWidget *widget = mainWindow();
        QApplication::alert(widget);
        const QString question =
            (m_mainWindows.count() > 1 || tabCount > 1)
            ? tr("There are %1 windows and %2 tabs open\n"
                 "Do you want to quit anyway?")
                  .arg(m_mainWindows.count()).arg(tabCount)
            : tr("Do you want to quit Arora?");
        int ret = QMessageBox::warning(widget, QString(), question,
                           QMessageBox::Yes | QMessageBox::No,
                           QMessageBox::No);
        if (ret == QMessageBox::No)
            return;
    }

    saveSession();
    clearPrivateDataOnExit();
    // SESS02: ::exit() never unwinds to main()'s markSessionCleanExit,
    // so an explicit Quit would leave the sentinel armed and score as
    // a crash on the next start — record the clean exit first.
    BrowserProfile::markSessionCleanExit(0);
    exit(0);
}

/*!
    Any actions that can be delayed until the window is visible
 */
void BrowserApplication::postLaunch()
{
    StartupProfile::mark("postLaunch: begin");

    // PDF01: staging copies from the sanitize-on-view pipeline live
    // only for the session — sweep leftovers from the previous run.
    PdfSupport::sweepTempDir();

    // The WebKit icon database is gone in Qt WebEngine — icons are
    // delivered per-page via QWebEnginePage::iconChanged and cached by
    // HistoryManager, so there is nothing to configure here.

    loadSettings();
    StartupProfile::mark("postLaunch: settings applied");

    // newMainWindow() needs to be called in main() for this to happen
    if (m_mainWindows.count() > 0) {
        const QString url = argumentUrl();

        if (isTorMode()) {
            // TOR02: no navigation may leave this process before the
            // daemon's SOCKS listener is live — the fail-closed proxy
            // would just error the load, so the startup navigation is
            // deferred until ready.  Sessions are never restored here:
            // the blob is clearnet-profile state.
            if (m_torManager && !m_torManager->isReady()) {
                connect(m_torManager, &TorManager::ready, this,
                        [this, url]() { torStartup(url); });
            } else {
                torStartup(url);
            }
        } else if (!url.isEmpty()) {
            // SEC09: argv urls are untrusted input, same as a
            // forwarded second-instance message — an external program
            // invoking `arora javascript:...` must not get script
            // execution in the new window.
            switch (startupMode()) {
            case LastSessionStartup: {
                restoreLastSession();
                mainWindow()->tabWidget()->loadStringFromUntrustedSource(url, TabWidget::NewSelectedTab);
                break;
            }
            default:
                mainWindow()->tabWidget()->loadStringFromUntrustedSource(url);
                break;
            }
        } else {
            // PREF04: the Startup-with radios — see startupMode() for
            // the MainWindow/startupMode key + legacy migration.
            switch (startupMode()) {
            case LastSessionStartup:
                restoreLastSession();
                break;
            case HomepageStartup:
                mainWindow()->goHome();
                break;
            case SpecificPagesStartup:
                openStartupPages(mainWindow());
                break;
            case StartPageStartup:
            default:
                mainWindow()->tabWidget()->loadUrl(startPageUrl());
                break;
            }
        }
    }
    StartupProfile::mark("postLaunch: navigation dispatched");

    // SESS02: the dispatch above must never leave a real session
    // windowless — an empty/corrupt session blob, a declined
    // crash-guard restore prompt, or a window lost between its
    // creation and this queued slot all land here without a live
    // BrowserMainWindow.  Standalone runs manage their own windows.
    if (!isStandalone() || isTorMode())
        ensureSessionWindow();

    // PERF03: the heavyweight stores were forced synchronously here —
    // after the first window painted but before the startup
    // navigation could make progress, so every millisecond of
    // parsing delayed the first page.  Queue them instead: this
    // zero-timeout slot runs when the event queue drains, i.e. while
    // the engine is already fetching.
    QTimer::singleShot(0, this, [this]() {
        StartupProfile::mark("deferred warmup: begin");
        // HistoryManager parses the history file and builds its three
        // models on construction — everything downstream (menu,
        // completer, addHistoryEntry) uses the same lazy instance.
        BrowserApplication::historyManager();
        // prepareProfile() installed the request interceptors with the
        // matcher's snapshot deferred — build it now, while the first
        // page is already in flight.
        AdBlockManager::instance()->network()->rebuildRules();
        StartupProfile::mark("deferred warmup: done");

        // TELEM01: the seeded ad-block subscriptions stay dormant until
        // the user consents to remote list downloads — ask once here, on
        // the first normal launch (standalone smoke runs and the tor
        // process never prompt; the tor profile still fetches lists only
        // if consent was already granted).
        if (!isStandalone() && !isTorMode())
            AdBlockManager::instance()->maybePromptForListConsent(
                m_mainWindows.isEmpty() ? nullptr : mainWindow());
        // SEC18: the domain blocklist refreshes on the same consent
        // gate and weekly cadence as the remote filter lists — a
        // just-granted consent above fetches immediately, otherwise
        // only a stale copy is refetched.  Without rustcore or
        // consent this is a no-op and the vendored seed alone blocks.
        DomainBlocklist::instance()->updateIfStale();
        // TRUST04: the Tranco rank table rides the same consent gate
        // and cadence — a no-op under the same conditions.
        TrancoList::instance()->updateIfStale();
    });
}

// TOR02: the tor window's first navigation — deferred by postLaunch()
// until TorManager reports the SOCKS listener ready (or run at once
// when it already is).  url is the sanitized argv operand, if any.
void BrowserApplication::torStartup(const QString &url)
{
    if (m_mainWindows.isEmpty())
        return;
    BrowserMainWindow *window = mainWindow();
    if (!url.isEmpty()) {
        // CONT07: the argv operand may be a hand-off from a link
        // context menu in another process — it gets the stricter
        // page-link gate, so a javascript:/data:/blob: operand is
        // refused outright instead of executing in this fresh chrome
        // context.
        if (isUrlAllowedOnTorArgv(url)) {
            window->tabWidget()->loadStringFromUntrustedSource(url);
        } else {
            qWarning() << "Ignoring untrusted --tor argv url:" << url;
        }
        return;
    }
    // PREF04: the startup pick applies minus Last Session — the
    // session blob is clearnet-profile state.  Start Page needs no
    // navigation here: the window's fresh tor tab already carries the
    // branded start page.
    switch (startupMode()) {
    case HomepageStartup:
        window->goHome();
        break;
    case SpecificPagesStartup:
        openStartupPages(window);
        break;
    default:
        break;
    }
}

void BrowserApplication::loadSettings()
{
    // PREFUI05: the Energy Saver mode is profile-independent — it must
    // be re-read in tor mode too, so it sits above the early return.
    EnergySaver::instance()->loadSettings();
    // MIG11: the QSettings->QWebEngineSettings mapping moved to
    // BrowserProfile so the settings dialog can share it while this
    // file is uncompiled.  Dropped keys with no WebEngine equivalent:
    // enableInspector (devtools always available), userStyleSheet is
    // now injected as a QWebEngineScript, maximumPagesInCache (Chromium
    // manages its own cache).  Applied to every profile the app has
    // brought up — private browsing keeps user preferences too.
    if (isTorMode()) {
        // TOR02: only the tor profile exists; the hardening pins from
        // prepareProfile must be re-applied — applySettings would
        // otherwise restore a user-configured UA / DNS prefetch.
        QWebEngineProfile *profile = BrowserProfile::torProfile();
        BrowserProfile::applySettings(profile);
        profile->setHttpUserAgent(BrowserProfile::defaultHttpUserAgent());
        BrowserProfile::applyClientHints(profile);
        profile->setHttpAcceptLanguage(QString::fromUtf8(
            AcceptLanguageDialog::httpString(
                AcceptLanguageDialog::normalizedAcceptLanguages())));
        profile->settings()->setAttribute(
            QWebEngineSettings::DnsPrefetchEnabled, false);
        return;
    }
    BrowserProfile::applySettings(BrowserProfile::normalProfile());
    if (QWebEngineProfile *otr = BrowserProfile::privateProfileIfCreated())
        BrowserProfile::applySettings(otr);
    // CONT01: materialized container profiles keep user preferences
    // too — they run the same applySettings as the normal profile.
    ContainerManager::instance()->reapplySettings();
}

QList<BrowserMainWindow*> BrowserApplication::mainWindows()
{
    clean();
    QList<BrowserMainWindow*> list;
    for (int i = 0; i < m_mainWindows.count(); ++i)
        list.append(m_mainWindows.at(i));
    return list;
}

bool BrowserApplication::allowToCloseWindow(BrowserMainWindow *window)
{
    Q_UNUSED(window)
    if (mainWindows().count() > 1)
        return true;

    return downloadManager()->allowQuit();
}

void BrowserApplication::clean()
{
    // cleanup any deleted main windows first
    for (int i = m_mainWindows.count() - 1; i >= 0; --i)
        if (m_mainWindows.at(i).isNull())
            m_mainWindows.removeAt(i);
}

static const qint32 BrowserApplicationMagic = 0xec;

void BrowserApplication::saveSession()
{
    if (quitting)
        return;
    QSettings settings;
    settings.beginGroup(QLatin1String("MainWindow"));
    settings.setValue(QLatin1String("restoring"), false);
    settings.endGroup();

    if (isPrivate())
        return;

    clean();

    settings.beginGroup(QLatin1String("sessions"));

#ifdef ARORA_RUSTCORE
    // RCORE03: the rustcore session store owns the schema and the
    // file — a JSON manifest per window (opaque shell blob, structured
    // tab records with container binding + engine tag + opaque
    // engine-state blob), atomically installed as session.dat.
    rustCoreEnsureDataDir();
    QJsonArray windows;
    for (int i = 0; i < m_mainWindows.count(); ++i) {
        BrowserMainWindow *window = m_mainWindows.at(i);
        QJsonObject win = window->tabWidget()->sessionStateJson();
        win.insert(QLatin1String("shell"), QString::fromLatin1(
            window->saveState(false).toBase64()));
        windows.append(win);
    }
    QJsonObject root;
    root.insert(QLatin1String("version"), 1);
    root.insert(QLatin1String("windows"), windows);
    const QByteArray manifest =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (rc_session_save(
            reinterpret_cast<const uint8_t *>(manifest.constData()),
            size_t(manifest.size())) == RC_OK) {
        // Retire the pre-rustcore QSettings blob so a stale copy can
        // never win a restore or survive the exit wipe.
        settings.remove(QLatin1String("lastSession"));
        settings.endGroup();
        m_lastSession.clear();
        return;
    }
    char *err = rc_last_error_message();
    qWarning() << "BrowserApplication::saveSession rustcore:"
               << (err ? err : "") << "— falling back to QSettings";
    rc_string_free(err);
#endif

    int version = 2;

    QByteArray data;
    QBuffer buffer(&data);
    QDataStream stream(&buffer);
    buffer.open(QIODevice::WriteOnly);

    stream << qint32(BrowserApplicationMagic);
    stream << qint32(version);

    stream << qint32(m_mainWindows.count());
    for (int i = 0; i < m_mainWindows.count(); ++i)
        stream << m_mainWindows.at(i)->saveState();
    settings.setValue(QLatin1String("lastSession"), data);
    settings.endGroup();
    m_lastSession = data;
}

void BrowserApplication::flushSessionsForTermination(int signalNumber)
{
    // CRASH01: a signal-driven shutdown is a clean exit — clear the
    // crash-loop sentinel/counter so logoff/service-stop kills aren't
    // scored against the next run's GPU safe-mode check.  SESS02: the
    // lastExit record still says 'terminated by signal N' rather than
    // 'clean rc=0' — a signal death must not masquerade as a normal
    // exit.  Done first: the watchdog may kill the process mid-flush.
    if (signalNumber)
        BrowserProfile::markSessionTerminated(signalNumber);
    else
        BrowserProfile::markSessionCleanExit();
    clean();
    for (int i = 0; i < m_mainWindows.count(); ++i) {
        if (BrowserMainWindow *window = m_mainWindows.at(i))
            window->m_autoSaver->saveIfNeccessary();
    }
    // The debounce only covers changes it was notified about — write
    // the live state unconditionally so anything that bypassed
    // changeOccurred() still lands in the session blob.  Private
    // browsing stays excluded: saveSession() writes nothing while
    // isPrivate().
    saveSession();
}

bool BrowserApplication::ensureSessionWindow()
{
    clean();
    bool created = false;
    if (m_mainWindows.isEmpty()) {
        qWarning() << "BrowserApplication: startup produced no browser"
                      " window — opening a fallback";
        BrowserMainWindow *window = newMainWindow();
        window->tabWidget()->loadUrl(startPageUrl(),
                                     TabWidget::CurrentTab);
        created = true;
    }
    // The same blank-ness one level down: a window whose strip ended
    // empty (e.g. every restored tab detached into a filtered
    // container level) still needs one live tab.
    for (const QPointer<BrowserMainWindow> &window :
             std::as_const(m_mainWindows)) {
        if (window && window->tabWidget()->totalTabCount() == 0)
            window->tabWidget()->newTabPage();
    }
    return created;
}

bool BrowserApplication::canRestoreSession() const
{
#ifdef ARORA_RUSTCORE
    rustCoreEnsureDataDir();
    if (rc_session_exists())
        return true;
#endif
    return !m_lastSession.isEmpty();
}

void BrowserApplication::clearPrivateDataOnExit()
{
    // Opt-in wipe (privacy/clearOnExit, default off).  The tor profile
    // is off-the-record — nothing to remove.
    if (isTorMode())
        return;
    if (!QSettings().value(QLatin1String("privacy/clearOnExit"), false).toBool())
        return;

    // CLR01: the exit wipe is the full category set at persistent
    // scope plus the two flags no dialog checkbox maps to —
    // ClearSession (the saved session blob is itself browsing
    // history; leaving it behind would undo the wipe) and
    // ClearFullWipe (the PRIV01 sentinel-driven removal of the
    // Network/+cache trees the async profile deletes may not flush
    // before exit, and the CONT05 whole-tree removal of
    // never-materialized containers).  The plan covers every profile
    // the app materialized — normal plus containers — and executing
    // the shared plan keeps the exit wipe identical to the dialog's.
    ClearPrivateData::executePlan(ClearPrivateData::plan(
        QLatin1String("all"),
        ClearPrivateData::ClearAll | ClearPrivateData::ClearSession
            | ClearPrivateData::ClearFullWipe,
        false));
}

bool BrowserApplication::restoreLastSession()
{
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("MainWindow"));
        if (settings.value(QLatin1String("restoring"), false).toBool()) {
            QMessageBox::StandardButton result = QMessageBox::question(nullptr, tr("Restore failed"),
                tr("Arora crashed while trying to restore this session.  Should I try again?"), QMessageBox::Yes | QMessageBox::No);
            if (result == QMessageBox::No)
                return false;
        }
    }
#ifdef ARORA_RUSTCORE
    // RCORE03: the canonical store is the rustcore session file — the
    // crate validated the whole blob before it reached us, so RC_OK
    // means the window set is complete and safe to arm the guard on.
    rustCoreEnsureDataDir();
    {
        RcBuffer stored{nullptr, 0};
        const RcStatus sessionStatus = rc_session_load(&stored);
        if (sessionStatus == RC_OK) {
            const QByteArray manifest(
                reinterpret_cast<const char *>(stored.data),
                int(stored.len));
            rc_buffer_free(stored);
            const QJsonDocument doc = QJsonDocument::fromJson(manifest);
            if (!doc.isObject())
                return false;
            const QJsonArray windows = doc.object()
                .value(QLatin1String("windows")).toArray();
            {
                QSettings settings;
                settings.beginGroup(QLatin1String("MainWindow"));
                // Armed only once the blob has validated — a corrupt
                // session can never loop the crash prompt.
                settings.setValue(QLatin1String("restoring"), true);
            }
            for (int i = 0; i < windows.count(); ++i) {
                const QJsonObject win = windows.at(i).toObject();
                BrowserMainWindow *newWindow = nullptr;
                if (i == 0 && m_mainWindows.count() >= 1) {
                    newWindow = mainWindow();
                } else {
                    newWindow = newMainWindow();
                }
                // The shell blob carries no tabs (saveState(false));
                // the structured record set restores them.
                newWindow->restoreState(QByteArray::fromBase64(
                    win.value(QLatin1String("shell"))
                        .toString().toLatin1()));
                newWindow->tabWidget()->restoreSessionState(win);
            }
            return true;
        }
        if (sessionStatus == RC_CORRUPT)
            return false;
        // RC_NOT_FOUND: fall through to the legacy QSettings blob — a
        // session saved before the Rust store restores once through
        // the old reader, and the next save writes the new file.
    }
#endif
    int version = 2;
    QList<QByteArray> windows;
    QBuffer buffer(&m_lastSession);
    QDataStream stream(&buffer);
    buffer.open(QIODevice::ReadOnly);

    qint32 marker;
    qint32 v;
    stream >> marker;
    stream >> v;
    if (marker != BrowserApplicationMagic || v != version)
        return false;

    qint32 windowCount;
    stream >> windowCount;
    // Bound the count by the blob itself — each serialized window
    // occupies at least a 4-byte length prefix, so a corrupt blob
    // claiming more windows than its size permits is invalid.
    const qint64 maxWindows = stream.device()
        ? stream.device()->bytesAvailable() / qint64(sizeof(qint32))
        : 0;
    if (windowCount < 0 || windowCount > maxWindows)
        return false;
    for (qint32 i = 0; i < windowCount; ++i) {
        QByteArray windowState;
        stream >> windowState;
        if (stream.status() != QDataStream::Ok)
            return false;
        windows.append(windowState);
    }
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("MainWindow"));
        // saveSession will be called by an AutoSaver timer from the set
        // tabs and in saveSession we will reset this flag back to false —
        // it is only set once the blob has validated, so a corrupt or
        // stale session does not loop the crash prompt on every launch.
        settings.setValue(QLatin1String("restoring"), true);
    }
    for (int i = 0; i < windows.count(); ++i) {
        BrowserMainWindow *newWindow = nullptr;
        if (i == 0 && m_mainWindows.count() >= 1) {
            newWindow = mainWindow();
        } else {
            newWindow = newMainWindow();
        }
        newWindow->restoreState(windows.at(i));
    }
    return true;
}

#if defined(Q_OS_MACOS)
bool BrowserApplication::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::ApplicationActivate: {
        clean();
        if (!m_mainWindows.isEmpty()) {
            BrowserMainWindow *mw = mainWindow();
            if (mw && !mw->isMinimized()) {
                mainWindow()->show();
            }
            return true;
        }
    }
    case QEvent::FileOpen:
        if (!m_mainWindows.isEmpty()) {
            QString file = static_cast<QFileOpenEvent*>(event)->file();
            mainWindow()->tabWidget()->loadUrl(QUrl::fromLocalFile(file));
            return true;
        }
    default:
        break;
    }
    return QApplication::event(event);
}
#endif

void BrowserApplication::askDesktopToOpenUrl(const QUrl &url)
{
    m_lastAskedUrl = url;
    m_lastAskedUrlDateTime = QDateTime::currentDateTime();
    QDesktopServices::openUrl(url);
}

void BrowserApplication::openUrl(const QUrl &url)
{
    setEventMouseButtons(mouseButtons());
    setEventKeyboardModifiers(keyboardModifiers());
    // SEC23: this slot backs the registered QDesktopServices 'http'
    // handler — any in-app path can invoke it, so the url gets the
    // same untrusted-source gate as argv and single-instance socket
    // input (a javascript: url here would execute script in the
    // current page).
    if (!WebView::isUrlAllowedOnUntrustedInput(url)) {
        qWarning() << "BrowserApplication: refusing untrusted url"
                      " from the QDesktopServices handler:" << url;
        return;
    }
    mainWindow()->tabWidget()->loadUrl(url);
}

BrowserMainWindow *BrowserApplication::newMainWindow()
{
    if (!m_mainWindows.isEmpty())
        mainWindow()->m_autoSaver->saveIfNeccessary();
    BrowserMainWindow *browser = new BrowserMainWindow();
    m_mainWindows.prepend(browser);
    connect(this, &BrowserApplication::privacyChanged,
            browser, &BrowserMainWindow::privacyChanged);
#ifndef AUTOTESTS
    // SESS02: if this turns out to be the last browser window, a stray
    // visible top-level (a dialog outliving its parent window) would
    // keep the process running windowless — windowDestroyed() forces
    // the clean exit the close was meant to be.
    connect(browser, &QObject::destroyed,
            this, &BrowserApplication::windowDestroyed);
#endif
    browser->show();
    if (m_mainWindows.count() == 1)
        StartupProfile::mark("first window shown");
    return browser;
}

void BrowserApplication::windowDestroyed()
{
    // Defer past the destroyed() emission — the QPointer in
    // m_mainWindows does not clear until ~QObject unwinds.
    QTimer::singleShot(0, this, [this]() {
        if (quitting)
            return;
        // Standalone smokes open and close windows in scripted order —
        // their harness owns the lifetime.
        if (isStandalone() && !isTorMode())
            return;
        clean();
        if (!m_mainWindows.isEmpty())
            return;
        // If no visible top-level survived, quitOnLastWindowClosed is
        // already unwinding exec() — nothing to do.  The zombie case
        // is a stray VISIBLE top-level keeping the event loop alive
        // with no browser chrome left (observed live: a session
        // restore whose pages died under it left a process only a
        // second launch could rouse).
        bool strayVisible = false;
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->isVisible()
                && !qobject_cast<BrowserMainWindow*>(top)) {
                strayVisible = true;
                break;
            }
        }
        if (!strayVisible)
            return;
        // The dying window's ~BrowserMainWindow already flushed the
        // session through its AutoSaver.  exit() on the application —
        // NOT ::exit() — lets exec() unwind so main() still records
        // the 'clean rc=0' breadcrumb; a bare ::exit() here would
        // leave the sentinel armed and read as a crash next start.
        qWarning() << "BrowserApplication: last browser window is gone"
                      " but a stray top-level kept the process alive"
                      " — exiting instead of running windowless";
        QCoreApplication::exit(0);
    });
}

BrowserMainWindow *BrowserApplication::newMainWindowInContainer(const QString &containerId)
{
    BrowserMainWindow *browser = newMainWindow();
    if (containerId.isEmpty() || isPrivate() || isTorMode())
        return browser;
    TabWidget *tabs = browser->tabWidget();
    // The window constructor already made a default (inheriting) first
    // tab — a non-default container swaps it for a bound one rather
    // than threading a profile through the window's whole setup.
    if (tabs->containerIdForTab(tabs->currentIndex()) != containerId
        && tabs->makeNewTabInContainer(containerId, true))
        tabs->closeTab(0);
    return browser;
}

BrowserMainWindow *BrowserApplication::mainWindow()
{
    clean();

    BrowserMainWindow *activeWindow = nullptr;

    if (m_mainWindows.isEmpty()) {
        activeWindow = newMainWindow();
    } else {
        activeWindow = qobject_cast<BrowserMainWindow*>(QApplication::activeWindow());
        if (!activeWindow)
            activeWindow = m_mainWindows[0];
    }

    return activeWindow;
}

CookieJar *BrowserApplication::cookieJar()
{
    // MIG11: the jar-per-profile registry lives on CookieJar now.
    return CookieJar::instance(webEngineProfile());
}

QWebEngineProfile *BrowserApplication::webEngineProfile()
{
    // MIG11: the lazy profile singletons live in BrowserProfile so
    // compiled modules share the same named "arora"/OTR profiles.
    // prepareProfile() attaches the app-level services the first time a
    // profile is handed out — this is where the off-the-record profile
    // picks up its cookie jar, scheme handlers, download manager and
    // adblock interceptor when private browsing starts.
    QWebEngineProfile *profile = isTorMode()
        ? BrowserProfile::torProfile()
        : isPrivate()
            ? BrowserProfile::privateProfile()
            : BrowserProfile::normalProfile();
    prepareProfile(profile);
    return profile;
}

QWebEngineProfile *BrowserApplication::privateWebEngineProfile()
{
    QWebEngineProfile *profile = BrowserProfile::privateProfile();
    prepareProfile(profile);
    return profile;
}

DownloadManager *BrowserApplication::downloadManager()
{
    // MIG11: the dialog owns its application-wide singleton now.
    return DownloadManager::instance();
}

NetworkAccessManager *BrowserApplication::networkAccessManager()
{
    // MIG04: the manager owns its application-wide singleton now.
    return NetworkAccessManager::instance();
}

HistoryManager *BrowserApplication::historyManager()
{
    // MIG06: the store owns its application-wide singleton now.
    return HistoryManager::instance();
}

BookmarksManager *BrowserApplication::bookmarksManager()
{
    // MIG07: the store owns its application-wide singleton now.
    return BookmarksManager::instance();
}

LanguageManager *BrowserApplication::languageManager()
{
    // MIG11: the manager owns its application-wide singleton now.
    return LanguageManager::instance();
}

AutoFillManager *BrowserApplication::autoFillManager()
{
    // MIG10: the store owns its application-wide singleton now.
    return AutoFillManager::instance();
}

QIcon BrowserApplication::icon(const QUrl &url)
{
    // MIG06: icons are cached on the HistoryManager, fed by
    // QWebEnginePage::iconChanged (the WebKit icon database is gone).
    return HistoryManager::instance()->icon(url);
}

QString BrowserApplication::installedDataDirectory()
{
#if defined(Q_OS_UNIX)
    // PACK01: prefer the relocatable-bundle layout (<bindir>/../share/arora)
    // over the compile-time PKGDATADIR so an unpacked bundle finds its
    // shipped translations and useragents.xml wherever it is extracted.
    // On macOS <bindir> is Contents/MacOS, so this resolves to
    // Contents/share/arora — and Contents/Resources is the conventional
    // macOS data location, checked first.
    const QString appDir = qApp->applicationDirPath();
    for (const QLatin1String rel : {
#ifdef Q_OS_MACOS
             QLatin1String("/../Resources/share/arora"),
#endif
             QLatin1String("/../share/arora")}) {
        const QString bundled = QDir(appDir + rel).absolutePath();
        if (QFileInfo::exists(bundled))
            return bundled;
    }
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(PKGDATADIR)
    return QLatin1String(PKGDATADIR);
#else
    return qApp->applicationDirPath();
#endif
}

QString BrowserApplication::dataFilePath(const QString &fileName)
{
    // MIG06: shared implementation moved to browserpaths.h so ported
    // modules can reach the data dir before this file compiles again.
    return BrowserPaths::dataFilePath(fileName);
}

// Qt WebEngine has no text-only zoom attribute — Chromium zoom applies
// to the whole page — so the preference selects which path
// WebView::applyZoom drives (per-page font scaling vs page factor).
static bool s_zoomTextOnly = false;

bool BrowserApplication::zoomTextOnly()
{
    return s_zoomTextOnly;
}

void BrowserApplication::setZoomTextOnly(bool textOnly)
{
    s_zoomTextOnly = textOnly;
    if (BrowserApplication *app = instance())
        emit app->zoomTextOnlyChanged(textOnly);
}

bool BrowserApplication::isPrivate()
{
    // There is no global private-browsing attribute in Qt WebEngine; the
    // flag selects which profile webEngineProfile() hands out.  Tor
    // mode counts as private (TOR02): every persistence guard —
    // session save, recent searches, download records — treats the
    // process as off-the-record.
    return s_isPrivate || s_torMode;
}

void BrowserApplication::setPrivate(bool isPrivate)
{
    if (s_isPrivate == isPrivate)
        return;
    s_isPrivate = isPrivate;
    if (BrowserApplication *app = instance())
        emit app->privacyChanged(isPrivate);
}

bool BrowserApplication::isTorMode()
{
    return s_torMode;
}

void BrowserApplication::setTorMode(bool torMode)
{
    s_torMode = torMode;
}

QUrl BrowserApplication::startPageUrl()
{
    return startPageUrl(defaultEngineId(), isPrivate());
}

QUrl BrowserApplication::startPageUrl(const QString &engineId,
                                      bool privateContext)
{
    if (s_torMode)
        return QUrl(QLatin1String("qrc:/startpage-tor.html"));
    if (privateContext)
        return QUrl(QLatin1String("qrc:/startpage-private.html"));
    if (engineId == QLatin1String("servo"))
        return QUrl(QLatin1String("qrc:/startpage-servo.html"));
    return QUrl(QLatin1String("qrc:/startpage.html"));
}

// PREF04: the Homepage radio's persisted mode.  A pre-mode settings
// store has no homePageMode key — then the old free-form "home" value
// decides: the default about:home IS the start page, anything else was
// a user-picked specific page.
int BrowserApplication::homePageMode()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("MainWindow"));
    const QVariant mode = settings.value(QLatin1String("homePageMode"));
    if (mode.isValid())
        return mode.toInt() == SpecificPageHome
            ? SpecificPageHome : StartPageHome;
    const QString home = settings.value(
        QLatin1String("home"), QLatin1String("about:home")).toString();
    return home.trimmed().isEmpty() || home == QLatin1String("about:home")
        ? StartPageHome : SpecificPageHome;
}

// PREF04: the Startup radio's persisted mode.  Migration — the retired
// combo stored MainWindow/startupBehavior: 0 "home page" -> Homepage,
// 1 "blank page" -> Start Page (the fresh tab it described now carries
// the start page), 2 "restore" -> Last Session.
int BrowserApplication::startupMode()
{
    QSettings settings;
    const QVariant mode =
        settings.value(QLatin1String("MainWindow/startupMode"));
    if (mode.isValid())
        return qBound(0, mode.toInt(), int(SpecificPagesStartup));
    const QVariant legacy =
        settings.value(QLatin1String("MainWindow/startupBehavior"));
    if (legacy.isValid()) {
        switch (legacy.toInt()) {
        case 0:
            return HomepageStartup;
        case 2:
            return LastSessionStartup;
        default:
            return StartPageStartup;
        }
    }
    return StartPageStartup;
}

QStringList BrowserApplication::startupPages()
{
    return QSettings()
        .value(QLatin1String("MainWindow/startupPages")).toStringList();
}

void BrowserApplication::openStartupPages(BrowserMainWindow *window)
{
    if (!window || !window->tabWidget())
        return;
    bool first = true;
    const QStringList pages = startupPages();
    for (const QString &page : pages) {
        const QString trimmed = page.trimmed();
        if (trimmed.isEmpty())
            continue;
        window->tabWidget()->loadString(trimmed,
            first ? TabWidget::CurrentTab : TabWidget::NewNotSelectedTab);
        first = false;
    }
}

QString BrowserApplication::defaultEngineId()
{
    return QSettings().value(QLatin1String("MainWindow/defaultEngine"),
                             QStringLiteral("webengine")).toString();
}

TorManager *BrowserApplication::torManager() const
{
    return m_torManager;
}

bool BrowserApplication::isTorProxyWarmed() const
{
    return m_torProxyWarmed;
}

// TOR17: Chromium's network service re-reads QNetworkProxy only from a
// lazy poll inside proxy resolution — and the request that triggers
// the poll still resolves on the STALE endpoint, so the first
// navigation after every setApplicationProxy() eats a
// net::ERR_PROXY_CONNECTION_FAILED on the fail-closed port (measured
// on 6.12: the sacrificial request at +490ms post-switch still failed;
// every later one used the new config).  Feed that stale read a
// probe on the tor profile instead of the user's search; the queue
// gate (WebView::torMustQueue) holds remote navigations until the
// probe settles so the stale read is never user-facing.
void BrowserApplication::warmTorProxy()
{
    m_torProxyWarmed = false;
    const qint64 seq = ++m_torProxyWarmSeq;
    if (m_torProxyProbe) {
        // A restart re-points the proxy and re-warms — a still-flying
        // probe from the last round must not settle this one.
        m_torProxyProbe->disconnect(this);
        m_torProxyProbe->triggerAction(QWebEnginePage::Stop);
        m_torProxyProbe->deleteLater();
    }
    QWebEngineProfile *profile = BrowserProfile::torProfile();
    prepareProfile(profile);
    m_torProxyProbe = new QWebEnginePage(profile, this);
    connect(m_torProxyProbe, &QWebEnginePage::loadFinished,
            this, [this, seq](bool) {
        if (seq == m_torProxyWarmSeq)
            markTorProxyWarmed();
    });
    // check.torproject.org is the canonical "is tor working" endpoint;
    // the probe's own outcome is irrelevant — its resolution refreshes
    // the proxy config either way.
    m_torProxyProbe->load(QUrl(QStringLiteral(
        "https://check.torproject.org/api/ip")));
    // The resolution runs within milliseconds of the request entering
    // the network service — never hold user navigations on the rest
    // of the probe's lifecycle.
    QTimer::singleShot(3000, this, [this, seq]() {
        if (seq == m_torProxyWarmSeq)
            markTorProxyWarmed();
    });
}

void BrowserApplication::markTorProxyWarmed()
{
    if (m_torProxyWarmed)
        return;
    m_torProxyWarmed = true;
    emit torProxyWarmed();
}

bool BrowserApplication::isUrlAllowedOnTorArgv(const QString &operand)
{
    return WebView::isUrlAllowedFromPageLink(QUrl(operand));
}

bool BrowserApplication::siteTrustEnabled()
{
    return QSettings().value(QLatin1String("trust/enabled"), true)
            .toBool();
}

bool BrowserApplication::siteTrustLookalikeEnabled()
{
    return QSettings().value(QLatin1String("trust/lookalike"), true)
            .toBool();
}

bool BrowserApplication::siteTrustThreatlistEnabled()
{
    return QSettings().value(QLatin1String("trust/threatlist"), true)
            .toBool();
}

QStringList BrowserApplication::torWindowArguments(const QUrl &url)
{
    QStringList arguments{QStringLiteral("--tor")};
    // CONT07: an optional start url travels as its own argv element —
    // startDetached never involves a shell, so no quoting/escaping
    // hazard exists — and the receiver re-gates it as untrusted
    // input.  Refuse the dangerous schemes on this side too.
    if (url.isValid() && !url.isEmpty()
        && WebView::isUrlAllowedFromPageLink(url))
        arguments << QString::fromUtf8(url.toEncoded());
    return arguments;
}

void BrowserApplication::openTorWindow(const QUrl &url)
{
    // TOR02: a separate process — the application proxy is
    // process-global, so tor routing can never share this one.  Each
    // tor process manages its own daemon (TAKEOWNERSHIP binds its
    // lifetime to the process), so several tor windows coexist
    // independently.
    if (!QProcess::startDetached(
            QCoreApplication::applicationFilePath(),
            torWindowArguments(url))) {
        // A failed spawn is otherwise invisible — the detached
        // process has no channel back to this one.
        qWarning() << "tor: failed to spawn"
                   << QCoreApplication::applicationFilePath();
    }
}

Qt::MouseButtons BrowserApplication::eventMouseButtons() const
{
    return m_eventMouseButtons;
}

Qt::KeyboardModifiers BrowserApplication::eventKeyboardModifiers() const
{
    return m_eventKeyboardModifiers;
}

void BrowserApplication::setEventMouseButtons(Qt::MouseButtons buttons)
{
    m_eventMouseButtons = buttons;
}

void BrowserApplication::setEventKeyboardModifiers(Qt::KeyboardModifiers modifiers)
{
    m_eventKeyboardModifiers = modifiers;
}

