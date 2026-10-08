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

#ifndef CONTAINERMANAGER_H
#define CONTAINERMANAGER_H

#include <qcolor.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qstring.h>

class QIcon;
class QWebEngineProfile;
class QWidget;

/*!
    Firefox-style containers — named, isolated browsing contexts.

    Each container owns a dedicated PERSISTENT QWebEngineProfile whose
    storage lives under <app data dir>/containers/<id>/, so cookies,
    localStorage/IndexedDB, service workers, the http cache and
    permissions are all partitioned by the engine itself.  The default
    container — the unlabeled browsing context — keeps using the
    shared "arora" profile and is identified by an empty id.

    The registry (id/name/color) is a QSettings "containers" group so
    it survives restarts; profiles themselves are materialized lazily
    by profileFor() and run through the exact same service attachment
    the normal profile gets (BrowserApplication::prepareProfile —
    cookie jar, scheme handlers, settings, download manager, adblock +
    privacy interceptor, extensions).

    Scope notes (see .devin/Arora-Task.md CONT01-05): tab assignment
    and the management UI land in CONT02/CONT03; this is the core —
    registry, lazy profiles and the isolation plumbing.
*/
class ContainerManager : public QObject
{
    Q_OBJECT

public:
    struct Container {
        QString id;
        QString name;
        QColor color;
    };

    explicit ContainerManager(QObject *parent = nullptr);
    ~ContainerManager();

    // Lazy qApp-owned singleton — same pattern as
    // HistoryManager::instance()/AdBlockManager::instance().
    static ContainerManager *instance();

    // The default (unlabeled) container's id.  Everything that is not
    // a registered container — including the normal "arora" profile —
    // maps to it.
    static QString defaultContainerId();

    // Registered containers in creation order.
    QList<Container> containers() const;
    bool isContainerId(const QString &id) const;
    Container containerForId(const QString &id) const;

    Container createContainer(const QString &name, const QColor &color);
    bool renameContainer(const QString &id, const QString &name);
    bool setContainerColor(const QString &id, const QColor &color);

    // Removes the registry entry AND the on-disk state: the
    // materialized profile (if any) is destroyed and its storage +
    // cache trees deleted recursively.  Callers must have closed
    // every page living on the container profile first — destroying a
    // profile with live WebContents is unsupported.  The default
    // container can never be deleted.
    bool deleteContainer(const QString &id);

    // The container's browsing profile, lazily created and prepared.
    // The default container resolves to BrowserProfile::normalProfile();
    // unknown ids, private browsing and tor mode resolve to nullptr —
    // containers are persistent state and must never be handed to an
    // off-the-record session (CONT05; isPrivate() covers tor).
    QWebEngineProfile *profileFor(const QString &id);
    // Same lookup without materializing — nullptr until profileFor()
    // has created it (or when it was released/deleted).
    QWebEngineProfile *profileIfCreated(const QString &id) const;
    // Every container profile materialized so far — the settings
    // re-apply loops (BrowserApplication::loadSettings, the settings
    // dialog, WebPage::setUserAgent) and the private-data wipes use
    // this so containers keep the full default-profile treatment.
    QList<QWebEngineProfile*> createdProfiles() const;
    // Reverse lookup for tab<->container binding (CONT02): the id a
    // profile belongs to — the default id for the normal profile and
    // for anything that is not a container profile.
    QString containerIdForProfile(QWebEngineProfile *profile) const;

    // <app data dir>/containers/<id> — the profile's persistent
    // storage root.  Only meaningful for registered ids.
    QString storagePath(const QString &id) const;

    // CONT05: the on-disk locations Qt derives from the container's
    // storage name — <CacheLocation>/QtWebEngine/<storageName> for the
    // http cache, and the mostly-empty default storage dir
    // <AppDataLocation>/QtWebEngine/<storageName> Qt creates at profile
    // construction before persistentStoragePath is overridden.  Both
    // are computed WITHOUT materializing the profile so deletion and
    // clear-data paths reach containers never used this session.
    QString cachePathFor(const QString &id) const;
    QString defaultDataPathFor(const QString &id) const;

    // CONT05: removes per-class on-disk data for every registered
    // container whose profile is NOT materialized — the live-profile
    // paths in ClearPrivateData only reach materialized ones.  Nothing
    // holds these trees open so removal is immediate (no deferred-wipe
    // sentinel); visitedLinks also removes Chromium's "Visited Links"
    // store.  Materialized containers are skipped — they are covered
    // by the per-profile clears.
    void clearUnmaterializedStorage(bool cookies, bool siteData,
                                    bool cache, bool visitedLinks);

    // CONT05: the clear-all-on-exit counterpart — removes every
    // on-disk tree of every unmaterialized container (storage root,
    // derived cache dir, derived default-storage dir).  The registry
    // entries stay; this is profile data only.
    void wipeUnmaterializedStorage();

    // Re-runs BrowserProfile::applySettings on every materialized
    // container profile — called from the settings save paths so a
    // settings change reaches live container profiles the same way it
    // reaches the normal and private ones.
    void reapplySettings();

    // Destroys the materialized profile without touching the registry
    // or on-disk state — the next profileFor() recreates it on the
    // same data.  Same no-live-pages contract as deleteContainer();
    // used by deleteContainer and by tests simulating a restart.
    void releaseProfileFor(const QString &id);

    // CONT02 UI helpers shared by the File menu and the tab context
    // menu: a small rounded swatch carrying a container's color for
    // menu entries, and the quick "name a new container" prompt
    // (returns the new container's id, empty when cancelled or in
    // tor mode).
    static QIcon colorIcon(const QColor &color);
    QString createContainerInteractive(QWidget *parent);
    // The accent palette containers rotate through when created
    // without an explicit color — also the tab-strip chip's fallback.
    static QList<QColor> defaultColors();

    // CONT04: "Always open this site in <container>" — persisted
    // host->container rules WebPage consults at navigation time.  A
    // rule on example.com covers every *.example.com subdomain; keys
    // are normalized (lowercased, one leading "www." and any trailing
    // dot stripped) so the stored form never duplicates a host.
    // Rules live inside the container's QSettings group, so deleting
    // a container takes its assignments with it.
    QStringList siteRules(const QString &id) const;
    bool setSiteRule(const QString &host, const QString &id);
    bool removeSiteRule(const QString &host);
    // The container a host is ruled into — the default (empty) id
    // when nothing matches or the ruling container no longer exists.
    QString containerIdForHost(const QString &host) const;

signals:
    void containersChanged();
    void siteRulesChanged();

private:
    void loadRegistry();
    void saveRegistry(const Container &container) const;
    void removeRegistry(const QString &id) const;
    void removeStorageTree(const QString &path) const;
    QString storageNameFor(const QString &id) const;
    // CONT04: canonical form of a rule key — see the siteRules()
    // comment for the normalization contract.
    static QString normalizeSiteHost(const QString &host);
    // Rewrites containers/<id>/sites from m_siteRules — the hash is
    // authoritative, the per-container list the persisted mirror.
    void saveSiteRules(const QString &id) const;

    QList<Container> m_containers;
    // Materialized profiles only, keyed by container id.  The default
    // container is never an entry — it lives in BrowserProfile.
    QHash<QString, QWebEngineProfile*> m_profiles;
    // CONT04: normalized host -> container id.
    QHash<QString, QString> m_siteRules;
};

#endif // CONTAINERMANAGER_H
