/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2008-2009 Arora Browser
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
** A PARTICULAR PURPOSE.  Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "networkaccessmanager.h"
#include "profilesettings.h"

#include "acceptlanguagedialog.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "networkcookiejar.h"
#include "networkdiskcache.h"
#include "networkproxyfactory.h"
#include "securestore.h"
#include "ui_passworddialog.h"
#include "ui_proxy.h"

#include <qapplication.h>
#include <qdialog.h>
#include <qlocale.h>
#include <qmessagebox.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qstyle.h>

#include <qauthenticator.h>
#include <qcryptographichash.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qset.h>
#include <qsslerror.h>

// #define NETWORKACCESSMANAGER_DEBUG

NetworkAccessManager *NetworkAccessManager::instance()
{
    static QPointer<NetworkAccessManager> manager;
    if (!manager)
        manager = new NetworkAccessManager(qApp);
    return manager;
}

NetworkAccessManager::NetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
    connect(this, &QNetworkAccessManager::authenticationRequired,
            this, &NetworkAccessManager::authenticationRequired);
    connect(this, &QNetworkAccessManager::proxyAuthenticationRequired,
            this, &NetworkAccessManager::proxyAuthenticationRequired);
#ifndef QT_NO_OPENSSL
    connect(this, &QNetworkAccessManager::sslErrors,
            this, &NetworkAccessManager::sslErrors);
#endif
    // BrowserApplication's constructor connects its privacyChanged
    // signal to the privacyChanged() slot of this singleton.
    loadSettings();

    // Web cookies live in the profile's QWebEngineCookieStore (MIG03);
    // this volatile jar only serves application-side requests and is
    // swapped out for a fresh one when private browsing starts.
    setCookieJar(new NetworkCookieJar);
}

void NetworkAccessManager::privacyChanged(bool isPrivate)
{
    Q_UNUSED(isPrivate);
    // A new empty cookie jar keeps private-mode fetches from seeing or
    // persisting any cookies gathered during normal browsing.
    setCookieJar(new NetworkCookieJar);
    if (NetworkDiskCache *diskCache = qobject_cast<NetworkDiskCache*>(cache()))
        diskCache->setPrivate(isPrivate);
}

void NetworkAccessManager::loadSettings()
{
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("proxy"));
    QNetworkProxy proxy;
    // TOR02: a tor process must not honor a configured clearnet proxy —
    // the application proxy IS the tor SOCKS listener.  Leaving the
    // factory's DefaultProxy in place makes every request re-resolve
    // the (currently fail-closed, later real) application proxy.
    if (!BrowserApplication::isTorMode()
            && settings.value(QLatin1String("enabled"), false).toBool()) {
        int proxyType = settings.value(QLatin1String("type"), 0).toInt();
        if (proxyType == 0)
            proxy = QNetworkProxy::Socks5Proxy;
        else if (proxyType == 1)
            proxy = QNetworkProxy::HttpProxy;
        else { // 2
            proxy.setType(QNetworkProxy::HttpCachingProxy);
            proxy.setCapabilities(QNetworkProxy::CachingCapability | QNetworkProxy::HostNameLookupCapability);
        }
        proxy.setHostName(settings.value(QLatin1String("hostName")).toString());
        proxy.setPort(settings.value(QLatin1String("port"), 1080).toInt());
        proxy.setUser(settings.value(QLatin1String("userName")).toString());
        proxy.setPassword(SecureStore::openString(settings.value(QLatin1String("password")).toString()));
    }
    NetworkProxyFactory *proxyFactory = new NetworkProxyFactory;
    if (proxy.type() == QNetworkProxy::HttpCachingProxy) {
        proxyFactory->setHttpProxy(proxy);
        proxyFactory->setGlobalProxy(QNetworkProxy::DefaultProxy);
    } else {
        proxyFactory->setHttpProxy(QNetworkProxy::DefaultProxy);
        proxyFactory->setGlobalProxy(proxy);
    }
    setProxyFactory(proxyFactory);
    settings.endGroup();

#ifndef QT_NO_OPENSSL
    // CA09/S3: the "CaCertificates" store this used to hydrate into
    // the default configuration's CA list is gone — a one-click
    // sslErrors "ignore" installed the peer's chain as a permanent
    // trust anchor for every app-side fetch.  Accepted exceptions
    // are session-scoped (host, fingerprint) in memory now (see
    // sslErrors()); drop the persisted set so certs a prior version
    // anchored stop being honored here.
    settings.remove(QLatin1String("CaCertificates"));
    settings.remove(QLatin1String("network/CaCertificates"));
