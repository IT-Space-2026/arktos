/*
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
 * Copyright 2009 Christian Franke <cfchris6@ts2server.com>
 * Copyright 2009 Christopher Eby <kreed@kreed.org>
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

#include "opensearchmanager.h"

#include "autosaver.h"
#include "browserapplication.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchreader.h"
#include "opensearchwriter.h"
#include "startupprofile.h"

#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qmessagebox.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qsettings.h>
#include <qstringlist.h>

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"
#include "rustcorebridge.h"

#include <qcoreapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlocale.h>

// OSE01: searchengines.json in the rustcore data dir is the canonical
// engine store — engine records (the rc_opensearch_parse field map
// plus keyword bindings), the display order and the removed-bundled
// blocklist.  These helpers marshal the JSON contract rustcore.h
// documents; every mutation below writes through to the store.
namespace {

QByteArray oseTake(char *raw)
{
    if (!raw)
        return QByteArray();
    const QByteArray bytes(raw, int(strlen(raw)));
    rc_string_free(raw);
    return bytes;
}

QStringList oseStringList(char *raw)
{
    QStringList out;
    const QJsonArray array =
        QJsonDocument::fromJson(oseTake(raw)).array();
    for (const QJsonValue &value : array)
        out.append(value.toString());
    return out;
}

QByteArray oseJsonStringArray(const QStringList &list)
{
    return QJsonDocument(QJsonArray::fromStringList(list))
        .toJson(QJsonDocument::Compact);
}

QByteArray oseEngineJson(OpenSearchEngine *engine)
{
    return QJsonDocument(openSearchEngineToJson(engine))
        .toJson(QJsonDocument::Compact);
}

// The {language}/{source} substitutions parseTemplate applies.
QByteArray oseLanguage()
{
    QString language = QLocale().name();
    language.replace(QLatin1Char('_'), QLatin1Char('-'));
    return language.toUtf8();
}

QByteArray oseSource()
{
    return QCoreApplication::applicationName().toUtf8();
}

} // namespace
#endif // ARORA_RUSTCORE

OpenSearchManager::OpenSearchManager(QObject *parent)
    : QObject(parent)
    , m_autoSaver(new AutoSaver(this))
{
    connect(this, &OpenSearchManager::changed,
            m_autoSaver, &AutoSaver::changeOccurred);

    load();
}

OpenSearchManager::~OpenSearchManager()
{
    m_autoSaver->saveIfNeccessary();
    qDeleteAll(m_engines.values());
    m_engines.clear();
}

QString OpenSearchManager::currentEngineName() const
{
    return m_current;
}

void OpenSearchManager::setCurrentEngineName(QString name)
{
    if (!m_engines.contains(name))
        return;

    m_current = std::move(name);
    emit currentEngineChanged();
    emit changed();
}

OpenSearchEngine *OpenSearchManager::currentEngine() const
{
    if (m_current.isEmpty() || !m_engines.contains(m_current))
        return nullptr;

    return m_engines[m_current];
}

void OpenSearchManager::setCurrentEngine(OpenSearchEngine *engine)
{
    if (!engine)
        return;

    setCurrentEngineName(m_engines.key(engine));
}

OpenSearchEngine *OpenSearchManager::engine(const QString &name)
{
    if (!m_engines.contains(name))
        return nullptr;

    return m_engines[name];
}

QString OpenSearchManager::privateEngineName() const
{
    return m_privateEngine;
}

void OpenSearchManager::setPrivateEngineName(QString name)
{
    if (!name.isEmpty() && !m_engines.contains(name))
        return;

    if (m_privateEngine == name)
        return;

    m_privateEngine = std::move(name);
    emit currentEngineChanged();
    emit changed();
}

QString OpenSearchManager::imageEngineName() const
{
    return m_imageEngine;
}

void OpenSearchManager::setImageEngineName(QString name)
{
    if (!name.isEmpty() && !m_engines.contains(name))
        return;

    if (m_imageEngine == name)
        return;

    m_imageEngine = std::move(name);
    emit currentEngineChanged();
    emit changed();
}

QString OpenSearchManager::fieldEngineName() const
{
    return m_fieldEngine;
}

void OpenSearchManager::setFieldEngineName(QString name)
{
    if (!name.isEmpty() && !m_engines.contains(name))
        return;

    if (m_fieldEngine == name)
        return;

    m_fieldEngine = std::move(name);
    emit currentEngineChanged();
    emit changed();
}

bool OpenSearchManager::keepFieldEngine() const
{
    return m_keepFieldEngine;
}

void OpenSearchManager::setKeepFieldEngine(bool keep)
{
    if (m_keepFieldEngine == keep)
        return;

    m_keepFieldEngine = keep;
    // Toggling the option off must not resurrect a stale pick on the
    // next start — drop the stored field engine entirely.
    if (!keep && !m_fieldEngine.isEmpty()) {
        m_fieldEngine.clear();
        emit currentEngineChanged();
    }
    emit changed();
}

OpenSearchEngine *OpenSearchManager::engineForContext(bool privateContext) const
{
    if (privateContext && m_engines.contains(m_privateEngine))
        return m_engines.value(m_privateEngine);

    return currentEngine();
}

OpenSearchEngine *OpenSearchManager::searchFieldEngine(bool privateContext) const
{
    if (privateContext)
        return engineForContext(true);

    if (m_engines.contains(m_fieldEngine))
        return m_engines.value(m_fieldEngine);

    return currentEngine();
}

OpenSearchEngine *OpenSearchManager::imageSearchEngine() const
{
    if (OpenSearchEngine *engine = m_engines.value(m_imageEngine)) {
        if (engine->providesImageSearch())
            return engine;
    }
    // The stored pick is unset, gone, or lost its image endpoint —
    // fall back to the default engine when it can image-search.
    OpenSearchEngine *fallback = currentEngine();
    return (fallback && fallback->providesImageSearch()) ? fallback
                                                       : nullptr;
}

bool OpenSearchManager::engineExists(const QString &name)
{
    return m_engines.contains(name);
}

QStringList OpenSearchManager::allEnginesNames() const
{
    // Ordered list: the persisted m_engineOrder first, then any
    // engine missing from it (defensive — addEngine/removeEngine/load
    // keep the two in sync).
    QStringList names;
    names.reserve(m_engines.count());
    for (const QString &name : m_engineOrder) {
        if (m_engines.contains(name) && !names.contains(name))
            names.append(name);
    }
    for (auto it = m_engines.constBegin(), end = m_engines.constEnd();
         it != end; ++it) {
        if (!names.contains(it.key()))
            names.append(it.key());
    }
    return names;
}

int OpenSearchManager::enginesCount() const
{
    return m_engines.count();
}

void OpenSearchManager::addEngine(const QUrl &url)
{
    if (!url.isValid())
        return;

    QNetworkReply *reply = NetworkAccessManager::instance()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, &OpenSearchManager::engineFromUrlAvailable);
    reply->setParent(this);
}

bool OpenSearchManager::addEngine(const QString &fileName)
{
    QFile file(fileName);
    // The reader bounds its input as well; rejecting here avoids
    // opening obviously-oversized files at all.
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return false;

    OpenSearchReader reader;
    OpenSearchEngine *engine = reader.read(&file);

    if (!addEngine(engine)) {
        delete engine;
        return false;
    }

    return true;
}

bool OpenSearchManager::addEngine(OpenSearchEngine *engine)
{
    if (!engine)
        return false;

    if (!engine->isValid())
        return false;

    if (m_engines.contains(engine->name()))
        return false;

#if defined(ARORA_RUSTCORE)
    // OSE01: the registry is canonical — the record writes through
    // immediately (keyword bindings arrive via setKeywordsForEngine).
    rustCoreEnsureDataDir();
    const QByteArray json = oseEngineJson(engine);
    if (rc_ose_put(reinterpret_cast<const uint8_t *>(json.constData()),
                   size_t(json.size())) != RC_OK)
        return false;
#endif

    m_engines[engine->name()] = engine;
    if (!m_engineOrder.contains(engine->name()))
        m_engineOrder.append(engine->name());

    emit changed();

    return true;
}

void OpenSearchManager::removeEngine(const QString &name)
{
    if (m_engines.count() <= 1)
        return;

    if (!m_engines.contains(name))
        return;

    OpenSearchEngine *engine = m_engines[name];
#if !defined(ARORA_RUSTCORE)
    for (const QString &keyword : m_keywords.keys(engine))
        m_keywords.remove(keyword);
#endif
    engine->deleteLater();

    m_engines[name] = nullptr;
    m_engines.remove(name);
    m_engineOrder.removeAll(name);

    m_suggestionsEnabled.removeAll(name);

    // A removed engine cannot stay the configured private/image/field
    // pick — the resolution helpers fall back to the default engine.
    if (name == m_privateEngine)
        m_privateEngine.clear();
    if (name == m_imageEngine)
        m_imageEngine.clear();
    if (name == m_fieldEngine)
        m_fieldEngine.clear();

#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    const QByteArray utf8 = name.toUtf8();
    // The record drops its keyword bindings with it.
    rc_ose_remove(utf8.constData());
#else
    QString file = QDir(enginesDirectory()).filePath(generateEngineFileName(name));
    QFile::remove(file);
#endif

    // Removing a bundled engine must survive the bundled merge in
    // load() — otherwise the next launch resurrects it.
    if (QFile::exists(QLatin1String(":/searchengines/") + generateEngineFileName(name))
            && !m_removedBundled.contains(name)) {
        m_removedBundled.append(name);
#if defined(ARORA_RUSTCORE)
        rc_ose_block_bundled(utf8.constData());
#endif
    }

    if (name == m_current)
        setCurrentEngineName(m_engines.keys().at(0));

    emit changed();
}

// SRCH05: reorder the engine list — offset is relative (-1/+1 for
// the settings editor's Up/Down buttons).
bool OpenSearchManager::moveEngine(const QString &name, int offset)
{
    const int from = m_engineOrder.indexOf(name);
    if (from < 0 || offset == 0)
        return false;

    const int to = from + offset;
    if (to < 0 || to >= m_engineOrder.count())
        return false;

    m_engineOrder.move(from, to);
#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    const QByteArray json = oseJsonStringArray(m_engineOrder);
    rc_ose_reorder(reinterpret_cast<const uint8_t *>(json.constData()),
                   size_t(json.size()));
#endif
    emit changed();
    return true;
}

// SRCH05: name is the m_engines key AND several name-keyed stores, so
// renaming goes through the manager to keep all of them in sync.
// Keyword bindings hold engine POINTERS and survive untouched.
bool OpenSearchManager::renameEngine(const QString &oldName, const QString &newName)
{
    const QString trimmed = newName.trimmed();
    if (!m_engines.contains(oldName) || trimmed.isEmpty())
        return false;
    if (trimmed == oldName)
        return false;
    if (m_engines.contains(trimmed))
        return false;

#if defined(ARORA_RUSTCORE)
    // The store move keeps the record's keyword bindings and its
    // display-order slot — abort before touching any Qt state if it
    // fails.
    rustCoreEnsureDataDir();
    const QByteArray oldUtf8 = oldName.toUtf8();
    const QByteArray newUtf8 = trimmed.toUtf8();
    if (rc_ose_rename(oldUtf8.constData(), newUtf8.constData())
            != RC_OK)
        return false;
#endif

    OpenSearchEngine *engine = m_engines.take(oldName);
    engine->setName(trimmed);
    m_engines.insert(trimmed, engine);

    const int orderIndex = m_engineOrder.indexOf(oldName);
    if (orderIndex >= 0)
        m_engineOrder[orderIndex] = trimmed;
    else
        m_engineOrder.append(trimmed);

    if (m_suggestionsEnabled.removeAll(oldName))
        m_suggestionsEnabled.append(trimmed);
    if (m_current == oldName)
        m_current = trimmed;
    if (m_privateEngine == oldName)
        m_privateEngine = trimmed;
    if (m_imageEngine == oldName)
        m_imageEngine = trimmed;
    if (m_fieldEngine == oldName)
        m_fieldEngine = trimmed;

    // Renaming a bundled engine blocks the bundled descriptor from
    // resurrecting on the next load (same bookkeeping removeEngine
    // uses); naming an engine after a previously removed bundled one
    // unblocks it — the user's own engine with that name exists now.
#if defined(ARORA_RUSTCORE)
    if (QFile::exists(QLatin1String(":/searchengines/")
                      + generateEngineFileName(oldName))
            && !m_removedBundled.contains(oldName)) {
        m_removedBundled.append(oldName);
        rc_ose_block_bundled(oldUtf8.constData());
    }
    m_removedBundled.removeAll(trimmed);
    rc_ose_unblock_bundled(newUtf8.constData());
#else
    // The persisted descriptor keeps the old generated file name.
    QFile::remove(QDir(enginesDirectory())
                  .filePath(generateEngineFileName(oldName)));

    if (QFile::exists(QLatin1String(":/searchengines/")
                      + generateEngineFileName(oldName))
            && !m_removedBundled.contains(oldName))
        m_removedBundled.append(oldName);
    m_removedBundled.removeAll(trimmed);
#endif

    emit currentEngineChanged();
    emit changed();
    return true;
}

void OpenSearchManager::engineEdited(OpenSearchEngine *engine)
{
    if (!engine || m_engines.key(engine).isEmpty())
        return;

#if defined(ARORA_RUSTCORE)
    // OSE01: engine objects carry no change notification — the edit
    // notification pushes the record through immediately.
    rustCoreEnsureDataDir();
    const QByteArray json = oseEngineJson(engine);
    rc_ose_put(reinterpret_cast<const uint8_t *>(json.constData()),
               size_t(json.size()));
#endif

    emit changed();
}

QString OpenSearchManager::generateEngineFileName(const QString &engineName) const
{
    QString fileName;

    // Strip special characters from the name.
    for (int i = 0; i < engineName.size(); ++i) {
        if (engineName.at(i).isSpace()) {
            fileName.append(QLatin1Char('_'));
            continue;
        }

        if (engineName.at(i).isLetterOrNumber())
            fileName.append(engineName.at(i));
    }

    fileName.append(QLatin1String(".xml"));

    return fileName;
}

void OpenSearchManager::saveDirectory(const QString &dirName)
{
    QDir dir;
    if (!dir.mkpath(dirName))
        return;
    dir.setPath(dirName);

    OpenSearchWriter writer;

    for (OpenSearchEngine *engine : m_engines.values()) {
        QString name = generateEngineFileName(engine->name());
        QString fileName = dir.filePath(name);

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly))
            continue;

        writer.write(&file, engine);
    }
}

void OpenSearchManager::save()
{
#if defined(ARORA_RUSTCORE)
    // OSE01: mutations write through; the autosave still re-syncs the
    // engine objects (a direct field edit only marks the saver dirty)
    // and the display order.  The descriptor dir plus the keyword,
    // engineOrder and removedBundledEngines keys are retired by
    // migration — only the preference keys are still persisted here.
    rustCoreEnsureDataDir();
    for (OpenSearchEngine *engine : m_engines.values()) {
        const QByteArray json = oseEngineJson(engine);
        rc_ose_put(reinterpret_cast<const uint8_t *>(json.constData()),
                   size_t(json.size()));
    }
    const QByteArray order = oseJsonStringArray(m_engineOrder);
    rc_ose_reorder(reinterpret_cast<const uint8_t *>(order.constData()),
                   size_t(order.size()));

    QSettings settings;
    settings.beginGroup(QLatin1String("openSearch"));
    settings.setValue(QLatin1String("engine"), m_current);
#else
    saveDirectory(enginesDirectory());

    QSettings settings;
    settings.beginGroup(QLatin1String("openSearch"));
    settings.setValue(QLatin1String("engine"), m_current);
    settings.setValue(QLatin1String("removedBundledEngines"), m_removedBundled);
    // SRCH05: user-arranged engine order for the settings editor and
    // the engine menus.
    settings.setValue(QLatin1String("engineOrder"), m_engineOrder);

    settings.beginWriteArray(QLatin1String("keywords"), m_keywords.count());
    QHash<QString, OpenSearchEngine*>::const_iterator i = m_keywords.constBegin();
    QHash<QString, OpenSearchEngine*>::const_iterator end = m_keywords.constEnd();
    int j = 0;
    for (; i != end; ++i) {
        settings.setArrayIndex(j++);
        settings.setValue(QLatin1String("keyword"), i.key());
        settings.setValue(QLatin1String("engine"), i.value()->name());
    }
    settings.endArray();
#endif
    settings.setValue(QLatin1String("suggestions"), m_suggestionsEnabled);

    // SRCH04: engine assignments + suggestion-context toggles.
    settings.setValue(QLatin1String("privateEngine"), m_privateEngine);
    settings.setValue(QLatin1String("imageEngine"), m_imageEngine);
    settings.setValue(QLatin1String("keepFieldEngine"), m_keepFieldEngine);
    // The field pick only persists while "keep last selected" is on;
    // with it off the field always starts on the default engine.
    settings.setValue(QLatin1String("fieldEngine"),
                      m_keepFieldEngine ? m_fieldEngine : QString());
    settings.setValue(QLatin1String("suggestInAddressField"), m_suggestInAddressField);
    settings.setValue(QLatin1String("suggestInSearchField"), m_suggestInSearchField);
    settings.setValue(QLatin1String("suggestOnlyWithKeyword"), m_suggestOnlyWithKeyword);

    settings.endGroup();
}

bool OpenSearchManager::loadDirectory(const QString &dirName)
{
    if (!QFile::exists(dirName))
        return false;

    QDirIterator iterator(dirName, QStringList() << QLatin1String("*.xml"));

    if (!iterator.hasNext())
        return false;

    bool success = false;

    while (iterator.hasNext()) {
        if (addEngine(iterator.next()))
            success = true;
    }

    return success;
}

#if defined(ARORA_RUSTCORE)
// OSE01: one-shot migration from the legacy layout — the descriptor
// directory, the openSearch/keywords array, engineOrder and
// removedBundledEngines all move into searchengines.json, then the
// legacy paths retire so a second run imports nothing.
void OpenSearchManager::importLegacyRegistry(QSettings &settings)
{
    // The blocklist first — it gates the bundled seed below.
    const QStringList removed = settings.value(
        QLatin1String("removedBundledEngines")).toStringList();
    for (const QString &name : removed) {
        const QByteArray utf8 = name.toUtf8();
        rc_ose_block_bundled(utf8.constData());
    }

    // The persisted descriptor dir imports filename-sorted so the
    // migrated order is deterministic; rc_ose_import tolerates the
    // same inputs addEngine(fileName) did.
    const QString dirName = enginesDirectory();
    const QDir dir(dirName);
    const QStringList files = dir.entryList(
        QStringList() << QLatin1String("*.xml"), QDir::Files, QDir::Name);
    for (const QString &fileName : files) {
        QFile file(dir.filePath(fileName));
        if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray xml = file.readAll();
        rc_ose_import(reinterpret_cast<const uint8_t *>(xml.constData()),
                      size_t(xml.size()));
    }

    rc_ose_seed_bundled();

    // The keyword array flattens into per-engine bindings — last write
    // wins, same as the QHash the code used to fill.
    QHash<QString, QStringList> byEngine;
    const int size = settings.beginReadArray(QLatin1String("keywords"));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        const QString keyword =
            settings.value(QLatin1String("keyword")).toString();
        const QString engineName =
            settings.value(QLatin1String("engine")).toString();
        if (!keyword.isEmpty() && !engineName.isEmpty()) {
            for (auto it = byEngine.begin(); it != byEngine.end(); ++it)
                it->removeAll(keyword);
            byEngine[engineName].append(keyword);
        }
    }
    settings.endArray();
    for (auto it = byEngine.constBegin(); it != byEngine.constEnd(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const QByteArray json = oseJsonStringArray(it.value());
        rc_ose_set_keywords(name.constData(),
                            reinterpret_cast<const uint8_t *>(json.constData()),
                            size_t(json.size()));
    }

    const QByteArray order = oseJsonStringArray(
        settings.value(QLatin1String("engineOrder")).toStringList());
    rc_ose_reorder(reinterpret_cast<const uint8_t *>(order.constData()),
                   size_t(order.size()));

    // Retire the legacy paths — a leftover descriptor dir or stale
    // keys must never re-import over the canonical store.
    if (dir.exists())
        QDir().rename(dirName,
                      dirName + QLatin1String(".migrated"));
    settings.remove(QLatin1String("keywords"));
    settings.remove(QLatin1String("engineOrder"));
    settings.remove(QLatin1String("removedBundledEngines"));
}
#endif // ARORA_RUSTCORE

void OpenSearchManager::load()
{
    StartupProfile::Scope profileScope("opensearch engines parse");
#if defined(ARORA_RUSTCORE)
    // OSE01: searchengines.json is canonical; first access migrates
    // the legacy descriptor dir + QSettings openSearch keys.
    rustCoreEnsureDataDir();

    QSettings settings;
    settings.beginGroup(QLatin1String("openSearch"));

    if (!rc_ose_store_present())
        importLegacyRegistry(settings);

    // The bundled merge survives in the Rust path too — upgrades
    // deliver new descriptors; removed_bundled keeps deleted ones
    // gone.  Idempotent: existing records always win.
    rc_ose_seed_bundled();

    m_removedBundled = oseStringList(rc_ose_removed_bundled());

    // Hydrate the engine objects from the canonical records — the
    // store's order IS the display order.
    const QStringList names = oseStringList(rc_ose_list());
    for (const QString &name : names) {
        const QByteArray utf8 = name.toUtf8();
        const QByteArray json = oseTake(rc_ose_get(utf8.constData()));
        OpenSearchEngine *engine = new OpenSearchEngine();
        openSearchEngineApplyJson(
            engine, QJsonDocument::fromJson(json).object());
        if (engine->isValid())
            m_engines[name] = engine;
        else
            delete engine;
    }
    m_engineOrder = names;

    m_current = settings.value(QLatin1String("engine"),
                               QLatin1String("DuckDuckGo")).toString();
    m_suggestionsEnabled =
        settings.value(QLatin1String("suggestions")).toStringList();

    // SRCH04: engine assignments + suggestion-context toggles.
    m_privateEngine = settings.value(QLatin1String("privateEngine")).toString();
    m_imageEngine = settings.value(QLatin1String("imageEngine")).toString();
    m_keepFieldEngine = settings.value(QLatin1String("keepFieldEngine"), true).toBool();
    m_fieldEngine = m_keepFieldEngine
        ? settings.value(QLatin1String("fieldEngine")).toString()
        : QString();
    m_suggestInAddressField = settings.value(
        QLatin1String("suggestInAddressField"), true).toBool();
    m_suggestInSearchField = settings.value(
        QLatin1String("suggestInSearchField"), true).toBool();
    m_suggestOnlyWithKeyword = settings.value(
        QLatin1String("suggestOnlyWithKeyword"), false).toBool();

    settings.endGroup();

    if (!m_engines.contains(m_current) && m_engines.count() > 0)
        m_current = m_engines.keys().at(0);

    emit currentEngineChanged();
#else
    loadDirectory(enginesDirectory());

    // get current engine
    QSettings settings;
    settings.beginGroup(QLatin1String("openSearch"));
    m_removedBundled = settings.value(
        QLatin1String("removedBundledEngines")).toStringList();

    // Bundled engines the persisted directory lacks get added —
    // upgrades deliver new descriptors to existing profiles.  Engines
    // the user explicitly removed stay gone via the blocklist.
    QDirIterator bundled(QLatin1String(":/searchengines"),
                       QStringList() << QLatin1String("*.xml"));
    while (bundled.hasNext()) {
        QFile file(bundled.next());
        // Same input bound as addEngine(fileName).
        if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
            continue;
        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&file);
        if (!engine || !engine->isValid()
                || m_engines.contains(engine->name())
                || m_removedBundled.contains(engine->name())) {
            delete engine;
            continue;
        }
        m_engines[engine->name()] = engine;
    }

    m_current = settings.value(QLatin1String("engine"), QLatin1String("DuckDuckGo")).toString();

    int size = settings.beginReadArray(QLatin1String("keywords"));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QString keyword = settings.value(QLatin1String("keyword")).toString();
        QString engineName = settings.value(QLatin1String("engine")).toString();
        m_keywords.insert(keyword, engine(engineName));
    }
    settings.endArray();

    m_suggestionsEnabled = settings.value(QLatin1String("suggestions")).toStringList();

    // SRCH05: restore the persisted engine order — names that failed
    // to load drop out, engines missing from the stored list (new
    // bundled descriptors, pre-SRCH05 profiles) append at the end.
    m_engineOrder = settings.value(QLatin1String("engineOrder")).toStringList();
    for (auto it = m_engineOrder.begin(); it != m_engineOrder.end();) {
        if (m_engines.contains(*it))
            ++it;
        else
            it = m_engineOrder.erase(it);
    }
    for (auto it = m_engines.constBegin(), end = m_engines.constEnd();
         it != end; ++it) {
        if (!m_engineOrder.contains(it.key()))
            m_engineOrder.append(it.key());
    }

    // SRCH04: engine assignments + suggestion-context toggles.
    m_privateEngine = settings.value(QLatin1String("privateEngine")).toString();
    m_imageEngine = settings.value(QLatin1String("imageEngine")).toString();
    m_keepFieldEngine = settings.value(QLatin1String("keepFieldEngine"), true).toBool();
    m_fieldEngine = m_keepFieldEngine
        ? settings.value(QLatin1String("fieldEngine")).toString()
        : QString();
    m_suggestInAddressField = settings.value(
        QLatin1String("suggestInAddressField"), true).toBool();
    m_suggestInSearchField = settings.value(
        QLatin1String("suggestInSearchField"), true).toBool();
    m_suggestOnlyWithKeyword = settings.value(
        QLatin1String("suggestOnlyWithKeyword"), false).toBool();

    settings.endGroup();

    if (!m_engines.contains(m_current) && m_engines.count() > 0)
        m_current = m_engines.keys().at(0);

    emit currentEngineChanged();
#endif // ARORA_RUSTCORE
}

bool OpenSearchManager::suggestionsEnabledForEngine(const QString &engineName) const
{
    // TOR02: suggestions stream keystrokes to the engine endpoint —
    // never enabled in a tor window regardless of the opt-in list.
    if (BrowserApplication::isTorMode())
        return false;
    return m_suggestionsEnabled.contains(engineName);
}

void OpenSearchManager::setSuggestionsEnabledForEngine(const QString &engineName, bool enabled)
{
    if (!m_engines.contains(engineName))
        return;

    if (enabled == suggestionsEnabledForEngine(engineName))
        return;

    if (enabled)
        m_suggestionsEnabled.append(engineName);
    else
        m_suggestionsEnabled.removeAll(engineName);

    emit suggestionsEnabledChanged();
    emit changed();
}

QStringList OpenSearchManager::suggestionsEnabledEngines() const
{
    return m_suggestionsEnabled;
}

bool OpenSearchManager::suggestionsInAddressField() const
{
    return m_suggestInAddressField;
}

void OpenSearchManager::setSuggestionsInAddressField(bool enabled)
{
    if (m_suggestInAddressField == enabled)
        return;
    m_suggestInAddressField = enabled;
    emit suggestionsEnabledChanged();
    emit changed();
}

bool OpenSearchManager::suggestionsInSearchField() const
{
    return m_suggestInSearchField;
}

void OpenSearchManager::setSuggestionsInSearchField(bool enabled)
{
    if (m_suggestInSearchField == enabled)
        return;
    m_suggestInSearchField = enabled;
    emit suggestionsEnabledChanged();
    emit changed();
}

bool OpenSearchManager::suggestionsOnlyWithKeyword() const
{
    return m_suggestOnlyWithKeyword;
}

void OpenSearchManager::setSuggestionsOnlyWithKeyword(bool enabled)
{
    if (m_suggestOnlyWithKeyword == enabled)
        return;
    m_suggestOnlyWithKeyword = enabled;
    emit suggestionsEnabledChanged();
    emit changed();
}

void OpenSearchManager::restoreDefaults()
{
#if defined(ARORA_RUSTCORE)
    // OSE01: the store-side restore clears the blocklist and re-adds
    // every bundled record, preserving keyword bindings.  Existing
    // objects refresh in place so consumers' pointers stay valid.
    rustCoreEnsureDataDir();
    m_removedBundled.clear();
    rc_ose_restore_bundled();
    const QStringList bundled = oseStringList(rc_ose_bundled_names());
    for (const QString &name : bundled) {
        const QByteArray utf8 = name.toUtf8();
        const QByteArray json = oseTake(rc_ose_get(utf8.constData()));
        const QJsonObject record =
            QJsonDocument::fromJson(json).object();
        OpenSearchEngine *engine = m_engines.value(name);
        if (!engine) {
            engine = new OpenSearchEngine();
            openSearchEngineApplyJson(engine, record);
            if (!engine->isValid()) {
                delete engine;
                continue;
            }
            m_engines[name] = engine;
            if (!m_engineOrder.contains(name))
                m_engineOrder.append(name);
            continue;
        }
        openSearchEngineApplyJson(engine, record);
    }
    emit currentEngineChanged();
    emit changed();
#else
    // Re-add every bundled engine.  Deleted ones come back (the
    // removal blocklist is part of "defaults"); ones still present
    // are REPLACED rather than skipped — persisted copies can be
    // stale (they predate new bundled fields like image-search
    // endpoints) and "restore defaults" means reverting them anyway.
    m_removedBundled.clear();
    QDirIterator iterator(QLatin1String(":/searchengines"),
                        QStringList() << QLatin1String("*.xml"));
    bool replaced = false;
    while (iterator.hasNext()) {
        QFile file(iterator.next());
        // Same input bound as addEngine(fileName).
        if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
            continue;

        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&file);
        if (!engine || !engine->isValid()) {
            delete engine;
            continue;
        }

        OpenSearchEngine *old = m_engines.value(engine->name());
        if (!old) {
            addEngine(engine); // emits changed()
            continue;
        }

        // Keyword bindings hold engine pointers — re-point them at
        // the fresh object before the old one dies.
        for (auto it = m_keywords.begin(), end = m_keywords.end();
             it != end; ++it) {
            if (it.value() == old)
                it.value() = engine;
        }
        m_engines[engine->name()] = engine;
        old->deleteLater();
        replaced = true;
    }
    if (replaced) {
        // Consumers cache the resolved engine pointer — re-resolve.
        emit currentEngineChanged();
        emit changed();
    }
#endif // ARORA_RUSTCORE
}

void OpenSearchManager::resetSearchPreferences()
{
    // Compiled defaults mirroring load()'s fallbacks; a missing engine
    // falls back to the first available exactly like load() does.
    m_current = QLatin1String("DuckDuckGo");
    if (!m_engines.contains(m_current) && m_engines.count() > 0)
        m_current = m_engines.keys().at(0);
    m_privateEngine.clear();
    m_imageEngine.clear();
    m_fieldEngine.clear();
    m_keepFieldEngine = true;
    m_suggestionsEnabled.clear();
    m_suggestInAddressField = true;
    m_suggestInSearchField = true;
    m_suggestOnlyWithKeyword = false;

    emit suggestionsEnabledChanged();
    emit currentEngineChanged();
    emit changed();
    // The autosave defers — persist immediately so the reset cannot be
    // lost to a crash before save() runs.
    save();
}

QString OpenSearchManager::enginesDirectory() const
{
    return BrowserPaths::dataFilePath(QLatin1String("searchengines"));
}

bool OpenSearchManager::confirmAddition(OpenSearchEngine *engine)
{
    if (!engine || !engine->isValid())
        return false;

    QString host = QUrl(engine->searchUrlTemplate()).host();

    QMessageBox::StandardButton button = QMessageBox::question(nullptr, QString(),
            tr("Do you want to add the following engine to your list of search engines?<br /><br />"
               "Name: %1<br />Searches on: %2").arg(engine->name(), host),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return (button == QMessageBox::Yes);
}

void OpenSearchManager::engineFromUrlAvailable()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

    if (!reply)
        return;

    if (reply->error() != QNetworkReply::NoError
        || reply->size() > 1024 * 1024) {
        reply->close();
        reply->deleteLater();
        return;
    }

    OpenSearchReader reader;
    OpenSearchEngine *engine = reader.read(reply);

    reply->close();
    reply->deleteLater();

    if (!engine->isValid()) {
        delete engine;
        return;
    }

    if (engineExists(engine->name())) {
        delete engine;
        return;
    }

    if (!confirmAddition(engine)) {
        delete engine;
        return;
    }

    if (!addEngine(engine)) {
        delete engine;
        return;
    }
}

QUrl OpenSearchManager::convertKeywordSearchToUrl(const QString &string)
{
    int i = string.indexOf(QLatin1Char(' '));
    if (i <= 0)
        return QUrl();

    const QString keyword = string.left(i);
    const QString terms = string.mid(i + 1);
    if (terms.isEmpty())
        return QUrl();

#if defined(ARORA_RUSTCORE)
    // OSE01: resolution + expansion run in Rust (same result as the
    // engineForKeyword + searchUrl calls below).
    rustCoreEnsureDataDir();
    const QByteArray keywordUtf8 = keyword.toUtf8();
    const QByteArray termsUtf8 = terms.toUtf8();
    const QByteArray language = oseLanguage();
    const QByteArray source = oseSource();
    const QByteArray url = oseTake(rc_ose_keyword_url(
        keywordUtf8.constData(), termsUtf8.constData(),
        language.constData(), source.constData()));
    return url.isEmpty() ? QUrl() : QUrl::fromEncoded(url);
#else
    if (OpenSearchEngine *engine = engineForKeyword(keyword))
        return engine->searchUrl(terms);

    return QUrl();
#endif
}

OpenSearchEngine *OpenSearchManager::engineForKeyword(const QString &keyword) const
{
    if (keyword.isEmpty())
        return nullptr;
#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    const QByteArray utf8 = keyword.toUtf8();
    const QString name = QString::fromUtf8(
        oseTake(rc_ose_engine_for_keyword(utf8.constData())));
    return name.isEmpty() ? nullptr : m_engines.value(name);
#else
    if (!m_keywords.contains(keyword))
        return nullptr;
    return m_keywords.value(keyword);
#endif
}

QStringList OpenSearchManager::keywords() const
{
#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    return oseStringList(rc_ose_keywords());
#else
    return m_keywords.keys();
#endif
}

void OpenSearchManager::setEngineForKeyword(const QString &keyword, OpenSearchEngine *engine)
{
    if (keyword.isEmpty())
        return;

#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    const QByteArray utf8 = keyword.toUtf8();
    if (!engine) {
        const QString owner = QString::fromUtf8(
            oseTake(rc_ose_engine_for_keyword(utf8.constData())));
        if (owner.isEmpty()) {
            emit changed();
            return;
        }
        QStringList list = keywordsForEngine(m_engines.value(owner));
        list.removeAll(keyword);
        setKeywordsForEngine(m_engines.value(owner), list);
        return;
    }
    QStringList list = keywordsForEngine(engine);
    list.removeAll(keyword);
    list.append(keyword);
    setKeywordsForEngine(engine, list);
    return;
#else
    if (!engine)
        m_keywords.remove(keyword);
    else
        m_keywords.insert(keyword, engine);

    emit changed();
#endif
}

QStringList OpenSearchManager::keywordsForEngine(OpenSearchEngine *engine) const
{
#if defined(ARORA_RUSTCORE)
    if (!engine)
        return QStringList();
    rustCoreEnsureDataDir();
    const QByteArray utf8 = engine->name().toUtf8();
    const QByteArray json = oseTake(rc_ose_get(utf8.constData()));
    QStringList out;
    const QJsonArray keywords =
        QJsonDocument::fromJson(json).object()
            .value(QLatin1String("keywords")).toArray();
    for (const QJsonValue &value : keywords)
        out.append(value.toString());
    return out;
#else
    return m_keywords.keys(engine);
#endif
}

void OpenSearchManager::setKeywordsForEngine(OpenSearchEngine *engine, const QStringList &keywords)
{
    if (!engine)
        return;

#if defined(ARORA_RUSTCORE)
    rustCoreEnsureDataDir();
    QStringList list;
    for (const QString &keyword : keywords) {
        if (!keyword.isEmpty())
            list.append(keyword);
    }
    const QByteArray name = engine->name().toUtf8();
    const QByteArray json = oseJsonStringArray(list);
    // The store strips the listed keywords off every other engine.
    rc_ose_set_keywords(name.constData(),
                        reinterpret_cast<const uint8_t *>(json.constData()),
                        size_t(json.size()));
#else
    for (const QString &keyword : keywordsForEngine(engine))
        m_keywords.remove(keyword);

    for (const QString &keyword : keywords) {
        if (keyword.isEmpty())
            continue;

        m_keywords.insert(keyword, engine);
    }
#endif

    emit changed();
}
