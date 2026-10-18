/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
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

#include "adblockschemeaccesshandler.h"

#include "adblockmanager.h"

#include <qwebengineurlrequestjob.h>
#include <qwebengineurlscheme.h>

AdBlockSchemeAccessHandler::AdBlockSchemeAccessHandler(QObject *parent)
    : SchemeAccessHandler(parent)
{
}

QByteArray AdBlockSchemeAccessHandler::scheme() const
{
    return schemeName();
}

QByteArray AdBlockSchemeAccessHandler::schemeName()
{
    return QByteArrayLiteral("abp");
}

void AdBlockSchemeAccessHandler::registerUrlScheme()
{
    QWebEngineUrlScheme scheme(schemeName());
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    QWebEngineUrlScheme::registerScheme(scheme);
}

void AdBlockSchemeAccessHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    // Both abp:subscribe?... (path form) and abp://subscribe?...
    // (host form) appear in the wild — reject only genuinely foreign
    // abp: urls.
    const QUrl requestUrl = job->requestUrl();
    const bool subscribe = requestUrl.path() == QLatin1String("subscribe")
        || requestUrl.host() == QLatin1String("subscribe")
        || requestUrl.path() == QLatin1String("//subscribe");
    if (job->requestMethod() != "GET" || !subscribe) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }

    // requestStarted() runs on the IO thread; the subscription prompt
    // is GUI work, so it is queued onto the thread that owns this
    // handler (same pattern as FileAccessHandler).
    QMetaObject::invokeMethod(this, [this, job]() {
        handleSubscribe(job);
    }, Qt::QueuedConnection);
}

void AdBlockSchemeAccessHandler::handleSubscribe(QPointer<QWebEngineUrlRequestJob> job)
{
    if (!job)
        return;

    // MPROF: this handler's parent is the persona's own manager — the
    // ambient instance() would add the subscription to whichever
    // profile last had focus, not the one that hosted the click.
    if (AdBlockManager *manager = qobject_cast<AdBlockManager *>(parent()))
        manager->promptSubscribe(job->requestUrl());

    // Nothing is ever served for abp: urls — they only carry the
    // subscription parameters handled above.
    job->fail(QWebEngineUrlRequestJob::RequestAborted);
}