#endif

    m_acceptLanguage = acceptLanguage();

    settings.beginGroup(QLatin1String("network"));
    bool cacheEnabled = settings.value(QLatin1String("cacheEnabled"), true).toBool();
    if (cacheEnabled) {
        NetworkDiskCache *diskCache;
        if (cache())
            diskCache = qobject_cast<NetworkDiskCache*>(cache());
        else
            diskCache = new NetworkDiskCache(this);
        setCache(diskCache);
        diskCache->loadSettings();
    } else {
        setCache(nullptr);
    }
    settings.endGroup();
}

// The parent every auth/cert dialog centers on — the focused
// window, falling back to the app's first registered one.
static QWidget *dialogParentWindow()
{
    QWidget *mainWindow = QApplication::activeWindow();
    if (BrowserApplication *application = BrowserApplication::instance())
        if (!application->mainWindows().isEmpty())
            mainWindow = application->mainWindow();
    return mainWindow;
}

void NetworkAccessManager::authenticationRequired(QNetworkReply *reply, QAuthenticator *auth)
{
#ifdef NETWORKACCESSMANAGER_DEBUG
    qDebug() << __FUNCTION__ << reply;
#endif
    // Parent the dialog on the application's main window when one is
    // already up; under a plain QApplication (autotests, the standalone
    // smoke harness) the active window is the best we have.
    QWidget *mainWindow = dialogParentWindow();

    QDialog dialog(mainWindow);
    dialog.setWindowFlags(Qt::Sheet);

    Ui::PasswordDialog passwordDialog;
    passwordDialog.setupUi(&dialog);

    passwordDialog.iconLabel->setText(QString());
    passwordDialog.iconLabel->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxQuestion, nullptr, mainWindow).pixmap(32, 32));

    QString introMessage = tr("<qt>Enter username and password for \"%1\" at %2</qt>");
    introMessage = introMessage.arg(auth->realm().toHtmlEscaped()).arg(reply->url().toString().toHtmlEscaped());
    passwordDialog.introLabel->setText(introMessage);
    passwordDialog.introLabel->setWordWrap(true);

    if (dialog.exec() == QDialog::Accepted) {
        auth->setUser(passwordDialog.userNameLineEdit->text());
        auth->setPassword(passwordDialog.passwordLineEdit->text());
    }
}

void NetworkAccessManager::proxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *auth)
{
#ifdef NETWORKACCESSMANAGER_DEBUG
    qDebug() << __FUNCTION__;
#endif
    QWidget *mainWindow = dialogParentWindow();

    QDialog dialog(mainWindow);
    dialog.setWindowFlags(Qt::Sheet);

    Ui::ProxyDialog proxyDialog;
    proxyDialog.setupUi(&dialog);

    proxyDialog.iconLabel->setText(QString());
    proxyDialog.iconLabel->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxQuestion, nullptr, mainWindow).pixmap(32, 32));

    QString introMessage = tr("<qt>Connect to proxy \"%1\" using:</qt>");
    introMessage = introMessage.arg(proxy.hostName().toHtmlEscaped());
    proxyDialog.introLabel->setText(introMessage);
    proxyDialog.introLabel->setWordWrap(true);

    if (dialog.exec() == QDialog::Accepted) {
        auth->setUser(proxyDialog.userNameLineEdit->text());
        auth->setPassword(proxyDialog.passwordLineEdit->text());
    }
}

#ifndef QT_NO_OPENSSL
QString NetworkAccessManager::certToFormattedString(QSslCertificate cert)
{
    QStringList message;
    message << cert.subjectInfo(QSslCertificate::CommonName);
    message << tr("Issuer: %1").arg(cert.issuerInfo(QSslCertificate::CommonName).join(QLatin1String(" ")));
    message << tr("Not valid before: %1").arg(cert.effectiveDate().toString());
    message << tr("Valid until: %1").arg(cert.expiryDate().toString());

    QMultiMap<QSsl::AlternativeNameEntryType, QString> names = cert.subjectAlternativeNames();
    if (names.count() > 0) {
        QString list;
        list += QLatin1String("<br />");
        list += tr("Alternate Names:");
        list += QLatin1String("<ul><li>");
        list += QStringList(names.values(QSsl::DnsEntry)).join(QLatin1String("</li><li>"));
        list += QLatin1String("</li></ul>");
        message << list;
    }

    QString result = QLatin1String("<p>") + message.join(QLatin1String("<br />")) + QLatin1String("</p>");

    return result;
}

