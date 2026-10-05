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

#ifndef ADBLOCKNETWORK_H
#define ADBLOCKNETWORK_H

#include "adblockrule.h"

#include <qobject.h>
#include <qreadwritelock.h>

class QUrl;

/*
    Matches request URLs against the adblock network rules.

    Under Qt WebKit this class hooked into the application's
    QNetworkAccessManager and returned a blocked QNetworkReply.  Qt
    WebEngine does its own networking in the Chromium IO thread, so all
    request blocking now goes through the profile's
    QWebEngineUrlRequestInterceptor (AdBlockRequestInterceptor), which
    calls shouldBlock() from that thread.

    Because the interceptor runs off the GUI thread, shouldBlock() never
    touches the live AdBlockSubscription objects: rebuildRules() (GUI
    thread only) copies the enabled network rules into a snapshot that
    shouldBlock() walks under a read lock.
*/
class AdBlockNetwork : public QObject
{
    Q_OBJECT

public:
    AdBlockNetwork(QObject *parent = 0);

    // Thread-safe; called by the request interceptor on the IO thread.
    bool shouldBlock(const QUrl &url) const;

public slots:
    // Snapshots the current subscriptions' network rules.  GUI thread only.
    void rebuildRules();

private:
    struct SubscriptionRules {
        QList<AdBlockRule> exceptionRules;
        QList<AdBlockRule> blockRules;
    };

    QList<SubscriptionRules> m_subscriptions;
    bool m_enabled;
    mutable QReadWriteLock m_lock;
};

#endif // ADBLOCKNETWORK_H
