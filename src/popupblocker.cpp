/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "popupblocker.h"
#include "profilesettings.h"

#include <qapplication.h>
#include <qsettings.h>
#include <qurl.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"
#include "sitedecisionstore.h"

#include <arktos_core.h>
#include <rustcore.h>

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <string.h>

// MD09: one granular call into rustcore's navpolicy module — the
// createWindow gate's decision layer (enabled flag, scheme floor,
// suffix-matched allow list, session overlay) lives there now; this
// class keeps the singleton, the changed() signal and the local
// consult as the FFI-failure fallback.  The default-bound AcPolicy
// is minted exactly like WebPermissionManager's permPolicyCall —
// pop-up exceptions are persona-independent (MPROF02); with no
// bridge serving yet the legacy rc_* spelling runs under the same
// explicit default anchor.  A NULL answer maps to an empty object —
// callers then run the local consult, so an FFI failure can never
// silently allow or block.
static QJsonObject navPolicyCall(const QJsonObject &request)
{
    AcPolicy *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->policy(QString());
    if (!h)
        rustCoreEnsureDataDirFor(QLatin1String("default"));
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = h
        ? ac_nav_call(h,
                      reinterpret_cast<const uint8_t *>(json.constData()),
                      size_t(json.size()))
        : rc_nav_call(
              reinterpret_cast<const uint8_t *>(json.constData()),
              size_t(json.size()));
    ac_policy_close(h);
    if (!raw)
        return QJsonObject();
    QByteArray out = QByteArray::fromRawData(raw, int(strlen(raw)));
    out.detach();
    rc_string_free(raw);
    return QJsonDocument::fromJson(out).object();
}
#endif

PopupBlocker::PopupBlocker(QObject *parent)
    : QObject(parent)
{
#if defined(ARKTOS_RUSTCORE)
    // SITED01: one-shot import of the legacy "popupExceptions" list —
    // replayed into the Rust store and retired after the writes land.
    // The verdict consults the store directly now, so the imported
    // rows need no in-memory hydration.
    ProfileSettings legacy;
    const QStringList legacyAllowed = legacy
        .value(QLatin1String("popupExceptions/allowed")).toStringList();
    if (!legacyAllowed.isEmpty()) {
        bool allWritten = true;
        for (const QString &host : legacyAllowed) {
            const QString normalized = normalizeHost(host);
            if (!normalized.isEmpty())
                allWritten = SiteDecisionStore::set(
                    SiteDecisionStore::KindPopup,
                    normalized, QLatin1String("allow")) && allWritten;
        }
        if (allWritten)
            legacy.remove(QLatin1String("popupExceptions/allowed"));
    }
#else
    const QStringList allowed = ProfileSettings()
        .value(QLatin1String("popupExceptions/allowed")).toStringList();
    for (const QString &host : allowed) {
        const QString normalized = normalizeHost(host);
        if (!normalized.isEmpty() && !m_allowed.contains(normalized))
            m_allowed.append(normalized);
    }
#endif
}

PopupBlocker *PopupBlocker::instance()
{
    static PopupBlocker *blocker = new PopupBlocker(qApp);
    return blocker;
}

bool PopupBlocker::isEnabled() const
{
    return ProfileSettings().value(QLatin1String("websettings/blockPopupWindows"),
                             true).toBool();
}

void PopupBlocker::setEnabled(bool enabled)
{
    ProfileSettings().setValue(QLatin1String("websettings/blockPopupWindows"),
                         enabled);
    emit changed();
}

QString PopupBlocker::normalizeHost(const QString &host)
{
    QString normalized = host.toLower();
    while (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    // A leading dot is meaningless in this store — the suffix match
    // already covers "example.com or any subdomain".
    while (normalized.startsWith(QLatin1Char('.')))
        normalized = normalized.mid(1);
    return normalized;
}

// CookieJar-style suffix matching: "example.com" governs
// "example.com" itself and every "www.example.com" under it.
bool PopupBlocker::matchesHost(const QStringList &list, const QString &host)
{
    for (const QString &rule : list) {
        if (host == rule || host.endsWith(QLatin1Char('.') + rule))
            return true;
    }
    return false;
}

bool PopupBlocker::matchesHost(const QSet<QString> &set, const QString &host)
{
    QString parent = host;
    for (;;) {
        if (set.contains(parent))
            return true;
        const int dot = parent.indexOf(QLatin1Char('.'));
        if (dot < 0)
            return false;
        parent = parent.mid(dot + 1);
    }
}

bool PopupBlocker::shouldBlock(const QUrl &openerUrl) const
{
#if defined(ARKTOS_RUSTCORE)
    // MD09: the whole gate — enabled flag, scheme floor, the
    // suffix-matched allow list — is one navpolicy consult; the
    // consult is the popup-shaped one (non-shaped requests never
    // reach this function, same as before).  An "ask" verdict maps
    // to the same dead-end a block does — the indicator's
    // open-once flow IS the ask UI.  An FFI failure falls through
    // to the local consult.
    const QJsonObject verdict = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_decide")},
        {QLatin1String("url"),
         QString::fromUtf8(openerUrl.toEncoded())},
        {QLatin1String("shaped"), true},
        {QLatin1String("enabled"), isEnabled()},
    });
    if (!verdict.isEmpty()) {
        const QString action =
            verdict.value(QLatin1String("action")).toString();
        return action == QLatin1String("block")
            || action == QLatin1String("ask");
    }
