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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "rustcorebridge.h"

#include "browserpaths.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qsettings.h>
#include <qthread.h>

#include <arktos_core.h>

namespace {

// Set once the leaky singleton exists — exists() reads it so a
// non-GUI caller can tell minting is already done (instance() would
// otherwise pin the QObject's affinity to the wrong thread).
bool s_bridgeCreated = false;

} // namespace

RustCoreBridge *RustCoreBridge::instance()
{
    static RustCoreBridge *self = new RustCoreBridge;
    s_bridgeCreated = true;
    return self;
}

bool RustCoreBridge::exists()
{
    return s_bridgeCreated;
}

RustCoreBridge *RustCoreBridge::serving()
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return nullptr;
    if (!exists() && QThread::currentThread() != app->thread())
        return nullptr;
    RustCoreBridge *bridge = instance();
    if (!bridge->core())
        return nullptr;
    // A late root move (the test harness renames the application
    // after BrowserApplication already ran) strands every handle the
    // core minted — they re-assert the OLD root per op.  Refuse so
    // callers take the rc_* lane, which re-anchors per call.
    if (bridge->m_root != BrowserPaths::dataRoot())
        return nullptr;
    return bridge;
}

bool RustCoreBridge::rootDrifted()
{
    if (!exists())
        return false;
    return instance()->m_root != BrowserPaths::dataRoot();
}

RustCoreBridge::RustCoreBridge(QObject *parent)
    : QObject(parent)
{
    rc_set_change_callback(&RustCoreBridge::trampoline, this);

    // RC07: the app-lifetime facade root.  Bound once to the app
    // data root — the accessors re-assert profile bindings per call,
    // so the handle stays correct across profile switches without
    // the rustCoreEnsureDataDirFor dance.  m_root records the binding
    // so serving() can tell a late root move (test-mode renames)
    // from a stable one.
    m_root = BrowserPaths::dataRoot();
    const QByteArray root = m_root.toUtf8();
    // Same default-name seeding as RustCoreProfiles: the PROF01-era
    // QSettings key first, the localized fallback after.
    QByteArray seed = QSettings()
        .value(QLatin1String("profile/name")).toString().toUtf8();
    if (seed.isEmpty())
        seed = QCoreApplication::translate("ProfileRegistry", "Profile")
            .toUtf8();
    AcConfig cfg;
    cfg.struct_size = sizeof(cfg);
    cfg.api_version = AC_API_VERSION;
    cfg.data_root = root.constData();
    cfg.app_name = "arktos";
    cfg.default_profile_name = seed.constData();
    m_core = ac_init(&cfg);
    if (m_core != nullptr) {
        ac_set_change_callback(m_core, &RustCoreBridge::coreTrampoline,
                               this);
    } else {
        char *err = ac_last_error();
        qWarning() << "RustCoreBridge: ac_init failed:"
                   << QString::fromUtf8(err ? err : "unknown");
        ac_string_free(err);
    }
}

void RustCoreBridge::trampoline(const char *topic, void *userdata)
{
    RustCoreBridge *self = static_cast<RustCoreBridge *>(userdata);
    const QString t = QString::fromUtf8(topic ? topic : "");
    // Rust may call from any thread; hop to the object's thread.
    QMetaObject::invokeMethod(self, [self, t]() {
        emit self->storeChanged(t);
    }, Qt::QueuedConnection);
}

void RustCoreBridge::coreTrampoline(const char *module,
                                    const char *profileId,
                                    void *userdata)
{
    RustCoreBridge *self = static_cast<RustCoreBridge *>(userdata);
    const QString t = QString::fromUtf8(module ? module : "");
    const QString p = QString::fromUtf8(profileId ? profileId : "");
    // Same any-thread hop as trampoline; only the scoped signal
    // fires — storeChanged stays the legacy callback's alone so
    // listeners don't see double traffic.
    QMetaObject::invokeMethod(self, [self, t, p]() {
        emit self->storeChangedFor(t, p);
    }, Qt::QueuedConnection);
}

RcSettings *RustCoreBridge::settings(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_settings(m_core,
                       id.isEmpty() ? nullptr : id.constData());
}

RcProfiles *RustCoreBridge::profiles()
{
    if (m_core == nullptr)
        return nullptr;
    return ac_profiles(m_core);
}