// CA09/S3: app-side certificate exceptions are session-scoped
// (host, certificate-fingerprint) pairs kept in memory — the
// Qt-demo flow this replaced installed the peer's chain into the
// default configuration's CA list AND persisted it, so one "ignore"
// click minted a permanent trust anchor covering every host and
// every error type on every app-side fetch (update checks, list
// downloads, sync).  This mirrors WebPage's SEC06 whitelist on the
// page-load plane; nothing is persisted and a changed certificate
// prompts again.
static QSet<QString> &appCertOverrides()
{
    static QSet<QString> overrides;
    return overrides;
}

static QString appCertOverrideKey(const QString &host,
                                  const QSslCertificate &cert)
{
    return host + QLatin1Char('|')
        + QString::fromLatin1(
            cert.digest(QCryptographicHash::Sha256).toHex());
}

void NetworkAccessManager::sslErrors(QNetworkReply *reply, const QList<QSslError> &error)
{
#ifdef NETWORKACCESSMANAGER_DEBUG
    qDebug() << __FUNCTION__;
#endif
    QWidget *mainWindow = dialogParentWindow();

    // Errors the user already proceeded past for this (host,
    // certificate) pair skip the prompt — but only errors carrying a
    // certificate can ever be whitelisted, and a cert-less error or
    // a new certificate always asks again.
    const QString host = reply->url().host().toLower();
    QList<QSslError> pending;
    QStringList errorStrings;
    QStringList certInfos;
    QList<QSslCertificate> shownCerts;
    for (const QSslError &e : error) {
        if (!e.certificate().isNull()
                && appCertOverrides().contains(
                    appCertOverrideKey(host, e.certificate())))
            continue;
        pending.append(e);
        errorStrings += e.errorString();
        if (!e.certificate().isNull()
                && !shownCerts.contains(e.certificate())) {
            shownCerts.append(e.certificate());
            certInfos += certToFormattedString(e.certificate());
        }
    }
    if (pending.isEmpty()) {
        // Every reported error is one already accepted for this
        // (host, certificate) pair — ignore exactly those, nothing
        // more.
        reply->ignoreSslErrors(error);
        return;
    }

    QString errors = errorStrings.join(QLatin1String("</li><li>"));
    const int ret = QMessageBox::warning(mainWindow,
                           QCoreApplication::applicationName() + tr(" - SSL Errors"),
                           tr("<qt>SSL Errors:"
                              "<br/><br/>for: <tt>%1</tt>"
                              "<ul><li>%2</li></ul>"
                              "%3\n\n"
                              "Do you want to ignore these errors for "
                              "this host and certificate until you "
                              "quit?</qt>").arg(reply->url().toString().toHtmlEscaped()).arg(errors)
                              .arg(certInfos.isEmpty()
                                   ? QString()
                                   : tr("<br/>Certificates:<br/>%1")
                                       .arg(certInfos.join(QString()))),
                           QMessageBox::Yes | QMessageBox::No,
                           QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        for (const QSslError &e : pending) {
            if (!e.certificate().isNull())
                appCertOverrides().insert(
                    appCertOverrideKey(host, e.certificate()));
        }
        // Ignore only the errors this reply actually reported — an
        // error arriving later (renegotiation, a different chain)
        // still fails.
        reply->ignoreSslErrors(error);
    }
}
#endif

/*
    Builds the Accept-Language header (RFC 2616 section 14.4) from the
    effective language list — shared with
    BrowserProfile::applySettings(), which pushes the same value to the
    web profile via QWebEngineProfile::setHttpAcceptLanguage (MIG11).
 */
QByteArray NetworkAccessManager::acceptLanguage()
{
    return AcceptLanguageDialog::httpString(AcceptLanguageDialog::acceptLanguages());
}

QNetworkReply *NetworkAccessManager::createRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
    // The WebKit-era interception hooks are gone:
    //  - adblock blocking moved to the profile's
    //    QWebEngineUrlRequestInterceptor (MIG09); application-side
    //    fetches are never ad-matching candidate URLs anyway.
    //  - the AutoFillManager POST capture died with WebEngine (form
    //    posts happen inside Chromium, MIG10 decides the replacement).
    //  - custom scheme replies are now QWebEngineUrlSchemeHandlers on
    //    the profile (SchemeAccessHandler::installAll()).
    QNetworkRequest req = request;
    if (!m_acceptLanguage.isEmpty())
        req.setRawHeader("Accept-Language", m_acceptLanguage);

    QNetworkReply *reply = QNetworkAccessManager::createRequest(op, req, outgoingData);
    emit requestCreated(op, req, reply);
    return reply;
}
