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
#include "adblocksubscription.h"
#include "adblockdialog.h"

#include <qmessagebox.h>
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
    if (job->requestMethod() != "GET"
        || job->requestUrl().path() != QLatin1String("subscribe")) {
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

    AdBlockSubscription *subscription = new AdBlockSubscription(job->requestUrl(), AdBlockManager::instance());

    // The subscription title arrives inside the abp:subscribe URL —
    // page-controlled, so render it literally.
    QMessageBox box(QMessageBox::Question, tr("Subscribe?"),
            tr("Subscribe to this AdBlock subscription?\n%1").arg(subscription->title()),
            QMessageBox::Yes | QMessageBox::No);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::No);
    const QMessageBox::StandardButton result =
        static_cast<QMessageBox::StandardButton>(box.exec());
    if (result == QMessageBox::No) {
        delete subscription;
    } else {
        // TELEM01: clicking Subscribe is consent to fetch the list —
        // the constructor's automatic update was consent-gated, so
        // kick the download here.
        AdBlockManager::setRemoteListsConsent(
            AdBlockManager::RemoteListsGranted);
        AdBlockManager::instance()->addSubscription(subscription);
        subscription->updateNow();
        AdBlockDialog *dialog = AdBlockManager::instance()->showDialog();
        dialog->selectSubscription(subscription);
        dialog->setFocus();
    }
    // Nothing is ever served for abp: urls — they only carry the
    // subscription parameters handled above.
    job->fail(QWebEngineUrlRequestJob::RequestAborted);
}
