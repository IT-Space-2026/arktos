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
    for (const QString &keyword : m_keywords.keys(engine))
        m_keywords.remove(keyword);
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

    QString file = QDir(enginesDirectory()).filePath(generateEngineFileName(name));
    QFile::remove(file);

    // Removing a bundled engine must survive the bundled merge in
    // load() — otherwise the next launch resurrects it.
    if (QFile::exists(QLatin1String(":/searchengines/") + generateEngineFileName(name))
            && !m_removedBundled.contains(name))
        m_removedBundled.append(name);

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

    // The persisted descriptor keeps the old generated file name.
    QFile::remove(QDir(enginesDirectory())
                  .filePath(generateEngineFileName(oldName)));

    // Renaming a bundled engine blocks the bundled descriptor from
    // resurrecting on the next load (same bookkeeping removeEngine
    // uses); naming an engine after a previously removed bundled one
    // unblocks it — the user's own engine with that name exists now.
    if (QFile::exists(QLatin1String(":/searchengines/")
                      + generateEngineFileName(oldName))
            && !m_removedBundled.contains(oldName))
        m_removedBundled.append(oldName);
    m_removedBundled.removeAll(trimmed);

    emit currentEngineChanged();
    emit changed();
    return true;
}

void OpenSearchManager::engineEdited(OpenSearchEngine *engine)
{
    if (!engine || m_engines.key(engine).isEmpty())
        return;
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

void OpenSearchManager::load()
{
    StartupProfile::Scope profileScope("opensearch engines parse");
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

    if (OpenSearchEngine *engine = engineForKeyword(keyword))
        return engine->searchUrl(terms);

    return QUrl();
}

OpenSearchEngine *OpenSearchManager::engineForKeyword(const QString &keyword) const
{
    if (keyword.isEmpty())
        return nullptr;
    if (!m_keywords.contains(keyword))
        return nullptr;
    return m_keywords.value(keyword);
}

void OpenSearchManager::setEngineForKeyword(const QString &keyword, OpenSearchEngine *engine)
{
    if (keyword.isEmpty())
        return;

    if (!engine)
        m_keywords.remove(keyword);
    else
        m_keywords.insert(keyword, engine);

    emit changed();
}

QStringList OpenSearchManager::keywordsForEngine(OpenSearchEngine *engine) const
{
    return m_keywords.keys(engine);
}

void OpenSearchManager::setKeywordsForEngine(OpenSearchEngine *engine, const QStringList &keywords)
{
    if (!engine)
        return;

    for (const QString &keyword : keywordsForEngine(engine))
        m_keywords.remove(keyword);

    for (const QString &keyword : keywords) {
        if (keyword.isEmpty())
            continue;

        m_keywords.insert(keyword, engine);
    }

    emit changed();
}
