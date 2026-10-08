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

#include "containermanager.h"

#include "browserapplication.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "twoleveldomains_p.h"

#include <qapplication.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qicon.h>
#include <qinputdialog.h>
#include <qlineedit.h>
#include <qpainter.h>
#include <qpixmap.h>
#include <qsettings.h>
#include <qthread.h>
#include <quuid.h>
#include <qwebengineprofile.h>

ContainerManager::ContainerManager(QObject *parent)
    : QObject(parent)
{
    loadRegistry();
}

ContainerManager::~ContainerManager()
{
    // Profiles are qApp-owned; the manager only maps them.  Nothing to
    // free here — destroying profiles at app teardown is Qt's job.
}

ContainerManager *ContainerManager::instance()
{
    static ContainerManager *manager = new ContainerManager(qApp);
    return manager;
}

QString ContainerManager::defaultContainerId()
{
    return QString();
}

QList<ContainerManager::Container> ContainerManager::containers() const
{
    return m_containers;
}

bool ContainerManager::isContainerId(const QString &id) const
{
    for (const Container &container : m_containers) {
        if (container.id == id)
            return true;
    }
    return false;
}

ContainerManager::Container ContainerManager::containerForId(const QString &id) const
{
    for (const Container &container : m_containers) {
        if (container.id == id)
            return container;
    }
    return Container();
}

ContainerManager::Container ContainerManager::createContainer(
        const QString &name, const QColor &color)
{
    Container container;
    // A uuid keeps the id opaque and collision-free; hex is safe both
    // as a QSettings group and as a directory name.
    container.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    container.name = name;
    container.color = color;
    m_containers.append(container);
    saveRegistry(container);

    QSettings settings;
    settings.beginGroup(QLatin1String("containers"));
    QStringList order = settings.value(QLatin1String("order")).toStringList();
    order.append(container.id);
    settings.setValue(QLatin1String("order"), order);

    emit containersChanged();
    return container;
}

bool ContainerManager::renameContainer(const QString &id, const QString &name)
{
    for (int i = 0; i < m_containers.count(); ++i) {
        if (m_containers.at(i).id != id)
            continue;
        if (m_containers.at(i).name == name)
            return true;
        m_containers[i].name = name;
        saveRegistry(m_containers.at(i));
        emit containersChanged();
        return true;
    }
    return false;
}

bool ContainerManager::setContainerColor(const QString &id, const QColor &color)
{
    for (int i = 0; i < m_containers.count(); ++i) {
        if (m_containers.at(i).id != id)
            continue;
        if (m_containers.at(i).color == color)
            return true;
        m_containers[i].color = color;
        saveRegistry(m_containers.at(i));
        emit containersChanged();
        return true;
    }
    return false;
}

bool ContainerManager::deleteContainer(const QString &id)
{
    if (id.isEmpty() || !isContainerId(id))
        return false;

    QString storagePath;
    QString cachePath;
    if (QWebEngineProfile *profile = m_profiles.take(id)) {
        storagePath = profile->persistentStoragePath();
        cachePath = profile->cachePath();
        delete profile;
    }
    if (storagePath.isEmpty())
        storagePath = this->storagePath(id);

    for (int i = m_containers.count() - 1; i >= 0; --i) {
        if (m_containers.at(i).id == id)
            m_containers.removeAt(i);
    }
    removeRegistry(id);

    // CONT04: site rules die with their container — the persisted
    // list went away with the registry group, only the hash mirror
    // needs purging.
    for (auto it = m_siteRules.begin(); it != m_siteRules.end();) {
        if (it.value() == id)
            it = m_siteRules.erase(it);
        else
            ++it;
    }

    removeStorageTree(storagePath);
    if (!cachePath.isEmpty())
        removeStorageTree(cachePath);

    emit containersChanged();
    emit siteRulesChanged();
    return true;
}

// Chromium's storage-partition teardown runs on background threads and
// can still be writing (cache/visited-links/cookie commits) after the
// QWebEngineProfile object is gone, so a single removeRecursively can
// race files reappearing under it.  Retry over a bounded window —
// callers expect the tree to actually be gone when we return.
void ContainerManager::removeStorageTree(const QString &path) const
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        QDir(path).removeRecursively();
        if (!QDir(path).exists())
            return;
        QThread::msleep(125);
        QCoreApplication::processEvents();
    }
    qWarning() << "containermanager: could not remove" << path;
}

