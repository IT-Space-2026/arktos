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
#include "autosaver.h"
#include "autofillmanager.h"
#include "bookmarksmanager.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "browsermainwindow.h"
#include "browsertheme.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "domainblocklist.h"
#include "downloadmanager.h"
#include "extensionmanager.h"
#include "historymanager.h"
#include "languagemanager.h"
#include "networkaccessmanager.h"
#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "securestore.h"
#include "startupprofile.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "tormanager.h"
#include "torrequestinterceptor.h"
#include "webview.h"

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
    DownloadManager::instance()->installOnProfile(profile);
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
{
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setOrganizationDomain(QLatin1String("arora-browser.org"));
    QCoreApplication::setApplicationName(QLatin1String("Arora"));
    QCoreApplication::setApplicationVersion(QLatin1String("0.2"
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
        SetForegroundWindow(wid);
#endif
        return;
    }
}

void BrowserApplication::quitBrowser()
{
    if (!downloadManager()->allowQuit())
        return;

    if (QSettings().value(QLatin1String("tabs/confirmClosingMultipleTabs"), true).toBool()) {
        clean();
        int tabCount = 0;
        for (int i = 0; i < m_mainWindows.count(); ++i) {
            tabCount += m_mainWindows.at(i)->tabWidget()->count();
        }

        if (tabCount > 1) {
            QWidget *widget = mainWindow();
            QApplication::alert(widget);
            int ret = QMessageBox::warning(widget, QString(),
                               tr("There are %1 windows and %2 tabs open\n"
                                  "Do you want to quit anyway?").arg(m_mainWindows.count()).arg(tabCount),
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No);
            if (ret == QMessageBox::No)
                return;
        }
    }

    saveSession();
    clearPrivateDataOnExit();
    exit(0);
}

/*!
    Any actions that can be delayed until the window is visible
 */
void BrowserApplication::postLaunch()
{
    StartupProfile::mark("postLaunch: begin");

    // The WebKit icon database is gone in Qt WebEngine — icons are
    // delivered per-page via QWebEnginePage::iconChanged and cached by
    // HistoryManager, so there is nothing to configure here.

    loadSettings();
    StartupProfile::mark("postLaunch: settings applied");

    // newMainWindow() needs to be called in main() for this to happen
    if (m_mainWindows.count() > 0) {
        QSettings settings;
        settings.beginGroup(QLatin1String("MainWindow"));
        int startup = settings.value(QLatin1String("startupBehavior")).toInt();
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
            switch (startup) {
            case 2: {
                restoreLastSession();
                mainWindow()->tabWidget()->loadStringFromUntrustedSource(url, TabWidget::NewSelectedTab);
                break;
            }
            default:
                mainWindow()->tabWidget()->loadStringFromUntrustedSource(url);
                break;
            }
        } else {
            switch (startup) {
            case 0:
                mainWindow()->goHome();
                break;
            case 1:
                break;
            case 2:
                restoreLastSession();
                break;
            }
        }
    }
    StartupProfile::mark("postLaunch: navigation dispatched");

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
        window->tabWidget()->loadStringFromUntrustedSource(url);
        return;
    }
    // startupBehavior: homepage (0) still applies; restore-session (2)
    // is meaningless — the blob belongs to the clearnet profile — and
    // blank (1) is the fallback for it.
    if (QSettings().value(QLatin1String("MainWindow/startupBehavior")).toInt() == 0)
        window->goHome();
}

void BrowserApplication::loadSettings()
{
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

bool BrowserApplication::canRestoreSession() const
{
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

    // Every browsing profile the app materialized: the normal profile
    // plus any container profiles — a "clear all data" exit wipe that
    // skipped containers would silently keep their cookies/storage.
    QList<QWebEngineProfile*> profiles;
    profiles.append(BrowserProfile::normalProfile());
    profiles.append(ContainerManager::instance()->createdProfiles());

    HistoryManager *history = HistoryManager::instance();
    history->clear();
    history->clearIcons();
    for (QWebEngineProfile *profile : profiles) {
        profile->clearAllVisitedLinks();
        CookieJar::instance(profile)->clear();
        profile->clearHttpCache();
    }
    DownloadManager::instance()->cleanup();
    // The search-history lists live in every ToolbarSearch; windows
    // still exist on both call paths, so clear them while they can.
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        if (ToolbarSearch *search = qobject_cast<ToolbarSearch*>(widget))
            search->clear();
    }
    // Sentinel-driven disk wipe at next start — covers the DOM
    // storage trees AND the cookie/cache network state that async
    // profile deletes may not flush before this process exits.
    for (QWebEngineProfile *profile : profiles)
        BrowserProfile::clearAllStorageOnNextStart(profile);

    // CONT05: registered containers whose profile was never
    // materialized this session have no live profile to send the
    // sentinels to — nothing holds their trees open, so remove them
    // outright.  Without this an exit wipe would silently keep every
    // unused container's cookies, DOM storage and cache.
    ContainerManager::instance()->wipeUnmaterializedStorage();

    // The saved session blob is itself browsing history — leaving it
    // behind would undo the wipe.
    QSettings settings;
    settings.beginGroup(QLatin1String("sessions"));
    settings.remove(QLatin1String("lastSession"));
    settings.endGroup();
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
    browser->show();
    if (m_mainWindows.count() == 1)
        StartupProfile::mark("first window shown");
    return browser;
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
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // PACK01: prefer the relocatable-bundle layout (<bindir>/../share/arora)
    // over the compile-time PKGDATADIR so an unpacked bundle finds its
    // shipped translations and useragents.xml wherever it is extracted.
    const QString bundled = QDir(qApp->applicationDirPath()
                                 + QLatin1String("/../share/arora"))
                            .absolutePath();
    if (QFileInfo::exists(bundled))
        return bundled;
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
// to the whole page.  Keep the preference stored anyway so the menu
// action stays in sync and the value survives the migration.
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

TorManager *BrowserApplication::torManager() const
{
    return m_torManager;
}

void BrowserApplication::openTorWindow()
{
    // TOR02: a separate process — the application proxy is
    // process-global, so tor routing can never share this one.  Each
    // tor process manages its own daemon (TAKEOWNERSHIP binds its
    // lifetime to the process), so several tor windows coexist
    // independently.
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                          QStringList() << QStringLiteral("--tor"));
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

