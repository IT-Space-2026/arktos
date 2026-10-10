/*
 * Copyright 2026 The Arora Authors
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

#include "tlsverifier.h"

#include "browserapplication.h"
#include "privacyrequestinterceptor.h"

#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qnetworkproxy.h>
#include <qpointer.h>
#include <qthreadpool.h>

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"
#endif

TlsVerifier *TlsVerifier::instance()
{
    // Leaked like the other app-wide services — worker lambdas post
    // back through a QPointer, but the object must effectively outlive
    // every in-flight probe.
    static TlsVerifier *s_instance = new TlsVerifier(qApp);
    return s_instance;
}

TlsVerifier::TlsVerifier(QObject *parent)
    : QObject(parent)
{
}

QString TlsVerifier::keyFor(const QString &host, quint16 port)
{
    return host.toLower() + QLatin1Char(':') + QString::number(port);
}

void TlsVerifier::probeUrl(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https"))
        return;

    const QString host = url.host();
    const quint16 port = quint16(url.port(443));
    if (host.isEmpty())
        return;

    // Tor hard rule: the probe is a DIRECT TLS handshake from this
    // process — from a tor window it would bypass the managed SOCKS
    // and de-anonymize the user.  Same release-blocker class as
    // DLACC04's direct-download rule.
    if (BrowserApplication::isTorMode())
        return;

    // Any application proxy gets the same protection: the probe can
    // not ride SOCKS/HTTP-CONNECT, and a direct connection would leak
    // the browsing destination around the user's configured route.
    const QNetworkProxy::ProxyType proxy =
        QNetworkProxy::applicationProxy().type();
    if (proxy != QNetworkProxy::NoProxy
        && proxy != QNetworkProxy::DefaultProxy)
        return;

    // SSRF discipline (same list the interceptors enforce): never let
    // the probe reach into loopback/LAN/.onion space.
    if (PrivacyRequestInterceptor::isPrivateOrLocalHost(host))
        return;

    probeHost(host, port, 0);
}

void TlsVerifier::probeHost(const QString &host, quint16 port, quint32 flags)
{
#if !defined(ARORA_RUSTCORE)
    Q_UNUSED(host);
    Q_UNUSED(port);
    Q_UNUSED(flags);
    return;
#else
    if (host.isEmpty())
        return;
    const QString key = keyFor(host, port);
    if (m_entries.contains(key) || m_inflight.contains(key))
        return;

    Entry pending;
    pending.status = Status::Pending;
    m_entries.insert(key, pending);
    m_inflight.insert(key);

    QPointer<TlsVerifier> self(this);
    QThreadPool::globalInstance()->start(
        [self, key, host, port, flags]() {
            // Worker thread: rc_tls_check blocks up to ~15 s on the
            // real handshake.  Everything it returns is already
            // verdict-shaped JSON — the classes come straight through.
            int status = int(Status::Unverified);
            QString errorClass, detail, tlsVersion;

            char *json = rc_tls_check(host.toUtf8().constData(), port, flags);
            if (json) {
                const QJsonDocument doc = QJsonDocument::fromJson(
                    QByteArray(json));
                rc_string_free(json);
                const QJsonObject o = doc.object();
                const QString s = o.value(QLatin1String("status"))
                                      .toString();
                if (s == QLatin1String("verified"))
                    status = int(Status::Verified);
                else if (s == QLatin1String("warning"))
                    status = int(Status::Warning);
                else if (s == QLatin1String("refused"))
                    status = int(Status::Refused);
                else
                    status = int(Status::Unverified);
                errorClass = o.value(QLatin1String("error_class"))
                                 .toString();
                detail = o.value(QLatin1String("detail")).toString();
                tlsVersion = o.value(QLatin1String("negotiated"))
                                 .toObject()
                                 .value(QLatin1String("tls_version"))
                                 .toString();
            }
            // detail may carry a server-provided reason string — it is
            // stored for the chip's tooltip but never logged verbatim.

            if (self) {
                QMetaObject::invokeMethod(
                    self,
                    [self, key, status, errorClass, detail, tlsVersion]() {
                        self->record(key, status, errorClass, detail,
                                     tlsVersion);
                    },
                    Qt::QueuedConnection);
            } else {
                // The singleton outlives the app normally; this path
                // only exists to keep a destroyed-in-test object safe.
            }
        });
#endif
}

void TlsVerifier::record(const QString &key, int status,
                         const QString &errorClass,
                         const QString &detail,
                         const QString &tlsVersion)
{
    m_inflight.remove(key);
    Entry &entry = m_entries[key];
    entry.status = static_cast<Status>(status);
    entry.errorClass = errorClass;
    entry.detail = detail;
    entry.tlsVersion = tlsVersion;

    // Log host + class only (DLACC05 log hygiene): never the URL, the
    // chain, or the server's detail string.
    if (entry.status == Status::Warning)
        qDebug("TlsVerifier: %s warning %s", qPrintable(key),
               qPrintable(errorClass));
    else
        qDebug("TlsVerifier: %s %d", qPrintable(key), int(entry.status));

    emit statusChanged(key.section(QLatin1Char(':'), 0, 0));
}

TlsVerifier::Status TlsVerifier::statusFor(const QString &host,
                                         quint16 port) const
{
    return m_entries.value(keyFor(host, port)).status;
}

QString TlsVerifier::errorClass(const QString &host, quint16 port) const
{
    return m_entries.value(keyFor(host, port)).errorClass;
}

QString TlsVerifier::detail(const QString &host, quint16 port) const
{
    return m_entries.value(keyFor(host, port)).detail;
}

QString TlsVerifier::tlsVersion(const QString &host, quint16 port) const
{
    return m_entries.value(keyFor(host, port)).tlsVersion;
}