QWebEngineProfile *ContainerManager::profileFor(const QString &id)
{
    // CONT05: containers are persistent state and can never live on
    // the tor profile — a tor process hands out no container at all,
    // not even the "default container" notion (its profile IS a
    // persistent store).
    if (BrowserApplication::isTorMode())
        return nullptr;
    // The default container IS the normal browsing profile — no
    // separate storage, all existing state belongs to it.
    if (id.isEmpty()) {
        QWebEngineProfile *profile = BrowserProfile::normalProfile();
        BrowserApplication::prepareProfile(profile);
        return profile;
    }
    if (QWebEngineProfile *profile = m_profiles.value(id))
        return profile;
    if (!isContainerId(id))
        return nullptr;

    const QString path = storagePath(id);
    QDir().mkpath(path);

    // A unique storage name keeps every derived default path —
    // persistentStoragePath is overridden below but cachePath,
    // dictionaries and download state all key off the name — isolated
    // per container and unable to collide with "arora".
    QWebEngineProfile *profile =
        new QWebEngineProfile(storageNameFor(id), qApp);
    profile->setPersistentStoragePath(path);
    // Parity with normalProfile(): honor a deferred site-data wipe
    // sentinel left by a previous session before the storage services
    // open these trees (HARD01 — only safe before any WebContents).
    BrowserProfile::clearDeferredSiteStorage(path);
    // The full app-service attach — cookie jar, scheme handlers,
    // persisted settings, download manager, adblock + privacy
    // interceptor, extensions — identical to what webEngineProfile()
    // hands out.
    BrowserApplication::prepareProfile(profile);
    m_profiles.insert(id, profile);
    return profile;
}

QWebEngineProfile *ContainerManager::profileIfCreated(const QString &id) const
{
    // The default container's profile always resolves — it is the
    // lazily-created normal profile, not a container entry.
    if (id.isEmpty())
        return BrowserProfile::normalProfile();
    return m_profiles.value(id);
}

QList<QWebEngineProfile*> ContainerManager::createdProfiles() const
{
    return m_profiles.values();
}

QString ContainerManager::containerIdForProfile(QWebEngineProfile *profile) const
{
    if (!profile)
        return QString();
    const QList<QString> ids = m_profiles.keys();
    for (const QString &id : ids) {
        if (m_profiles.value(id) == profile)
            return id;
    }
    return QString();
}

QString ContainerManager::storagePath(const QString &id) const
{
    return BrowserPaths::dataFilePath(QLatin1String("containers"))
        + QLatin1Char('/') + id;
}

void ContainerManager::reapplySettings()
{
    const QList<QWebEngineProfile*> profiles = m_profiles.values();
    for (QWebEngineProfile *profile : profiles)
        BrowserProfile::applySettings(profile);
}

void ContainerManager::releaseProfileFor(const QString &id)
{
    delete m_profiles.take(id);
}

// Firefox's container accent palette — blue, turquoise, green,
// yellow, orange, red, pink, purple.
QList<QColor> ContainerManager::defaultColors()
{
    return { QColor(0x37, 0xad, 0xff), QColor(0x00, 0xc7, 0x9a),
             QColor(0x51, 0xcd, 0x00), QColor(0xff, 0xcb, 0x00),
             QColor(0xff, 0x9f, 0x00), QColor(0xff, 0x61, 0x3d),
             QColor(0xff, 0x4b, 0xda), QColor(0xaf, 0x51, 0xf5) };
}

QIcon ContainerManager::colorIcon(const QColor &color)
{
    if (!color.isValid())
        return QIcon();
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(color.darker(160));
    painter.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 4, 4);
    return QIcon(pixmap);
}

QString ContainerManager::createContainerInteractive(QWidget *parent)
{
    // Tor mode hands out no containers — the prompt would create a
    // registry entry nothing could ever use.
    if (BrowserApplication::isTorMode())
        return QString();
    bool ok = false;
    const QString name = QInputDialog::getText(parent, tr("New Container"),
        tr("Container name:"), QLineEdit::Normal, QString(), &ok);
    const QString trimmed = name.trimmed();
    if (!ok || trimmed.isEmpty())
        return QString();
    const QList<QColor> palette = defaultColors();
    const Container created = createContainer(trimmed,
        palette.value(m_containers.count() % palette.count()));
    return created.id;
}

// True when host itself is a public suffix under the bundled
// two-level TLD heuristic (same list the location bar and the
// network cookie jar use): a bare TLD, or a <label>.<tld> pair whose
// tld takes third-level registrations (co.uk and friends).
static bool hostIsPublicSuffix(const QString &host)
{
    const int lastDot = host.lastIndexOf(QLatin1Char('.'));
    if (lastDot < 0)
        return true;
    const QStringView tld = QStringView(host).mid(lastDot + 1);
    bool twoLevel = false;
    for (int i = 0; twoLevelDomains[i]; ++i) {
        if (!tld.compare(QLatin1String(twoLevelDomains[i]),
                         Qt::CaseInsensitive)) {
            twoLevel = true;
            break;
        }
    }
    return twoLevel && host.count(QLatin1Char('.')) < 2;
}