#endif
    if (!isEnabled())
        return false;
    const QString scheme = openerUrl.scheme();
    if (scheme != QLatin1String("http")
        && scheme != QLatin1String("https"))
        return false;
    return !isAllowed(openerUrl);
}

bool PopupBlocker::isAllowed(const QUrl &openerUrl) const
{
    return isAllowedHost(openerUrl.host());
}

bool PopupBlocker::isAllowedHost(const QString &host) const
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return false;
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_is_allowed")},
        {QLatin1String("host"), normalized},
    });
    if (r.contains(QLatin1String("allowed")))
        return r.value(QLatin1String("allowed")).toBool();
    // FFI-failure fallback: the local session overlay plus the
    // persisted rows consulted directly.
    if (matchesHost(m_sessionAllowed, normalized))
        return true;
    QString value;
    return SiteDecisionStore::lookup(SiteDecisionStore::KindPopup,
                                     normalized, &value);
#else
    return matchesHost(m_sessionAllowed, normalized)
        || matchesHost(m_allowed, normalized);
#endif
}

void PopupBlocker::allowHost(const QString &host, bool persistent)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_allow")},
        {QLatin1String("host"), normalized},
        {QLatin1String("persistent"), persistent},
    });
    if (r.isEmpty()) {
        // FFI-failure fallback: persistent rows write the store;
        // session rules keep the local overlay.
        if (persistent)
            SiteDecisionStore::set(SiteDecisionStore::KindPopup,
                                   normalized, QLatin1String("allow"));
        else
            m_sessionAllowed.insert(normalized);
    }
#else
    if (persistent) {
        if (!m_allowed.contains(normalized)) {
            m_allowed.append(normalized);
            save();
        }
    } else {
        m_sessionAllowed.insert(normalized);
    }
#endif
    emit changed();
}

void PopupBlocker::removeAllowedHost(const QString &host)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_remove")},
        {QLatin1String("host"), normalized},
    });
    if (!r.isEmpty()) {
        if (r.value(QLatin1String("removed_persistent")).toBool()
            || r.value(QLatin1String("removed_session")).toBool())
            emit changed();
        return;
    }
    // FFI-failure fallback.
    const bool removedPersistent =
        SiteDecisionStore::remove(SiteDecisionStore::KindPopup,
                                  normalized);
    const bool removedSession = m_sessionAllowed.remove(normalized);
    if (removedPersistent || removedSession)
        emit changed();
#else
    const bool removedPersistent = m_allowed.removeAll(normalized) > 0;
    const bool removedSession = m_sessionAllowed.remove(normalized);
    if (removedPersistent)
        save();
    if (removedPersistent || removedSession)
        emit changed();
#endif
}

QStringList PopupBlocker::allowedHosts() const
{
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_list")},
    });
    if (r.contains(QLatin1String("hosts"))) {
        QStringList hosts;
        const QJsonArray rows =
            r.value(QLatin1String("hosts")).toArray();
        for (const QJsonValue &row : rows)
            hosts.append(row.toString());
        return hosts;
    }
    // FFI-failure fallback: the persisted keys, sorted like the
    // facade's answer.
    QStringList hosts =
        SiteDecisionStore::entries(SiteDecisionStore::KindPopup).keys();
    hosts.sort();
    return hosts;
#else
    return m_allowed;
#endif
}

void PopupBlocker::clearAllowedHosts()
{
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_clear")},
    });
    if (!r.isEmpty()) {
        if (r.value(QLatin1String("cleared")).toBool())
            emit changed();
        return;
    }
    // FFI-failure fallback.
    if (SiteDecisionStore::clear(SiteDecisionStore::KindPopup))
        emit changed();
#else
    if (m_allowed.isEmpty())
        return;
    m_allowed.clear();
    save();
    emit changed();
#endif
}

void PopupBlocker::clearSessionHosts()
{
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = navPolicyCall({
        {QLatin1String("op"), QLatin1String("popup_clear_session")},
    });
    if (!r.isEmpty()) {
        if (r.value(QLatin1String("cleared")).toBool())
            emit changed();
        return;
    }
#endif
    if (m_sessionAllowed.isEmpty())
        return;
    m_sessionAllowed.clear();
    emit changed();
}

void PopupBlocker::save() const
{
    // No-rust builds only — under ARKTOS_RUSTCORE the mutators write
    // single rows through SiteDecisionStore or the facade's ops and
    // never call this; the in-memory list is never authoritative.
#if !defined(ARKTOS_RUSTCORE)
    ProfileSettings settings;
    if (m_allowed.isEmpty())
        settings.remove(QLatin1String("popupExceptions/allowed"));
    else
        settings.setValue(QLatin1String("popupExceptions/allowed"),
                          m_allowed);
#endif
}
