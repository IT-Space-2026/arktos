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

#ifndef TLSVERIFIER_H
#define TLSVERIFIER_H

#include <qhash.h>
#include <qobject.h>
#include <qset.h>
#include <qurl.h>

// SEC22: the Qt face of rustcore's tlsprobe module — a second-opinion
// TLS chain check that runs on a worker thread and informs the site
// panel; it NEVER gates navigation.
//
// WebPage calls probeUrl() on each https main-frame commit; the
// verdict lands in a per-(host,port) session cache and statusChanged
// wakes the SitePanel chip.  The probe is a real handshake performed
// by the rustls+webpki stack in src/rustcore, independent of whatever
// Chromium already decided — on WebEngine it is additive hardening,
// and on a future engine without a cert-error delegate (the ENG03
// libservo spike found none) the same status/error-class pair is what
// the ENG05 cert interstitial will consume.
//
// Honesty rules mirrored from tlsprobe.rs:
//   * a probe failure (timeout/refused/protocol) records Unverified —
//     NEVER Warning: a network hiccup is not a bad chain;
//   * tor-mode pages are never probed and no probe is issued while an
//     application proxy is set — the probe is a direct connection and
//     bypassing SOCKS/HTTP-proxy would de-anonymize the user;
//   * private/loopback/LAN hosts are never probed (same SSRF rule the
//     interceptors and DLACC04 apply).
//
// Without CONFIG+=rustcore every call is an inert no-op — statuses
// stay None and the chip hides.
class TlsVerifier : public QObject
{
    Q_OBJECT

public:
    // Mirrors the verdict vocabulary in rustcore.h.
    enum class Status {
        None,       // never probed — gated or rustcore absent
        Pending,    // worker in flight
        Verified,   // handshake done, chain validated
        Warning,    // chain evaluated and FAILED — errorClass() says why
        Unverified, // chain never evaluated (network/protocol/roots)
        Refused,    // probe declined (private host, invalid args)
    };

    static TlsVerifier *instance();

    // The navigation-commit hook.  Applies the scheme/tor/proxy/
    // private-host gates, then starts the worker — a no-op for
    // non-https or already-covered (host,port) pairs.
    void probeUrl(const QUrl &url);

    // The site-panel view of the last verdict for (host,port).
    Status statusFor(const QString &host, quint16 port = 443) const;
    QString errorClass(const QString &host, quint16 port = 443) const;
    QString detail(const QString &host, quint16 port = 443) const;
    QString tlsVersion(const QString &host, quint16 port = 443) const;

    // The post-gate entry point probeUrl() delegates to — public so
    // autotests can pass RC_TLS_F_ALLOW_LOCAL for loopback fixtures
    // (production callers go through probeUrl()'s gates).
    void probeHost(const QString &host, quint16 port, quint32 flags);

signals:
    void statusChanged(const QString &host);

private slots:
    void record(const QString &key, int status,
                const QString &errorClass, const QString &detail,
                const QString &tlsVersion);

private:
    struct Entry {
        Status status = Status::None;
        QString errorClass;
        QString detail;
        QString tlsVersion;
    };

    explicit TlsVerifier(QObject *parent = nullptr);
    static QString keyFor(const QString &host, quint16 port);

    QHash<QString, Entry> m_entries;
    QSet<QString> m_inflight;
};

#endif // TLSVERIFIER_H
