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

#include "adblockrequestinterceptor.h"

#include "adblocknetwork.h"
#include "adblockresourcehandler.h"

#include <qregularexpression.h>
#include <qurlquery.h>
#include <qwebengineurlrequestinfo.h>

// #define ADBLOCKINTERCEPTOR_DEBUG

AdBlockRequestInterceptor::AdBlockRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_network(network)
{
}

// Applies the $removeparam specs to url; returns true when the URL
// changed.  Runs on the IO thread — keep it allocation-light.
static bool stripQueryParams(QUrl *url, const QStringList &specs)
{
    if (specs.isEmpty())
        return false;
    QUrlQuery query(*url);
    const QList<QPair<QString, QString> > items = query.queryItems();
    if (items.isEmpty())
        return false;

    QUrlQuery kept;
    bool removed = false;
    const bool stripAll = specs.contains(QLatin1String("*"));
    for (const QPair<QString, QString> &item : items) {
        bool strip = stripAll;
        if (!strip) {
            for (const QString &spec : specs) {
                if (spec.startsWith(QLatin1Char('/'))
                    && spec.endsWith(QLatin1Char('/'))) {
                    const QRegularExpression re(
                        spec.mid(1, spec.size() - 2));
                    if (re.isValid() && re.match(item.first).hasMatch())
                        strip = true;
                } else if (item.first == spec) {
                    strip = true;
                }
                if (strip)
                    break;
            }
        }
        if (strip)
            removed = true;
        else
            kept.addQueryItem(item.first, item.second);
    }
    if (!removed)
        return false;

    url->setQuery(kept);
    return true;
}

void AdBlockRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread.  info.block(true) fails the
    // request with net::ERR_BLOCKED_BY_CLIENT, the same net result the
    // old ContentAccessDenied QNetworkReply produced.
    const AdBlockDecision decision = m_network->match(
        info.requestUrl(), info.firstPartyUrl(), info.resourceType());

    switch (decision.action) {
    case AdBlockDecision::Allow:
        break;
    case AdBlockDecision::Redirect: {
        // adblock-rust decisions carry a literal URL (data: stub
        // payload or $removeparam-rewritten request URL); the native
        // matcher names a bundled resource instead.
        if (!decision.redirectUrl.isEmpty()) {
#if defined(ADBLOCKINTERCEPTOR_DEBUG)
            qDebug() << "AdBlockRequestInterceptor: redirect-url"
                     << info.requestUrl() << "->" << decision.redirectUrl;
#endif
            info.redirect(QUrl(decision.redirectUrl));
            return;
        }
        const QByteArray resource = AdBlockResourceHandler::canonicalResourceName(
            decision.redirectResource);
        if (resource.isEmpty()) {
            info.block(true); // unknown stub — plain block
        } else {
#if defined(ADBLOCKINTERCEPTOR_DEBUG)
            qDebug() << "AdBlockRequestInterceptor: redirect"
                     << info.requestUrl() << "->" << resource;
#endif
            info.redirect(AdBlockResourceHandler::urlForResource(resource));
        }
        return;
    }
    case AdBlockDecision::Block:
        info.block(true);
        return;
    }

    if (!decision.removeParams.isEmpty()) {
        QUrl url = info.requestUrl();
        if (stripQueryParams(&url, decision.removeParams)) {
#if defined(ADBLOCKINTERCEPTOR_DEBUG)
            qDebug() << "AdBlockRequestInterceptor: removeparam"
                     << info.requestUrl() << "->" << url;
#endif
            info.redirect(url);
        }
    }
}
