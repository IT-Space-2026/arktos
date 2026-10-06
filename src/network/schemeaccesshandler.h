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
    SchemeAccessHandler(QObject *parent = 0);

    // The scheme this handler serves, e.g. "arora-file".
    virtual QByteArray scheme() const = 0;

    // Declares every application scheme with QWebEngineUrlScheme.
    // Must run before the QApplication constructor; afterwards
    // installAll() can attach the handlers to a profile.
    // The "abp" adblock scheme lives outside network/ — it is declared
    // by AdBlockSchemeAccessHandler::registerUrlScheme() (called from
    // main) and installed by AdBlockManager::installOnProfile().
    static void registerUrlSchemes();
    static void installAll(QWebEngineProfile *profile, QObject *parent = 0);

    // SEC06: certificate-error interstitial plumbing.  WebPage renders
    // the per-error markup and publishes it under a random nonce; the
    // CertErrorSchemeHandler serves it on the IO thread for
    // arora-cert-error:interstitial?n=<nonce>.  The nonce is also what
    // WebPage checks on the proceed/back action links, so web content
    // cannot forge either half of the flow.
    static void publishCertErrorPage(const QString &nonce, const QString &html);
    static bool hasCertErrorPage(const QString &nonce);
    static void installCertErrorHandler(QWebEngineProfile *profile);

    // Internals for CertErrorSchemeHandler — call certErrorMutex()
    // before touching certErrorPages()/certErrorOrder() (IO thread).
    static QMutex &certErrorMutex();
    static QHash<QString, QString> &certErrorPages();
    static QStringList &certErrorOrder();
};

#endif // SCHEMEACCESSHANDLER_H
