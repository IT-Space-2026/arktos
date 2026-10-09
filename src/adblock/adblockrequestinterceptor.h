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

#ifndef ADBLOCKREQUESTINTERCEPTOR_H
#define ADBLOCKREQUESTINTERCEPTOR_H

#include <qwebengineurlrequestinterceptor.h>

class AdBlockNetwork;
class QUrl;
class QWebEngineUrlRequestInfo;

// Profile-level request interceptor: the only request-blocking surface
// Qt WebEngine offers (there is no per-request page hook like WebKit's
// QNetworkAccessManager integration).  interceptRequest() is invoked on
// the WebEngine IO thread, so it only consults the lock-guarded rule
// snapshot in AdBlockNetwork and never touches live GUI state.
class AdBlockRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    AdBlockRequestInterceptor(AdBlockNetwork *network, QObject *parent = nullptr);

    virtual void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    // ADB05: session tally of requests the blocker stopped, keyed by
    // first-party host (the top-level page), so the location-bar
    // button can show a per-page blocked count.  Written on the IO
    // thread; both accessors take the internal lock.  Counts are
    // cumulative for the session — a caller wanting "this page load"
    // diffs against a baseline taken at load start (counts can mix
    // when two tabs share a host, which is an accepted approximation).
    static int blockedRequestCount(const QString &host);
    static void clearBlockedRequestCounts();  // test hook

    // STALL01: true only for schemes filter lists can legitimately
    // describe — http(s) pages/resources and ws(s) for $websocket
    // rules.  Everything else (devtools:, chrome:, qrc:, arora-file:,
    // arora-cert-error:, arora-http-warning:, arora-resource: — the
    // stub redirect targets — abp:, data:, about:, file:, blob:,
    // javascript: ...) must pass through interception untouched: a
    // filter that rewrites an internal request can break browser UI,
    // and an adblock redirect target matching the same rule again
    // spins the redirect chain.
    static bool isWebRequestScheme(const QString &scheme);

    // DLACC04: the $removeparam rewriter, shared with the rustdl
    // download gate — applies the specs to *url, true when the URL
    // changed.  Pure; safe on any thread.
    static bool stripQueryParams(QUrl *url, const QStringList &specs);

private:
    static void noteBlockedRequest(const QUrl &firstPartyUrl);

    AdBlockNetwork *m_network;
};

#endif // ADBLOCKREQUESTINTERCEPTOR_H
