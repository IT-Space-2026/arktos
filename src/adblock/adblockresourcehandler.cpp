/**
 * Copyright (c) 2026, The Arora Authors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "adblockresourcehandler.h"

#include <qbuffer.h>
#include <qhash.h>
#include <qimage.h>
#include <qmutex.h>
#include <qurl.h>
#include <qwebengineurlrequestjob.h>
#include <qwebengineurlscheme.h>

namespace {

struct StubResource {
    QByteArray mimeType;
    QByteArray body;
};

QByteArray transparentPng(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

QHash<QByteArray, StubResource> buildResourceTable()
{
    QHash<QByteArray, StubResource> table;

    StubResource gif;
    gif.mimeType = QByteArrayLiteral("image/gif");
    gif.body = QByteArray::fromBase64(
        QByteArrayLiteral(
            "R0lGODlhAQABAIAAAP///////yH5BAEKAAEALAAAAAABAAEAAAICTAEAOw=="));
    table.insert(QByteArrayLiteral("1x1.gif"), gif);

    StubResource png2;
    png2.mimeType = QByteArrayLiteral("image/png");
    png2.body = transparentPng(2, 2);
    table.insert(QByteArrayLiteral("2x2.png"), png2);

    StubResource png3;
    png3.mimeType = QByteArrayLiteral("image/png");
    png3.body = transparentPng(3, 2);
    table.insert(QByteArrayLiteral("3x2.png"), png3);

    StubResource png32;
    png32.mimeType = QByteArrayLiteral("image/png");
    png32.body = transparentPng(32, 32);
    table.insert(QByteArrayLiteral("32x32.png"), png32);

    StubResource js;
    js.mimeType = QByteArrayLiteral("application/javascript");
    js.body = QByteArrayLiteral("(function() {})();\n");
    table.insert(QByteArrayLiteral("noop.js"), js);

    StubResource txt;
    txt.mimeType = QByteArrayLiteral("text/plain");
    txt.body = QByteArrayLiteral("\n");
    table.insert(QByteArrayLiteral("noop.txt"), txt);

    StubResource html;
    html.mimeType = QByteArrayLiteral("text/html");
    html.body = QByteArrayLiteral(
        "<!DOCTYPE html><html><head><title></title></head>"
        "<body></body></html>\n");
    table.insert(QByteArrayLiteral("noop.html"), html);

    StubResource click;
    click.mimeType = QByteArrayLiteral("text/html");
    click.body = QByteArrayLiteral(
        "<!DOCTYPE html><html><head><title>Blocked frame</title>"
        "<style>body{font-family:sans-serif;color:#555;padding:1em}"
        "a{cursor:pointer}</style></head>"
        "<body><a href=\"#\">This content was blocked by Arora."
        " Click to load.</a>"
        "<script>document.querySelector('a').onclick=function(){"
        "location.reload();return false;};</script>"
        "</body></html>\n");
    table.insert(QByteArrayLiteral("click2load.html"), click);

    StubResource css;
    css.mimeType = QByteArrayLiteral("text/css");
    css.body = QByteArrayLiteral("\n");
    table.insert(QByteArrayLiteral("noop.css"), css);

    StubResource vast2;
    vast2.mimeType = QByteArrayLiteral("text/xml");
    vast2.body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<VAST version=\"2.0\"></VAST>\n");
    table.insert(QByteArrayLiteral("noop-vast-2.0"), vast2);

    StubResource vast3 = vast2;
    vast3.body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<VAST version=\"3.0\"></VAST>\n");
    table.insert(QByteArrayLiteral("noop-vast-3.0"), vast3);

    StubResource vast4 = vast2;
    vast4.body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<VAST version=\"4.0\"></VAST>\n");
    table.insert(QByteArrayLiteral("noop-vast-4.0"), vast4);

    StubResource vmap;
    vmap.mimeType = QByteArrayLiteral("text/xml");
    vmap.body = QByteArrayLiteral(
        "<VMAP xmlns:vast=\"http://www.iab.net/videosuite/vmap\""
        " version=\"1.0\"></VMAP>\n");
    table.insert(QByteArrayLiteral("noop-vmap-1.0"), vmap);

    return table;
}

const QHash<QByteArray, StubResource> &resourceTable()
{
    static const QHash<QByteArray, StubResource> table =
        buildResourceTable();
    return table;
}

const QHash<QString, QByteArray> &aliasTable()
{
    static const QHash<QString, QByteArray> aliases = {
        { QStringLiteral("1x1-transparent-gif"), "1x1.gif" },
        { QStringLiteral("1x1.gif"), "1x1.gif" },
        { QStringLiteral("2x2-transparent-png"), "2x2.png" },
        { QStringLiteral("2x2.png"), "2x2.png" },
        { QStringLiteral("3x2-transparent-png"), "3x2.png" },
        { QStringLiteral("3x2.png"), "3x2.png" },
        { QStringLiteral("32x32-transparent-png"), "32x32.png" },
        { QStringLiteral("32x32.png"), "32x32.png" },
        { QStringLiteral("noopjs"), "noop.js" },
        { QStringLiteral("noop.js"), "noop.js" },
        { QStringLiteral("abp-resource:blank-js"), "noop.js" },
        { QStringLiteral("nooptext"), "noop.txt" },
        { QStringLiteral("noop.txt"), "noop.txt" },
        { QStringLiteral("empty"), "noop.txt" },
        { QStringLiteral("none"), "noop.txt" },
        { QStringLiteral("noopframe"), "noop.html" },
        { QStringLiteral("noop.html"), "noop.html" },
        { QStringLiteral("blank"), "noop.html" },
        { QStringLiteral("noopcss"), "noop.css" },
        { QStringLiteral("noop.css"), "noop.css" },
        { QStringLiteral("click2load.html"), "click2load.html" },
        { QStringLiteral("noop-vast-2.0"), "noop-vast-2.0" },
        { QStringLiteral("noop-vast-3.0"), "noop-vast-3.0" },
        { QStringLiteral("noop-vast-4.0"), "noop-vast-4.0" },
        { QStringLiteral("noop-vmap-1.0"), "noop-vmap-1.0" },
        // Common named scripts in the ABP redirectable list; a no-op
        // implementation covers them.
        { QStringLiteral("googlesyndication_adsbygoogle.js"), "noop.js" },
        { QStringLiteral("googlesyndication.com/adsbygoogle.js"), "noop.js" },
        { QStringLiteral("googletagmanager_gtm.js"), "noop.js" },
        { QStringLiteral("googletagmanager.com/gtm.js"), "noop.js" },
        { QStringLiteral("googletagservices_gpt.js"), "noop.js" },
        { QStringLiteral("googletagservices.com/gpt.js"), "noop.js" },
        { QStringLiteral("google-analytics_analytics.js"), "noop.js" },
        { QStringLiteral("google-analytics.com/analytics.js"), "noop.js" },
        { QStringLiteral("amazon_adsbygoogle.js"), "noop.js" },
        { QStringLiteral("amazon-apstag.js"), "noop.js" },
        { QStringLiteral("scorecardresearch_beacon.js"), "noop.js" },
        { QStringLiteral("doubleclick_instream_ad_status.js"), "noop.js" },
        { QStringLiteral("outbrain-widget.js"), "noop.js" },
        { QStringLiteral("monkeybroker.js"), "noop.js" },
        { QStringLiteral("nobab.js"), "noop.js" },
        { QStringLiteral("bab-defuser.js"), "noop.js" },
        { QStringLiteral("fingerprint2.js"), "noop.js" },
        { QStringLiteral("popads.js"), "noop.js" },
        { QStringLiteral("popads.net.js"), "noop.js" },
        { QStringLiteral("hd-main.js"), "noop.js" },
        { QStringLiteral("noeval.js"), "noop.js" },
        { QStringLiteral("silent-noeval.js"), "noop.js" },
    };
    return aliases;
}

} // namespace

AdBlockResourceHandler::AdBlockResourceHandler(QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
{
}

QByteArray AdBlockResourceHandler::schemeName()
{
    return QByteArrayLiteral("arora-resource");
}

void AdBlockResourceHandler::registerUrlScheme()
{
    QWebEngineUrlScheme scheme(schemeName());
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    // Redirect targets must load from any page origin: secure so https
    // pages accept them, CSP-ignored so strict sites cannot defeat the
    // redirect, fetch-allowed so XHR/fetch stubs work.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::CorsEnabled
                    | QWebEngineUrlScheme::ContentSecurityPolicyIgnored
                    | QWebEngineUrlScheme::FetchApiAllowed
                    | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
}

QByteArray AdBlockResourceHandler::canonicalResourceName(const QString &name)
{
    QString cleaned = name.trimmed();
    if (cleaned.isEmpty())
        return QByteArray();

    // uBO priorities ("redirect=noop.js:99") and ABP priorities
    // ("redirect=noop.js:47") — everything after the last ':' that is
    // purely numeric is a priority hint, strip it.
    const int colon = cleaned.lastIndexOf(QLatin1Char(':'));
    if (colon != -1) {
        bool numeric = !cleaned.mid(colon + 1).isEmpty();
        for (const QChar c : cleaned.mid(colon + 1))
            numeric = numeric && c.isDigit();
        if (numeric)
            cleaned = cleaned.left(colon);
    }

    const QByteArray alias = aliasTable().value(cleaned);
    if (!alias.isEmpty())
        return alias;

    // Extension fallback for the long tail of named stubs.
    if (cleaned.endsWith(QLatin1String(".js")))
        return QByteArrayLiteral("noop.js");
    if (cleaned.endsWith(QLatin1String(".gif")))
        return QByteArrayLiteral("1x1.gif");
    if (cleaned.endsWith(QLatin1String(".png")))
        return QByteArrayLiteral("2x2.png");
    if (cleaned.endsWith(QLatin1String(".html"))
        || cleaned.endsWith(QLatin1String(".htm")))
        return QByteArrayLiteral("noop.html");
    if (cleaned.endsWith(QLatin1String(".css")))
        return QByteArrayLiteral("noop.css");
    if (cleaned.endsWith(QLatin1String(".txt")))
        return QByteArrayLiteral("noop.txt");

    return QByteArray();
}

QUrl AdBlockResourceHandler::urlForResource(const QByteArray &canonicalName)
{
    QUrl url;
    url.setScheme(QString::fromLatin1(schemeName()));
    url.setPath(QLatin1Char('/') + QString::fromLatin1(canonicalName));
    return url;
}

bool AdBlockResourceHandler::resourceFor(const QByteArray &canonicalName,
                                         QByteArray *mimeType,
                                         QByteArray *body)
{
    const StubResource resource = resourceTable().value(canonicalName);
    if (resource.mimeType.isEmpty())
        return false;
    *mimeType = resource.mimeType;
    *body = resource.body;
    return true;
}

QList<QByteArray> AdBlockResourceHandler::registrationNames()
{
    QList<QByteArray> names = resourceTable().keys();
    for (const QString &alias : aliasTable().keys())
        names.append(alias.toUtf8());
    return names;
}

void AdBlockResourceHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    QString path = job->requestUrl().path();
    if (path.startsWith(QLatin1Char('/')))
        path = path.mid(1);

    QByteArray mimeType, body;
    if (job->requestMethod() != "GET"
        || !resourceFor(path.toUtf8(), &mimeType, &body)) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // The job takes ownership of the buffer.
    QBuffer *buffer = new QBuffer;
    buffer->setData(body);
    buffer->open(QIODevice::ReadOnly);
    job->reply(mimeType, buffer);
}
