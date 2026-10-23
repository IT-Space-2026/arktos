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

#include "webpermissionmanager.h"
#include "profilesettings.h"

#include <qapplication.h>
#include <qcheckbox.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qmetaobject.h>
#include <qsettings.h>
#include <qwebenginepage.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"
#include "sitedecisionstore.h"

#include <arktos_core.h>

#include <qjsondocument.h>
#include <qjsonobject.h>

#include <string.h>

// MD03: one granular call into rustcore's permpolicy module — the
// broker's decision layer (feature tables, origin-key normalization,
// remembered + session verdict pipeline) lives there now; this
// manager keeps the modal queue and the engine resolves.  The
// default-bound AcPolicy is minted exactly like SiteDecisionStore's
// PolicyAccess (permission decisions are persona-independent,
// MPROF02); with no bridge serving yet the legacy rc_* spelling runs
// under the same explicit default anchor.  A NULL verdict maps to an
// empty object — callers then run the local consult, so an FFI
// failure can never silently grant or deny.
static QJsonObject permPolicyCall(const QJsonObject &request)
{
    AcPolicy *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->policy(QString());
    if (!h)
        rustCoreEnsureDataDirFor(QLatin1String("default"));
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = h
        ? ac_perm_call(h,
                       reinterpret_cast<const uint8_t *>(json.constData()),
                       size_t(json.size()))
        : rc_perm_call(
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

// SITED01: one-shot import of the legacy "webpermissions" QSettings
// group.  Every origin/type row is replayed into the Rust store; the
// group is retired only after the writes land, so an interrupted run
// re-imports on the next start.  Malformed rows are dropped rather
// than migrated — no valid decision is ever lost.
static void importLegacyWebPermissions()
{
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    const QStringList origins = settings.childGroups();
    if (origins.isEmpty()) {
        settings.endGroup();
        return;
    }
    bool allWritten = true;
    for (const QString &originKey : origins) {
        settings.beginGroup(originKey);
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            const QString value = settings.value(key).toString();
            if (value != QLatin1String("grant")
                && value != QLatin1String("deny"))
                continue;
            allWritten = SiteDecisionStore::set(
                SiteDecisionStore::KindWebPermission,
                originKey + QLatin1Char('/') + key, value) && allWritten;
        }
        settings.endGroup();
    }
    settings.endGroup();
    if (allWritten)
        settings.remove(QLatin1String("webpermissions"));
}
#endif

WebPermissionManager::WebPermissionManager(QObject *parent)
    : QObject(parent)
    , m_inPrompt(false)
{
#if defined(ARKTOS_RUSTCORE)
    importLegacyWebPermissions();
#endif
}

WebPermissionManager *WebPermissionManager::instance()
{
    static WebPermissionManager *manager = nullptr;
    if (!manager)
        manager = new WebPermissionManager(qApp);
    return manager;
}

// Capture hardware, desktop/screen capture, clipboard reads and font
// enumeration never reach the user: they are denied before the store
// or the prompt is consulted.
bool WebPermissionManager::isDenied(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
    case QWebEnginePermission::PermissionType::Unsupported:
        return true;
    default:
        return false;
    }
}

bool WebPermissionManager::isPromptable(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::Notifications:
    case QWebEnginePermission::PermissionType::Geolocation:
    case QWebEnginePermission::PermissionType::MouseLock:
        return true;
    default:
        return false;
    }
}

static QMetaEnum permissionTypeEnum()
{
    return QWebEnginePermission::staticMetaObject.enumerator(
        QWebEnginePermission::staticMetaObject.indexOfEnumerator("PermissionType"));
}

QString WebPermissionManager::typeName(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
        return tr("Audio capture (microphone)");
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
        return tr("Video capture (camera)");
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
        return tr("Audio and video capture");
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
        return tr("Screen capture");
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
        return tr("Screen and audio capture");
    case QWebEnginePermission::PermissionType::MouseLock:
        return tr("Mouse lock");
    case QWebEnginePermission::PermissionType::Notifications:
        return tr("Notifications");
    case QWebEnginePermission::PermissionType::Geolocation:
        return tr("Geolocation");
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
        return tr("Clipboard read/write");
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
        return tr("Local fonts");
    case QWebEnginePermission::PermissionType::Unsupported:
        break;
    }
    return tr("Unknown feature");
}

