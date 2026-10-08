/*
 * Copyright 2009 Jonas Gehring <jonas.gehring@boolsoft.org>
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

#ifndef SCHEMEACCESSHANDLER_H
#define SCHEMEACCESSHANDLER_H

#include <qwebengineurlschemehandler.h>

#include <qhash.h>
#include <qmutex.h>
#include <qstringlist.h>

class QWebEngineProfile;
class SchemeAccessHandler : public QWebEngineUrlSchemeHandler
{
public:
    SchemeAccessHandler(QObject *parent = nullptr);

    // The scheme this handler serves, e.g. "arora-file".
    virtual QByteArray scheme() const = 0;

    // Declares every application scheme with QWebEngineUrlScheme.
    // Must run before the QApplication constructor; afterwards
    // installAll() can attach the handlers to a profile.
    // The "abp" adblock scheme lives outside network/ — it is declared
    // by AdBlockSchemeAccessHandler::registerUrlScheme() (called from
    // main) and installed by AdBlockManager::installOnProfile().
    static void registerUrlSchemes();
    static void installAll(QWebEngineProfile *profile, QObject *parent = nullptr);

    // SEC06/SAFE01: interstitial plumbing shared by the
    // certificate-error (arora-cert-error:) and HTTPS-Only warning
    // (arora-http-warning:) pages.  WebPage renders the per-warning
    // markup and publishes it under a random nonce; the
    // InterstitialSchemeHandler serves it on the IO thread for
    // <scheme>:interstitial?n=<nonce>.  The nonce is also what WebPage
    // checks on the proceed/back action links, so web content cannot
    // forge either half of the flow.
    static void publishInterstitialPage(const QString &nonce, const QString &html);
    static bool hasInterstitialPage(const QString &nonce);
    static void installInterstitialHandlers(QWebEngineProfile *profile);

    // Internals for InterstitialSchemeHandler — call
    // interstitialMutex() before touching
    // interstitialPages()/interstitialOrder() (IO thread).
    static QMutex &interstitialMutex();
    static QHash<QString, QString> &interstitialPages();
    static QStringList &interstitialOrder();
};

#endif // SCHEMEACCESSHANDLER_H
