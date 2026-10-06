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

private:
    AdBlockNetwork *m_network;
};

#endif // ADBLOCKREQUESTINTERCEPTOR_H