QString WebPermissionManager::typeDescription(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::Notifications:
        return tr("show notifications");
    case QWebEnginePermission::PermissionType::Geolocation:
        return tr("know your location");
    case QWebEnginePermission::PermissionType::MouseLock:
        return tr("take control of your mouse pointer");
    default:
        return typeName(type);
    }
}

// QSettings INI keys would split on '/', so the security origin is
// percent-encoded whole ("https%3A%2F%2Fexample.com") under the
// "webpermissions" group; each feature is a key below it.  The origin
// is normalized to bare scheme://host:port first — a permission is
// per security origin, so path/query/credentials must not split one
// site's decisions across multiple keys.
QString WebPermissionManager::keyFor(const QUrl &origin,
        QWebEnginePermission::PermissionType type)
{
    const QMetaEnum metaEnum = permissionTypeEnum();
    const char *key = metaEnum.valueToKey(static_cast<int>(type));
    const QString typeName = QLatin1String(key ? key : "Unsupported");
#if defined(ARKTOS_RUSTCORE)
    // MD03: the normalization lives in permpolicy now — every write
    // path must key identically to the store consults, so the facade
    // owns the spelling.  An FFI hiccup falls through to the local
    // computation, which produces the same bytes by construction.
    const QString derived = permPolicyCall({
        {QLatin1String("op"), QLatin1String("key_for")},
        {QLatin1String("origin"), QString::fromUtf8(origin.toEncoded())},
        {QLatin1String("type"), typeName},
    }).value(QLatin1String("key")).toString();
    if (!derived.isEmpty())
        return derived;
#endif
    QUrl o = origin;
    o.setUserInfo(QString());
    o.setPath(QString());
    o.setQuery(QString());
    o.setFragment(QString());
    return QString::fromUtf8(o.toEncoded().toPercentEncoding())
        + QLatin1Char('/') + typeName;
}

QList<WebPermissionManager::Entry> WebPermissionManager::entries() const
{
    QList<Entry> result;
    const QMetaEnum metaEnum = permissionTypeEnum();
#if defined(ARKTOS_RUSTCORE)
    // Keys round-trip as "<enc-origin>/<Type>" rows in the Rust store.
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindWebPermission);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const int slash = it.key().lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            continue;
        const QString originKey = it.key().left(slash);
        const QString typeKey = it.key().mid(slash + 1);
        int value = metaEnum.keyToValue(typeKey.toUtf8().constData());
        if (value == -1)
            continue;
        Entry entry;
        entry.origin = QUrl::fromEncoded(
            QByteArray::fromPercentEncoding(originKey.toUtf8()));
        entry.type = static_cast<QWebEnginePermission::PermissionType>(value);
        entry.granted = it.value() == QLatin1String("grant");
        result.append(entry);
    }
#else
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    const QStringList origins = settings.childGroups();
    for (const QString &originKey : origins) {
        settings.beginGroup(originKey);
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            int value = metaEnum.keyToValue(key.toUtf8().constData());
            if (value == -1)
                continue;
            Entry entry;
            entry.origin = QUrl::fromEncoded(
                QByteArray::fromPercentEncoding(originKey.toUtf8()));
            entry.type = static_cast<QWebEnginePermission::PermissionType>(value);
            entry.granted = settings.value(key).toString() == QLatin1String("grant");
            result.append(entry);
        }
        settings.endGroup();
    }
#endif
    return result;
}

void WebPermissionManager::setEntry(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool granted)
{
#if defined(ARKTOS_RUSTCORE)
    SiteDecisionStore::set(SiteDecisionStore::KindWebPermission,
                           keyFor(origin, type),
                           QLatin1String(granted ? "grant" : "deny"));
#else
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    settings.setValue(keyFor(origin, type),
                      QLatin1String(granted ? "grant" : "deny"));
    settings.endGroup();
#endif
    emit changed();
}

void WebPermissionManager::removeEntry(const QUrl &origin,
        QWebEnginePermission::PermissionType type)
{
#if defined(ARKTOS_RUSTCORE)
    // The facade removes the persistent row AND both session
    // variants in one op; the local path is the FFI-failure
    // fallback.
    const QMetaEnum metaEnum = permissionTypeEnum();
    const char *key = metaEnum.valueToKey(static_cast<int>(type));
    const QJsonObject r = permPolicyCall({
        {QLatin1String("op"), QLatin1String("remove")},
        {QLatin1String("origin"), QString::fromUtf8(origin.toEncoded())},
        {QLatin1String("type"), QLatin1String(key ? key : "Unsupported")},
    });
    if (r.isEmpty()) {
        SiteDecisionStore::remove(SiteDecisionStore::KindWebPermission,
                                  keyFor(origin, type));
        m_sessionDecisions.remove(sessionKeyFor(origin, type, false));
        m_sessionDecisions.remove(sessionKeyFor(origin, type, true));
    }
#else
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    settings.remove(keyFor(origin, type));
    settings.endGroup();
    m_sessionDecisions.remove(sessionKeyFor(origin, type, false));
    m_sessionDecisions.remove(sessionKeyFor(origin, type, true));
#endif
    emit changed();
}

