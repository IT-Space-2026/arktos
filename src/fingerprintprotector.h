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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef FINGERPRINTPROTECTOR_H
#define FINGERPRINTPROTECTOR_H

#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

class QWebEngineProfile;

// SAFE06: anti-fingerprinting script injection.
//
// The countermeasures themselves live in src/data/fingerprint.js and
// are installed on each profile as a named QWebEngineScript by
// BrowserProfile::installFingerprintProtection — canvas readout gets
// per-session seeded noise, WebGL reports a generic vendor/renderer
// and navigator.hardwareConcurrency/deviceMemory/plugins normalize
// to cohort-common values.
//
// This class owns the C++ side of the feature:
//
//   - the privacy/fingerprintProtection toggle (default OFF — the
//     spoofing is detectable and canvas-heavy sites can break, so it
//     is opt-in; the tor profile gets it unconditionally),
//   - the per-site exemption list.  Persistent exemptions live in
//     privacy/fingerprintExceptions (normalized lowercase hosts,
//     CookieJar-style suffix matching); session exemptions are
//     memory-only and the only kind an off-the-record page may
//     record (same discipline as SEC05/SAFE01),
//   - scriptSource(), which bakes the session noise seed and the
//     exemption list into the JS payload.  The list travels inside
//     the injected source because the document-side exemption check
//     has to run before any page script does — the C++ side cannot
//     be consulted at DocumentCreation.
//
// After a toggle or an exemption change the installed script is
// stale; reinstallOnProfiles() re-pushes it onto every materialized
// profile so the next navigation picks the new source up.
class FingerprintProtector : public QObject
{
    Q_OBJECT

public:
    // Lazy qApp-owned singleton (HistoryManager pattern).
    static FingerprintProtector *instance();

    // The user toggle — privacy/fingerprintProtection, default off.
    bool isEnabled() const;

    // True when the feature is active for pages on this profile:
    // the toggle, or unconditionally on the tor profile.
    bool isActiveForProfile(QWebEngineProfile *profile) const;

    // Per-site exemption lookup (session + persistent, suffix match —
    // an exemption on "example.com" covers "www.example.com").
    bool isExempt(const QString &host) const;
    QStringList exceptionHosts() const;
    void addException(const QString &host, bool persistent);
    void clearException(const QString &host);

    // fingerprint.js with the seed + exemption list filled in, or
    // empty when the resource is missing.
    QString scriptSource() const;

    // Re-installs the named script on every profile that has been
    // brought up (normal, private-if-created, tor-if-created and the
    // materialized container profiles) so a toggle or exemption
    // change reaches the next navigation.
    void reinstallOnProfiles();

signals:
    void changed();

private:
    explicit FingerprintProtector(QObject *parent = nullptr);

    static QString normalizeHost(QString host);

    QStringList m_persistent;
    QSet<QString> m_session;
    quint32 m_seed;
};

#endif // FINGERPRINTPROTECTOR_H
