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

#ifndef ADBLOCKRESOURCEHANDLER_H
#define ADBLOCKRESOURCEHANDLER_H

#include <qbytearray.h>
#include <qwebengineurlschemehandler.h>

class QUrl;

/*
    Serves the bundled stub ("redirect") resources that adblock
    $redirect= / $redirect-rule= rules point at, on the
    arora-resource:// scheme.  The resource table is compiled in
    (generated programmatically for the images) so nothing on disk or
    in a filter list can influence what gets served.

    Resource names follow the ABP/uBO conventions; alias names such as
    "1x1-transparent-gif" or "noopjs" normalize to the canonical files.
*/
class AdBlockResourceHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    AdBlockResourceHandler(QObject *parent = 0);

    static QByteArray schemeName();
    static void registerUrlScheme();

    // Canonical resource name for a $redirect= option value, or an
    // empty QByteArray when nothing bundled covers it (the caller then
    // falls back to a plain block).
    static QByteArray canonicalResourceName(const QString &name);
    static QUrl urlForResource(const QByteArray &canonicalName);
    static bool resourceFor(const QByteArray &canonicalName,
                            QByteArray *mimeType, QByteArray *body);

    // Every spelling an external filter engine should know:
    // canonical stub names plus the alias spellings lists actually
    // use in $redirect= options (each resolves through
    // canonicalResourceName + resourceFor).
    static QList<QByteArray> registrationNames();

    void requestStarted(QWebEngineUrlRequestJob *job) Q_DECL_OVERRIDE;
};

#endif // ADBLOCKRESOURCEHANDLER_H