void WebPermissionManager::clearEntries()
{
#if defined(ARKTOS_RUSTCORE)
    // "clear" empties the kind and the whole session map behind the
    // facade; the local pair is the FFI-failure fallback.
    const QJsonObject r = permPolicyCall({
        {QLatin1String("op"), QLatin1String("clear")},
    });
    if (r.isEmpty()) {
        SiteDecisionStore::clear(SiteDecisionStore::KindWebPermission);
        m_sessionDecisions.clear();
    }
#else
    ProfileSettings settings;
    settings.remove(QLatin1String("webpermissions"));
    m_sessionDecisions.clear();
#endif
    emit changed();
}

QString WebPermissionManager::sessionKeyFor(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool offTheRecord)
{
    return QLatin1String(offTheRecord ? "otr/" : "")
        + keyFor(origin, type);
}

bool WebPermissionManager::storedDecision(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool *granted,
        bool offTheRecord) const
{
#if defined(ARKTOS_RUSTCORE)
    // MD03: persistent-row-then-session consult lives in permpolicy —
    // the facade answer is authoritative; the local reads below are
    // the FFI-failure fallback.
    const QMetaEnum metaEnum = permissionTypeEnum();
    const char *typeKey = metaEnum.valueToKey(static_cast<int>(type));
    const QJsonObject r = permPolicyCall({
        {QLatin1String("op"), QLatin1String("stored")},
        {QLatin1String("origin"), QString::fromUtf8(origin.toEncoded())},
        {QLatin1String("type"), QLatin1String(typeKey ? typeKey : "Unsupported")},
        {QLatin1String("otr"), offTheRecord},
    });
    if (!r.isEmpty()) {
        if (!r.value(QLatin1String("found")).toBool())
            return false;
        *granted = r.value(QLatin1String("granted")).toBool();
        return true;
    }
    const QString key = keyFor(origin, type);
    QString stored;
    if (SiteDecisionStore::get(SiteDecisionStore::KindWebPermission,
                               key, &stored)) {
        *granted = stored == QLatin1String("grant");
        return true;
    }
#else
    const QString key = keyFor(origin, type);
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    if (settings.contains(key)) {
        *granted = settings.value(key).toString() == QLatin1String("grant");
        return true;
    }
#endif
    const QString sessionKey = sessionKeyFor(origin, type, offTheRecord);
    if (m_sessionDecisions.contains(sessionKey)) {
        *granted = m_sessionDecisions.value(sessionKey);
        return true;
    }
    return false;
}

void WebPermissionManager::handleRequest(QWidget *parent,
        const QWebEnginePermission &request, bool offTheRecord)
{
    if (!request.isValid())
        return;
    // WK19: the engine-neutral overload carries the real machinery —
    // the WebEngine carrier unwraps to origin+type plus a resolver
    // that re-checks isValid before granting (a request can die while
    // it queues behind a prompt).
    handleRequest(parent, request.origin(), request.permissionType(),
        offTheRecord, [request](bool granted) {
            if (!request.isValid())
                return;
            if (granted)
                request.grant();
            else
                request.deny();
        });
}