// Canonical rule key: lowercased, trailing root dot dropped, and one
// literal "www." prefix folded away — a rule set while standing on
// www.example.com anchors at example.com so it covers both the apex
// and the www host (the subdomain walk in containerIdForHost does the
// rest).  The fold never lands on a public suffix — www.com or
// www.co.uk keep their full host as the key rather than claiming an
// entire suffix.  Anything that cannot name a host (empty, or
// carrying characters a QUrl host never produces) maps to no rule.
QString ContainerManager::normalizeSiteHost(const QString &host)
{
    QString normalized = host.toLower();
    while (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    if (normalized.startsWith(QLatin1String("www."))) {
        const QString remainder = normalized.mid(4);
        if (!hostIsPublicSuffix(remainder))
            normalized = remainder;
    }
    for (const QChar &c : normalized) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('.')
            && c != QLatin1Char('-') && c != QLatin1Char(':')
            && c != QLatin1Char('[') && c != QLatin1Char(']'))
            return QString();
    }
    return normalized;
}

QStringList ContainerManager::siteRules(const QString &id) const
{
    QStringList hosts;
    for (auto it = m_siteRules.constBegin(); it != m_siteRules.constEnd(); ++it) {
        if (it.value() == id)
            hosts.append(it.key());
    }
    hosts.sort();
    return hosts;
}

bool ContainerManager::setSiteRule(const QString &host, const QString &id)
{
    const QString key = normalizeSiteHost(host);
    if (key.isEmpty() || !isContainerId(id))
        return false;
    const QString previous = m_siteRules.value(key);
    if (previous == id)
        return true;
    // Reassignment moves the host between containers — both lists
    // are rewritten so the persisted mirror stays exact.
    m_siteRules.insert(key, id);
    if (!previous.isEmpty())
        saveSiteRules(previous);
    saveSiteRules(id);
    emit siteRulesChanged();
    return true;
}

bool ContainerManager::removeSiteRule(const QString &host)
{
    const QString key = normalizeSiteHost(host);
    const QString id = m_siteRules.take(key);
    if (id.isEmpty())
        return false;
    saveSiteRules(id);
    emit siteRulesChanged();
    return true;
}

QString ContainerManager::containerIdForHost(const QString &host) const
{
    // Exact match first, then the parent-domain walk: a rule on
    // example.com covers *.example.com, and a rule on
    // sub.example.com covers deeper names only — the apex is never
    // claimed by a subdomain rule.
    QString candidate = normalizeSiteHost(host);
    while (!candidate.isEmpty()) {
        const QString id = m_siteRules.value(candidate);
        if (!id.isEmpty() && isContainerId(id))
            return id;
        const int dot = candidate.indexOf(QLatin1Char('.'));
        if (dot < 0)
            break;
        candidate = candidate.mid(dot + 1);
    }
    return QString();
}

void ContainerManager::saveSiteRules(const QString &id) const
{
    if (!isContainerId(id))
        return;
    QSettings settings;
    settings.beginGroup(QLatin1String("containers"));
    settings.beginGroup(id);
    settings.setValue(QLatin1String("sites"), siteRules(id));
}

// QSettings "containers" group:
//   order            QStringList of ids in creation order
//   <id>/name        user-visible name
//   <id>/color       QColor
//   <id>/sites       QStringList of hosts assigned to the container
//                    (CONT04 "always open in this container" rules)
// Ids are generated uuids, so they are always safe group names.
void ContainerManager::loadRegistry()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("containers"));
    const QStringList order =
        settings.value(QLatin1String("order")).toStringList();
    const QStringList groups = settings.childGroups();

    QSet<QString> seen;
    for (const QString &id : order) {
        if (id.isEmpty() || seen.contains(id) || !groups.contains(id))
            continue;
        seen.insert(id);
        settings.beginGroup(id);
        Container container;
        container.id = id;
        container.name = settings.value(QLatin1String("name")).toString();
        container.color = settings.value(QLatin1String("color")).value<QColor>();
        const QStringList sites =
            settings.value(QLatin1String("sites")).toStringList();
        settings.endGroup();
        m_containers.append(container);
        for (const QString &site : sites) {
            const QString key = normalizeSiteHost(site);
            if (!key.isEmpty())
                m_siteRules.insert(key, id);
        }
    }
    // Registry hygiene: a container group that never made the order
    // list (interrupted write) is stale — drop it.
    for (const QString &group : groups) {
        if (!seen.contains(group))
            settings.remove(group);
    }
}

void ContainerManager::saveRegistry(const Container &container) const
{
    QSettings settings;
    settings.beginGroup(QLatin1String("containers"));
    settings.beginGroup(container.id);
    settings.setValue(QLatin1String("name"), container.name);
    settings.setValue(QLatin1String("color"), container.color);
}

void ContainerManager::removeRegistry(const QString &id) const
{
    QSettings settings;
    settings.beginGroup(QLatin1String("containers"));
    settings.remove(id);
    QStringList order = settings.value(QLatin1String("order")).toStringList();
    order.removeAll(id);
    settings.setValue(QLatin1String("order"), order);
}

QString ContainerManager::storageNameFor(const QString &id) const
{
    return QLatin1String("arora-container-") + id;
}