AcHistory *RustCoreBridge::history(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_history(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcBookmarks *RustCoreBridge::bookmarks(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_bookmarks(m_core,
                        id.isEmpty() ? nullptr : id.constData());
}

AcPolicy *RustCoreBridge::policy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_policy(m_core,
                     id.isEmpty() ? nullptr : id.constData());
}

AcSession *RustCoreBridge::session(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_session(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcIcons *RustCoreBridge::icons(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_icons(m_core,
                    id.isEmpty() ? nullptr : id.constData());
}

RcNotes *RustCoreBridge::notes(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_notes(m_core,
                    id.isEmpty() ? nullptr : id.constData());
}

AcDlwpolicy *RustCoreBridge::dlwpolicy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_dlwpolicy(m_core,
                        id.isEmpty() ? nullptr : id.constData());
}

AcPrivacy *RustCoreBridge::privacy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_privacy(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcUrlclean *RustCoreBridge::urlclean(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_urlclean(m_core,
                       id.isEmpty() ? nullptr : id.constData());
}

AcCookies *RustCoreBridge::cookies(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_cookies(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

void rustCoreEnsureDataDir()
{
    // Cheap and idempotent — call per operation so test-mode
    // data-dir switches propagate.
    const QByteArray dir =
        QFileInfo(BrowserPaths::dataFilePath(QLatin1String(".rc")))
            .absolutePath().toUtf8();
    rc_set_data_dir(dir.constData());
}

void rustCoreEnsureDataDirFor(const QString &profileId)
{
    const QByteArray dir =
        BrowserPaths::profileDataDir(profileId).toUtf8();
    rc_set_data_dir(dir.constData());
}

// ---- RC02: RustCoreSettings ---------------------------------------

namespace {

QByteArray takeBuffer(RcBuffer &buf)
{
    if (buf.data == nullptr)
        return QByteArray();
    const QByteArray out(reinterpret_cast<char *>(buf.data),
                         static_cast<qsizetype>(buf.len));
    rc_buffer_free(buf);
    return out;
}

} // namespace

RustCoreSettings::RustCoreSettings(const QString &profileId)
    : m_h(nullptr)
    , m_profileId(profileId)
{
    // Facade-first (RC08): the shared AcCore mints the same
    // RcSettings but tracks it against the app-lifetime root.  The
    // legacy spelling remains for contexts serving() refuses.
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        m_h = bridge->settings(profileId);
    if (m_h == nullptr) {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        const QByteArray id = profileId.toUtf8();
        m_h = rc_settings_open(root.constData(),
                               id.isEmpty() ? nullptr : id.constData());
    }
}

RustCoreSettings::~RustCoreSettings()
{
    // ac_settings_close is NULL-tolerant and also legal on an
    // rc_settings_open handle (its core-untrack lookup just misses).
    ac_settings_close(m_h);
}

bool RustCoreSettings::isValid() const
{
    return m_h != nullptr;
}

QString RustCoreSettings::profileId() const
{
    return m_profileId;
}

QVariant RustCoreSettings::value(const QString &key,
                                 const QVariant &defaultValue) const
{
    if (m_h == nullptr)
        return defaultValue;
    RcBuffer buf{nullptr, 0};
    if (rc_settings_get(m_h, key.toUtf8().constData(), &buf) != RC_OK)
        return defaultValue;
    const QJsonObject row =
        QJsonDocument::fromJson(takeBuffer(buf)).object();
    const QString t = row.value(QLatin1String("t")).toString();
    const QJsonValue v = row.value(QLatin1String("v"));
    if (t == QLatin1String("s"))
        return v.toString();
    if (t == QLatin1String("b"))
        return v.toBool();
    if (t == QLatin1String("i"))
        return QVariant::fromValue(v.toInteger());
    if (t == QLatin1String("d"))
        return v.toDouble();
    if (t == QLatin1String("x"))
        return QByteArray::fromHex(v.toString().toLatin1());
    if (t == QLatin1String("l")) {
        QStringList list;
        const QJsonArray items = v.toArray();
        list.reserve(items.size());
        for (const QJsonValue &item : items)
            list.append(item.toString());
        return list;
    }
    return defaultValue;
}

void RustCoreSettings::setValue(const QString &key,
                                const QVariant &value)
{
    if (m_h == nullptr)
        return;
    const QByteArray k = key.toUtf8();
    switch (value.typeId()) {
    case QMetaType::Bool:
        rc_settings_set_bool(m_h, k.constData(), value.toBool() ? 1 : 0);
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        rc_settings_set_int(m_h, k.constData(), value.toLongLong());
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        rc_settings_set_double(m_h, k.constData(), value.toDouble());
        break;
    case QMetaType::QByteArray: {
        const QByteArray bytes = value.toByteArray();
        rc_settings_set_bytes(m_h, k.constData(),
                              reinterpret_cast<const uint8_t *>(
                                  bytes.constData()),
                              static_cast<size_t>(bytes.size()));
        break;
    }
    case QMetaType::QStringList: {
        const QByteArray json = QJsonDocument(
            QJsonArray::fromStringList(value.toStringList())).toJson();
        rc_settings_set_list(m_h, k.constData(),
                             reinterpret_cast<const uint8_t *>(
                                 json.constData()),
                             static_cast<size_t>(json.size()));
        break;
    }
    default: {
        const QByteArray s = value.toString().toUtf8();
        rc_settings_set_string(m_h, k.constData(), s.constData());
        break;
    }
    }
}

void RustCoreSettings::remove(const QString &key)
{
    if (m_h == nullptr)
        return;
    rc_settings_remove(m_h, key.toUtf8().constData());
}

bool RustCoreSettings::contains(const QString &key) const
{
    if (m_h == nullptr)
        return false;
    return rc_settings_contains(m_h, key.toUtf8().constData()) != 0;
}

QStringList RustCoreSettings::allKeys(const QString &prefix) const
{
    if (m_h == nullptr)
        return {};
    char *json = rc_settings_list(
        m_h, prefix.isEmpty() ? nullptr : prefix.toUtf8().constData());
    if (json == nullptr)
        return {};
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    const QJsonArray keys = QJsonDocument::fromJson(text).array();
    QStringList out;
    out.reserve(keys.size());
    for (const QJsonValue &k : keys)
        out.append(k.toString());
    return out;
}

void RustCoreSettings::clear()
{
    if (m_h == nullptr)
        return;
    // The store has no bulk clear; removing each listed key keeps the
    // same remove-and-persist lane one key at a time.
    const QStringList keys = allKeys();
    for (const QString &key : keys)
        rc_settings_remove(m_h, key.toUtf8().constData());
}

void RustCoreSettings::sync()
{
    if (m_h == nullptr)
        return;
    rc_settings_sync(m_h);
}

bool RustCoreSettings::importLegacyIni(const QString &iniPath)
{
    if (m_h == nullptr)
        return false;
    return rc_settings_import_ini(m_h, iniPath.toUtf8().constData())
        == RC_OK;
}

// ---- RC03: RustCoreProfiles ---------------------------------------

namespace {

QString takeString(char *p)
{
    if (p == nullptr)
        return QString();
    const QString out = QString::fromUtf8(p);
    rc_string_free(p);
    return out;
}

ProfileContext contextFromJson(const QJsonObject &row)
{
    ProfileContext context;
    context.id = row.value(QLatin1String("id")).toString();
    context.name = row.value(QLatin1String("name")).toString();
    context.avatar = row.value(QLatin1String("avatar")).toString();
    context.createdAt = QDateTime::fromString(
        row.value(QLatin1String("createdAt")).toString(), Qt::ISODate);
    return context;
}

} // namespace

RustCoreProfiles::RustCoreProfiles(const QString &dataRoot)
    : m_h(nullptr)
{
    // Facade-first (RC08): the default root binds through the shared
    // AcCore; an explicit root is a fixture scope the core never saw,
    // so it keeps the direct rc_profiles_open spelling.
    if (dataRoot.isEmpty()) {
        if (RustCoreBridge *bridge = RustCoreBridge::serving())
            m_h = bridge->profiles();
    }
    if (m_h == nullptr) {
        const QString resolved =
            dataRoot.isEmpty() ? BrowserPaths::dataRoot() : dataRoot;
        // The synthesized default row's name seeds exactly like
        // ProfileRegistry::load: the PROF01-era QSettings key first,
        // the localized fallback after.
        QByteArray seed = QSettings()
            .value(QLatin1String("profile/name")).toString().toUtf8();
        if (seed.isEmpty())
            seed = QCoreApplication::translate("ProfileRegistry",
                                               "Profile").toUtf8();
        const QByteArray root = resolved.toUtf8();
        m_h = rc_profiles_open(root.constData(), seed.constData());
    }
}

RustCoreProfiles::~RustCoreProfiles()
{
    // ac_profiles_close accepts rc_profiles_open handles too.
    ac_profiles_close(m_h);
}

bool RustCoreProfiles::isValid() const
{
    return m_h != nullptr;
}

QString RustCoreProfiles::filePath() const
{
    if (m_h == nullptr)
        return QString();
    return takeString(rc_profiles_file(m_h));
}

QList<ProfileContext> RustCoreProfiles::profiles() const
{
    QList<ProfileContext> out;
    if (m_h == nullptr)
        return out;
    char *json = rc_profiles_list(m_h);
    if (json == nullptr)
        return out;
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    const QJsonArray rows = QJsonDocument::fromJson(text).array();
    out.reserve(rows.size());
    for (const QJsonValue &row : rows)
        out.append(contextFromJson(row.toObject()));
    return out;
}

ProfileContext RustCoreProfiles::profile(const QString &id) const
{
    if (m_h == nullptr)
        return ProfileContext();
    char *json = rc_profiles_get(m_h, id.toUtf8().constData());
    if (json == nullptr)
        return ProfileContext();
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    return contextFromJson(
        QJsonDocument::fromJson(text).object());
}

QString RustCoreProfiles::activeProfileId() const
{
    if (m_h == nullptr)
        return QStringLiteral("default");
    return takeString(rc_profiles_active(m_h));
}

void RustCoreProfiles::setActiveProfileId(const QString &id)
{
    if (m_h == nullptr)
        return;
    rc_profiles_set_active(m_h, id.toUtf8().constData());
}

ProfileContext RustCoreProfiles::createProfile(const QString &displayName)
{
    if (m_h == nullptr)
        return ProfileContext();
    const QString id = takeString(
        rc_profiles_create(m_h, displayName.toUtf8().constData()));
    if (id.isEmpty())
        return ProfileContext();
    return profile(id);
}

bool RustCoreProfiles::renameProfile(const QString &id,
                                     const QString &name)
{
    if (m_h == nullptr)
        return false;
    return rc_profiles_rename(m_h, id.toUtf8().constData(),
                              name.toUtf8().constData()) == RC_OK;
}

bool RustCoreProfiles::setAvatarName(const QString &id,
                                     const QString &fileName)
{
    if (m_h == nullptr)
        return false;
    const QByteArray name = fileName.toUtf8();
    return rc_profiles_set_avatar(
               m_h, id.toUtf8().constData(),
               name.isEmpty() ? nullptr : name.constData())
        == RC_OK;
}

bool RustCoreProfiles::removeAvatar(const QString &id)
{
    return setAvatarName(id, QString());
}

bool RustCoreProfiles::deleteProfile(const QString &id)
{
    if (m_h == nullptr)
        return false;
    return rc_profiles_delete(m_h, id.toUtf8().constData()) == RC_OK;
}

bool RustCoreProfiles::reload()
{
    if (m_h == nullptr)
        return false;
    return rc_profiles_reload(m_h) == RC_OK;
}

namespace {

using PathFn = char *(*)(RcProfiles *, const char *);

QString pathVia(RcProfiles *h, PathFn fn, const QString &id)
{
    if (h == nullptr)
        return QString();
    const QByteArray utf8 = id.toUtf8();
    return takeString(fn(h, utf8.constData()));
}

} // namespace

QString RustCoreProfiles::profileDir(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_dir, id);
}

QString RustCoreProfiles::profileCacheDir(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_cache_dir, id);
}

QString RustCoreProfiles::engineStorageDir(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_engine_storage_dir, id);
}

QString RustCoreProfiles::engineCacheDir(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_engine_cache_dir, id);
}

QString RustCoreProfiles::settingsDir(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_settings_dir, id);
}

QString RustCoreProfiles::avatarPath(const QString &id) const
{
    return pathVia(m_h, &rc_profiles_avatar_path, id);
}

QString RustCoreProfiles::slugify(const QString &displayName)
{
    return takeString(
        rc_profiles_slugify(displayName.toUtf8().constData()));
}