void WebPermissionManager::handleRequest(QWidget *parent,
        const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool offTheRecord,
        const std::function<void(bool)> &resolve)
{
#if defined(ARKTOS_RUSTCORE)
    // MD03: the whole verdict pipeline — feature gate, remembered
    // row, session answer — is one permpolicy consult.  Only "ask"
    // reaches the prompt queue below; an FFI failure falls through
    // to the same gates spelled locally (the feature table is a
    // compiled-in security boundary in both places).
    const QMetaEnum metaEnum = permissionTypeEnum();
    const char *typeKey = metaEnum.valueToKey(static_cast<int>(type));
    const QJsonObject verdict = permPolicyCall({
        {QLatin1String("op"), QLatin1String("decide")},
        {QLatin1String("origin"), QString::fromUtf8(origin.toEncoded())},
        {QLatin1String("type"), QLatin1String(typeKey ? typeKey : "Unsupported")},
        {QLatin1String("otr"), offTheRecord},
    });
    if (!verdict.isEmpty()) {
        const QString action =
            verdict.value(QLatin1String("action")).toString();
        if (action == QLatin1String("deny")
            || action == QLatin1String("allow")) {
            resolve(action == QLatin1String("allow"));
            return;
        }
    } else
#endif
    {
        // Never prompt for the dangerous surface: capture devices,
        // screen capture, clipboard, font enumeration and anything
        // unmapped.
        if (isDenied(type) || !isPromptable(type)) {
            resolve(false);
            return;
        }

        bool granted = false;
        if (storedDecision(origin, type, &granted, offTheRecord)) {
            resolve(granted);
            return;
        }
    }

    Pending pending;
    pending.parent = parent;
    pending.origin = origin;
    pending.type = type;
    pending.resolve = resolve;
    pending.offTheRecord = offTheRecord;
    m_pending.append(pending);
    QMetaObject::invokeMethod(this, "showNextPrompt", Qt::QueuedConnection);
}

// Prompts are serialized one modal at a time across every page so a
// redirect chain cannot stack dialogs (same guard as SEC02's external
// protocol prompt).
void WebPermissionManager::showNextPrompt()
{
    // The modal exec() below runs queued slot invocations, so this can
    // be re-entered while a prompt is open; the outer loop drains the
    // queue and stacked dialogs are never shown.
    if (m_inPrompt)
        return;
    m_inPrompt = true;
    while (!m_pending.isEmpty()) {
        const Pending pending = m_pending.takeFirst();

        // The answer may have been stored while this request queued
        // behind an earlier prompt.
        bool granted = false;
        if (storedDecision(pending.origin, pending.type, &granted,
                           pending.offTheRecord)) {
            pending.resolve(granted);
            continue;
        }

        emit promptRequested(pending.origin, pending.type);

        // The percent-encoded origin keeps control characters and
        // embedded newlines from spoofing the dialog text (SEC02).
        QString shownOrigin = QString::fromUtf8(pending.origin.toEncoded());
        if (shownOrigin.size() > 128)
            shownOrigin = shownOrigin.left(128) + QLatin1String("…");

        QMessageBox box(QMessageBox::Question, tr("Permission Request"),
            tr("The page at %1 wants to %2.\n\nAllow it?")
                .arg(shownOrigin, typeDescription(pending.type)),
            QMessageBox::NoButton, pending.parent);
        QAbstractButton *denyButton =
            box.addButton(QMessageBox::No);
        denyButton->setText(tr("Deny"));
        QAbstractButton *allowButton =
            box.addButton(QMessageBox::Yes);
        allowButton->setText(tr("Allow"));
        box.setDefaultButton(QMessageBox::No);
        QCheckBox *remember =
            new QCheckBox(tr("Remember this decision"), &box);
        remember->setChecked(true);
        box.setCheckBox(remember);
        box.exec();

        const bool allow = box.clickedButton() == allowButton;
        pending.resolve(allow);

        // Always remember for the session — without it a page looping
        // requestPermission() would spam modals.  Persisting is opt-in
        // via the checkbox and never happens for off-the-record pages.
#if defined(ARKTOS_RUSTCORE)
        const QMetaEnum metaEnum = permissionTypeEnum();
        const char *typeKey =
            metaEnum.valueToKey(static_cast<int>(pending.type));
        const QJsonObject rec = permPolicyCall({
            {QLatin1String("op"), QLatin1String("resolve")},
            {QLatin1String("origin"),
             QString::fromUtf8(pending.origin.toEncoded())},
            {QLatin1String("type"),
             QLatin1String(typeKey ? typeKey : "Unsupported")},
            {QLatin1String("granted"), allow},
            {QLatin1String("remember"), remember->isChecked()},
            {QLatin1String("otr"), pending.offTheRecord},
        });
        if (!rec.isEmpty()) {
            if (rec.value(QLatin1String("persisted")).toBool())
                emit changed();
        } else
#endif
        {
            // FFI failure (or a no-rust build): record locally, same
            // rules — session always, persistent on opt-in.
            m_sessionDecisions.insert(
                sessionKeyFor(pending.origin, pending.type,
                              pending.offTheRecord),
                allow);
            if (remember->isChecked() && !pending.offTheRecord)
                setEntry(pending.origin, pending.type, allow);
        }
    }
    m_inPrompt = false;
}
