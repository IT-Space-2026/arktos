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

#ifndef OPENSEARCHMANAGER_H
#define OPENSEARCHMANAGER_H

#include <qobject.h>

#include <qhash.h>
#include <qpixmap.h>
#include <qurl.h>

class QNetworkReply;
class QNetworkRequest;
class QSettings;

class AutoSaver;
class OpenSearchEngine;
class OpenSearchEngineModel;

class OpenSearchManager : public QObject
{
    Q_OBJECT

signals:
    void changed();
    void currentEngineChanged();
    void suggestionsEnabledChanged();

public:
    OpenSearchManager(QObject *parent = nullptr);
    ~OpenSearchManager();

    QStringList allEnginesNames() const;
    int enginesCount() const;

    QString currentEngineName() const;
    void setCurrentEngineName(QString currentName);

    OpenSearchEngine *currentEngine() const;
    void setCurrentEngine(OpenSearchEngine *current);

    // SRCH04: the private windows (and the tor window, which is always
    // off-the-record) search through a separate engine when one is
    // configured — an empty name means "same as the default engine".
    QString privateEngineName() const;
    void setPrivateEngineName(QString name);
    // SRCH04: the engine used for image searches; empty = default
    // engine (used only when it actually offers image search).
    QString imageEngineName() const;
    void setImageEngineName(QString name);
    // SRCH04: the engine last picked in the search field's own
    // drop-down.  Session-only unless keepFieldEngine() is on; empty =
    // follow the default engine.
    QString fieldEngineName() const;
    void setFieldEngineName(QString name);
    bool keepFieldEngine() const;
    void setKeepFieldEngine(bool keep);

    // Engine that a search action in the given context resolves to:
    // private contexts use the private engine (falling back to the
    // default when unset or unavailable), normal contexts the default.
    OpenSearchEngine *engineForContext(bool privateContext) const;
    // The dedicated search box's engine: private contexts share the
    // private engine; elsewhere the field override wins over default.
    OpenSearchEngine *searchFieldEngine(bool privateContext) const;
    // The configured image-search engine — the stored pick when it
    // still exists and offers image search, else the default engine
    // when capable, else nullptr.
    OpenSearchEngine *imageSearchEngine() const;

    OpenSearchEngine *engine(const QString &name);

    bool engineExists(const QString &name);

    // SEC11: suggestion requests stream every keystroke to the
    // engine's suggest endpoint, so they are an explicit per-engine
    // opt-in — nothing is sent until an engine is enabled here.
    bool suggestionsEnabledForEngine(const QString &engineName) const;
    void setSuggestionsEnabledForEngine(const QString &engineName, bool enabled);
    QStringList suggestionsEnabledEngines() const;

    // SRCH04: the SEC11 per-engine opt-in stays the master switch;
    // these per-context toggles decide WHERE suggestions may fire.
    // Both default on (an engine that is not opted in still sends
    // nothing).  When onlyWithKeyword is set, the address field only
    // suggests once input starts with an engine's keyword.
    bool suggestionsInAddressField() const;
    void setSuggestionsInAddressField(bool enabled);
    bool suggestionsInSearchField() const;
    void setSuggestionsInSearchField(bool enabled);
    bool suggestionsOnlyWithKeyword() const;
    void setSuggestionsOnlyWithKeyword(bool enabled);

    QUrl convertKeywordSearchToUrl(const QString &string);
    OpenSearchEngine *engineForKeyword(const QString &keyword) const;
    void setEngineForKeyword(const QString &keyword, OpenSearchEngine *engine);
    // OMNI01: the registered shortcut keywords — the rustcore
    // classifier consumes the live list so 'keyword terms' keeps
    // priority in the routing decision.
    QStringList keywords() const;

    QStringList keywordsForEngine(OpenSearchEngine *engine) const;
    void setKeywordsForEngine(OpenSearchEngine *engine, const QStringList &keywords);

    void addEngine(const QUrl &url);
    bool addEngine(const QString &fileName);
    bool addEngine(OpenSearchEngine *engine);
    void removeEngine(const QString &name);
    void restoreDefaults();

    // SRCH06: the Search settings page's reset button returns every
    // preference the page owns to its compiled default — engine picks
    // (default/private/image/field), suggestion toggles and per-engine
    // opt-ins.  The engine list itself (descriptors, keywords, order)
    // is untouched; the inline editor's own Restore Defaults covers
    // that.
    void resetSearchPreferences();

    // SRCH05: the Settings page's inline editor needs the engine list
    // to keep a stable, user-arrangeable order — allEnginesNames()
    // returns it (m_engineOrder first, stragglers appended), and the
    // order persists through save()/load().
    bool moveEngine(const QString &name, int offset);
    // Renames the engine keeping every name-keyed reference (current/
    // private/image/field picks, suggestion opt-ins) pointed at it.
    bool renameEngine(const QString &oldName, const QString &newName);
    // Marks an engine's fields as edited so the autosave persists them
    // (the engine object itself emits no change notification).
    void engineEdited(OpenSearchEngine *engine);

public slots:
    void save();

protected:
    void load();
    bool loadDirectory(const QString &dirName);
    void saveDirectory(const QString &dirName);
    QString enginesDirectory() const;
    QString generateEngineFileName(const QString &engineName) const;

private:
    bool confirmAddition(OpenSearchEngine *engine);
#if defined(ARORA_RUSTCORE)
    // OSE01: one-shot migration of the legacy descriptor dir +
    // QSettings openSearch keys into the rustcore searchengines.json
    // store — runs once on first load, then retires the old paths.
    void importLegacyRegistry(QSettings &settings);
#endif

protected slots:
    void engineFromUrlAvailable();

private:
    AutoSaver *m_autoSaver;

    QHash<QString, OpenSearchEngine*> m_engines;
    // SRCH05: display order of allEnginesNames() — user-arranged via
    // moveEngine, persisted as openSearch/engineOrder.
    QStringList m_engineOrder;
    QHash<QString, OpenSearchEngine*> m_keywords;
    QStringList m_suggestionsEnabled;
    // SRCH04: bundled engines the user removed — load() re-adds
    // missing bundled descriptors except these.
    QStringList m_removedBundled;
    QString m_current;
    QString m_privateEngine;
    QString m_imageEngine;
    QString m_fieldEngine;
    bool m_keepFieldEngine = true;
    bool m_suggestInAddressField = true;
    bool m_suggestInSearchField = true;
    bool m_suggestOnlyWithKeyword = false;
};

#endif //OPENSEARCHMANAGER_H

