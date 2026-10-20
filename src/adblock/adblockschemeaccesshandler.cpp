/**
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright (c) 2009, Arora Browser
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

bool AdBlockSchemeAccessHandler::isSubscribeRequest(const QUrl &requestUrl)
{
    // Both abp:subscribe?... (path form) and abp://subscribe?...
    // (host form) appear in the wild.
    return requestUrl.path() == QLatin1String("subscribe")
        || requestUrl.host() == QLatin1String("subscribe")
        || requestUrl.path() == QLatin1String("//subscribe");
}

void AdBlockSchemeAccessHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (job->requestMethod() != "GET"
        || !isSubscribeRequest(job->requestUrl())) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }

    // requestStarted() runs on the IO thread; the subscription prompt
    // is GUI work, so it is queued onto the thread that owns this
    // handler (same pattern as FileAccessHandler).  The job is
    // QPointer-guarded at capture time — constructing the guard inside
    // handleSubscribe would already be too late if WebEngine aborted
    // and deleted the request meanwhile (TAB03).
    const QPointer<QWebEngineUrlRequestJob> jobGuard(job);
    QMetaObject::invokeMethod(this, [this, jobGuard]() {
        handleSubscribe(jobGuard);
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
