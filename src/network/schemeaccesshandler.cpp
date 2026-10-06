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

#include "schemeaccesshandler.h"

#include "fileaccesshandler.h"

#include <qbuffer.h>
#include <qhash.h>
#include <qmutex.h>
#include <qset.h>
#include <qstringlist.h>
#include <qurlquery.h>
#include <quuid.h>
#include <qwebengineprofile.h>
#include <qwebengineurlrequestjob.h>
#include <qwebengineurlscheme.h>

SchemeAccessHandler::SchemeAccessHandler(QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
{
}

// SEC06: serves the certificate-error interstitial at
// arora-cert-error:interstitial?n=<nonce>.  The interstitial cannot be
// delivered with setHtml(): a data: document committed on top of a
// failed navigation is sandboxed by Chromium — its links to a custom
// scheme land on about:blank#blocked and never reach
// acceptNavigationRequest.  Serving the page from a real URL keeps
// its action links working.
//
// The rendered markup is per-error (host, chain details, nonce-bound
// action links), so WebPage publishes it into a nonce-keyed registry
// and this handler looks it up on the IO thread.  Action URLs
// (arora-cert-error:proceed|back) are intercepted and refused by
// WebPage::acceptNavigationRequest before they ever reach here.
class CertErrorSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    CertErrorSchemeHandler(QObject *parent = 0)
        : QWebEngineUrlSchemeHandler(parent)
    {
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl url = job->requestUrl();
        QString html;
        {
            QMutexLocker lock(&SchemeAccessHandler::certErrorMutex());
            html = SchemeAccessHandler::certErrorPages().value(
                    QUrlQuery(url).queryItemValue(QLatin1String("n")));
        }
        if (url.path() != QLatin1String("interstitial") || html.isEmpty()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        QBuffer *buffer = new QBuffer(job);
        buffer->setData(html.toUtf8());
        job->reply(QByteArrayLiteral("text/html"), buffer);
    }
};

QMutex &SchemeAccessHandler::certErrorMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString> &SchemeAccessHandler::certErrorPages()
{
    static QHash<QString, QString> pages;
    return pages;
}

QStringList &SchemeAccessHandler::certErrorOrder()
{
    static QStringList order;
    return order;
}

void SchemeAccessHandler::publishCertErrorPage(const QString &nonce,
        const QString &html)
{
    QMutexLocker lock(&certErrorMutex());
    certErrorPages().insert(nonce, html);
    certErrorOrder().append(nonce);
    // Bound the registry — each entry is a few KiB of markup.  Pages
    // are only re-served while their nonce survives, so evicting the
    // oldest just makes a very stale interstitial 404 on reload.
    while (certErrorOrder().size() > 64)
        certErrorPages().remove(certErrorOrder().takeFirst());
}

bool SchemeAccessHandler::hasCertErrorPage(const QString &nonce)
{
    QMutexLocker lock(&certErrorMutex());
    return certErrorPages().contains(nonce);
}

void SchemeAccessHandler::installCertErrorHandler(QWebEngineProfile *profile)
{
    if (!profile)
        return;
    // GUI thread only (called from installAll / WebPage).  Tracks which
    // profiles already carry the handler so foreign profiles touched by
    // a WebPage get it lazily without double-installing.
    static QSet<QWebEngineProfile *> installed;
    if (installed.contains(profile))
        return;
    profile->installUrlSchemeHandler(
            QByteArrayLiteral("arora-cert-error"),
            new CertErrorSchemeHandler(profile));
    installed.insert(profile);
    QObject::connect(profile, &QObject::destroyed, profile, [profile]() {
        installed.remove(profile);
    });
}

void SchemeAccessHandler::registerUrlSchemes()
{
    // file:// is a built-in scheme and cannot take a custom handler, so
    // directory listings are served on arora-file:// instead (WebPage
    // redirects file:// directory navigations there).
    QWebEngineUrlScheme scheme(FileAccessHandler::schemeName());
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    // Deliberately NOT SecureScheme or CorsEnabled (SEC02): a
    // directory listing has no business being a secure context or a
    // CORS target.  LocalScheme/LocalAccessAllowed keep the file-like
    // treatment the listing needs for its file:// entry links;
    // FileAccessHandler additionally rejects remote initiators.
    scheme.setFlags(QWebEngineUrlScheme::LocalScheme
                    | QWebEngineUrlScheme::LocalAccessAllowed
                    | QWebEngineUrlScheme::ViewSourceAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);

    // SEC06: arora-cert-error: carries the certificate-error
    // interstitial (served by CertErrorSchemeHandler from the nonce
    // registry) and its action links (proceed/back), which
    // WebPage::acceptNavigationRequest resolves and refuses.  The
    // scheme must be registered or clicks go down Chromium's
    // external-protocol path and never reach that virtual.
    QWebEngineUrlScheme certErrorScheme(
        QByteArrayLiteral("arora-cert-error"));
    certErrorScheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    certErrorScheme.setFlags(QWebEngineUrlScheme::SecureScheme);
    QWebEngineUrlScheme::registerScheme(certErrorScheme);
}

void SchemeAccessHandler::installAll(QWebEngineProfile *profile, QObject *parent)
{
    FileAccessHandler *fileHandler = new FileAccessHandler(parent);
    profile->installUrlSchemeHandler(fileHandler->scheme(), fileHandler);
    installCertErrorHandler(profile);
}
