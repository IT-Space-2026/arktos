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

#include <QtTest/QtTest>
#include "qtest_arktos.h"

#include <adblockmanager.h>
#include <autofillmanager.h>
#include <bookmarknode.h>
#include <bookmarksmanager.h>
#include <browserpaths.h>
#include <browserprofile.h>
#include <containermanager.h>
#include <cookiejar.h>
#include <downloadmanager.h>
#include <extensionmanager.h>
#include <historymanager.h>
#include <notesmodel.h>
#include <profileidentity.h>
#include <profileregistry.h>
#include <profilestore.h>
#include <securestore.h>

#if defined(ARKTOS_RUSTCORE)
#include <sitedecisionstore.h>
#endif

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qpointer.h>
#include <qwebengineprofile.h>

// MPROF02: the per-persona app singletons.  Two registry profiles must
// get disjoint manager instances whose persisted data never crosses —
// including the process-global Rust stores that each manager re-anchors
// to its own profiles/<id>/ directory before touching them.
class tst_ProfileManagers : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void personaProfileTagging();
    void profileIdForFallbacks();
    void instanceForStability();
    void instanceAliasDefault();
    void invalidIdResolvesDefault();
    void pathDisjointness();
    void historyIsolation();
    void historyRustAnchor();
    void bookmarkIsolation();
    void notesIsolation();
    void secureStoreIsolation();
    void siteDecisionIsolation();
    void offTheRecordVolatile();

    // MEM10: deleting a persona releases its live world.
    void releasePersonaDestroysManagers();
    void releasePersonaChurn();
    void releasePersonaRefusesUnsafe();
    void deleteProfileReleasesPersona();
    void offTheRecordRelease();
};

static const QString kAlpha = QStringLiteral("alpha");
static const QString kBeta = QStringLiteral("beta");

static bool treeContainsUrl(BookmarkNode *node, const QString &url)
{
    if (!node)
        return false;
    if (node->url == url)
        return true;
    for (BookmarkNode *child : node->children()) {
        if (treeContainsUrl(child, url))
            return true;
    }
    return false;
}

void tst_ProfileManagers::initTestCase()
{
    // profiles/<id>/ layout must exist before any manager resolves
    // its persona dir.
    QVERIFY(BrowserPaths::ensureDataLayout());
}

void tst_ProfileManagers::personaProfileTagging()
{
    QWebEngineProfile *normal = BrowserProfile::normalProfile();
    QCOMPARE(BrowserProfile::profileIdFor(normal),
             QStringLiteral("default"));

    QWebEngineProfile *work = BrowserProfile::profileFor(kAlpha);
    QVERIFY(work);
    QCOMPARE(work->storageName(), QStringLiteral("arktos-alpha"));
    QCOMPARE(BrowserProfile::profileIdFor(work), kAlpha);
    // Lazily created once — repeat lookups return the same object.
    QCOMPARE(BrowserProfile::profileFor(kAlpha), work);
}

void tst_ProfileManagers::profileIdForFallbacks()
{
    // An off-the-record (unnamed) profile declares no persona — the
    // fallback lands on default without aliasing a named one.
    QWebEngineProfile otr;
    QVERIFY(otr.isOffTheRecord());
    QCOMPARE(BrowserProfile::profileIdFor(&otr),
             QStringLiteral("default"));
    QCOMPARE(BrowserProfile::profileIdFor(nullptr),
             QStringLiteral("default"));

    // A named foreign profile that is not one of ours stays default.
    QWebEngineProfile foreign(QStringLiteral("someone-else"));
    QCOMPARE(BrowserProfile::profileIdFor(&foreign),
             QStringLiteral("default"));

    // The dynamic tag wins over storage-name parsing — this is how a
    // container profile reports its owning persona.
    QWebEngineProfile tagged(QStringLiteral("container-x"));
    tagged.setProperty("arktosProfileId", kAlpha);
    QCOMPARE(BrowserProfile::profileIdFor(&tagged), kAlpha);
}

void tst_ProfileManagers::instanceForStability()
{
    // Every per-persona singleton: same id -> same pointer, different
    // id -> different pointer.
    QCOMPARE(HistoryManager::instanceFor(kAlpha),
             HistoryManager::instanceFor(kAlpha));
    QVERIFY(HistoryManager::instanceFor(kAlpha)
            != HistoryManager::instanceFor(kBeta));
    QCOMPARE(HistoryManager::instanceFor(kAlpha)->profileId(), kAlpha);

    QCOMPARE(BookmarksManager::instanceFor(kAlpha),
             BookmarksManager::instanceFor(kAlpha));
    QVERIFY(BookmarksManager::instanceFor(kAlpha)
            != BookmarksManager::instanceFor(kBeta));

    QCOMPARE(AutoFillManager::instanceFor(kAlpha),
             AutoFillManager::instanceFor(kAlpha));
    QVERIFY(AutoFillManager::instanceFor(kAlpha)
            != AutoFillManager::instanceFor(kBeta));

    QCOMPARE(AdBlockManager::instanceFor(kAlpha),
             AdBlockManager::instanceFor(kAlpha));
    QVERIFY(AdBlockManager::instanceFor(kAlpha)
            != AdBlockManager::instanceFor(kBeta));

    QCOMPARE(ContainerManager::instanceFor(kAlpha),
             ContainerManager::instanceFor(kAlpha));
    QVERIFY(ContainerManager::instanceFor(kAlpha)
            != ContainerManager::instanceFor(kBeta));

    QCOMPARE(ExtensionManager::instanceFor(kAlpha),
             ExtensionManager::instanceFor(kAlpha));
    QVERIFY(ExtensionManager::instanceFor(kAlpha)
            != ExtensionManager::instanceFor(kBeta));

    QCOMPARE(DownloadManager::instanceFor(kAlpha),
             DownloadManager::instanceFor(kAlpha));
    QVERIFY(DownloadManager::instanceFor(kAlpha)
            != DownloadManager::instanceFor(kBeta));

    QCOMPARE(NotesModel::instanceFor(kAlpha),
             NotesModel::instanceFor(kAlpha));
    QVERIFY(NotesModel::instanceFor(kAlpha)
            != NotesModel::instanceFor(kBeta));

    QCOMPARE(CookieJar::instanceFor(kAlpha),
             CookieJar::instanceFor(kAlpha));
    QVERIFY(CookieJar::instanceFor(kAlpha)
            != CookieJar::instanceFor(kBeta));
    QCOMPARE(CookieJar::instanceFor(kAlpha)->profileId(), kAlpha);
}

void tst_ProfileManagers::instanceAliasDefault()
{
    // The legacy instance() API aliases the active profile — with no
    // window bound (MPROF03), the ambient persona is "default".
    QCOMPARE(BrowserPaths::activeProfileId(),
             QStringLiteral("default"));
    QCOMPARE(HistoryManager::instance(),
             HistoryManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(BookmarksManager::instance(),
             BookmarksManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(AutoFillManager::instance(),
             AutoFillManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(AdBlockManager::instance(),
             AdBlockManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(ContainerManager::instance(),
             ContainerManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(ExtensionManager::instance(),
             ExtensionManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(DownloadManager::instance(),
             DownloadManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(NotesModel::instanceFor(QStringLiteral("default")),
             NotesModel::instance(BrowserProfile::normalProfile()));
}

void tst_ProfileManagers::invalidIdResolvesDefault()
{
    // A non-slug id can never name a store — the same normalization
    // profileDataDir() applies.
    HistoryManager *fallback =
        HistoryManager::instanceFor(QStringLiteral("../etc"));
    QCOMPARE(fallback, HistoryManager::instanceFor(
                 QStringLiteral("default")));
    QCOMPARE(BookmarksManager::instanceFor(QString()),
             BookmarksManager::instanceFor(QStringLiteral("default")));
}

void tst_ProfileManagers::pathDisjointness()
{
    const QString alphaDir = BrowserPaths::profileDataDir(kAlpha);
    const QString betaDir = BrowserPaths::profileDataDir(kBeta);
    QVERIFY(alphaDir.endsWith(QLatin1String("/profiles/alpha")));
    QVERIFY(betaDir.endsWith(QLatin1String("/profiles/beta")));
    QVERIFY(alphaDir != betaDir);
    QVERIFY(BrowserPaths::profileDataFilePath(kAlpha,
                                              QLatin1String("x"))
            != BrowserPaths::profileDataFilePath(kBeta,
                                                 QLatin1String("x")));
}

void tst_ProfileManagers::historyIsolation()
{
    HistoryManager *alpha = HistoryManager::instanceFor(kAlpha);
    HistoryManager *beta = HistoryManager::instanceFor(kBeta);

    const QString urlA = QStringLiteral(
        "https://alpha-only.example/never-in-beta");
    const QString urlB = QStringLiteral(
        "https://beta-only.example/never-in-alpha");

    // Create beta first so the process-global Rust history store is
    // anchored to beta when alpha writes — the write must re-anchor,
    // never leak into the wrong database.
    alpha->addHistoryEntry(urlA);
    beta->addHistoryEntry(urlB);

    QVERIFY(alpha->historyContains(urlA));
    QVERIFY(!alpha->historyContains(urlB));
    QVERIFY(beta->historyContains(urlB));
    QVERIFY(!beta->historyContains(urlA));

    // Alternating back — beta's own writes after alpha's must not
    // retro-contaminate either.
    beta->addHistoryEntry(urlB + QLatin1String("/2"));
    QVERIFY(!alpha->historyContains(urlB + QLatin1String("/2")));
}

void tst_ProfileManagers::historyRustAnchor()
{
    HistoryManager *alpha = HistoryManager::instanceFor(kAlpha);
    const QString url = QStringLiteral(
        "https://anchored.example/in-alpha-db");
    alpha->addHistoryEntry(url);

    // The entry must land in alpha's own history database — never in
    // whatever profile the Rust store happened to be anchored to.
    const QString dbPath = BrowserPaths::profileDataFilePath(
        kAlpha, QLatin1String("history.db"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dbPath), 5000);

    // A freshly anchored read of beta's store still cannot see it.
    HistoryManager *beta = HistoryManager::instanceFor(kBeta);
    QVERIFY(!beta->historyContains(url));

    // And the beta-side database file — if it exists — must not carry
    // alpha's row.  historyContains already proved the query side;
    // the on-disk check guards the write side.
    const QString betaDb = BrowserPaths::profileDataFilePath(
        kBeta, QLatin1String("history.db"));
    if (QFile::exists(betaDb)) {
        QFile file(betaDb);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("anchored.example"));
    }
}

void tst_ProfileManagers::bookmarkIsolation()
{
    BookmarksManager *alpha = BookmarksManager::instanceFor(kAlpha);
    BookmarksManager *beta = BookmarksManager::instanceFor(kBeta);

    const QString url = QStringLiteral(
        "https://alpha-bookmark.example/only");

    // Beta binds the process-global tree first; alpha's addBookmark
    // must swap the binding before touching it.
    QVERIFY(beta->menu());
    BookmarkNode *node = new BookmarkNode(BookmarkNode::Bookmark);
    node->title = QStringLiteral("alpha bookmark");
    node->url = url;
    alpha->addBookmark(alpha->menu(), node);

    QVERIFY(treeContainsUrl(alpha->bookmarks(), url));

    // Re-binding back to beta must show beta's own tree — the alpha
    // node was never written into it.  The unbind also flushes alpha's
    // pending save, so alpha's file exists right after this call.
    QVERIFY(!treeContainsUrl(beta->bookmarks(), url));

    // The write landed in alpha's persona dir, not beta's.
    const QString alphaFile = BrowserPaths::profileDataFilePath(
        kAlpha, QLatin1String("bookmarks.xbel"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(alphaFile), 5000);
    if (QFile::exists(alphaFile)) {
        QFile file(alphaFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("alpha-bookmark.example"));
    }
    const QString betaFile = BrowserPaths::profileDataFilePath(
        kBeta, QLatin1String("bookmarks.xbel"));
    if (QFile::exists(betaFile)) {
        QFile file(betaFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("alpha-bookmark.example"));
    }
}

void tst_ProfileManagers::notesIsolation()
{
    NotesModel *alpha = NotesModel::instanceFor(kAlpha);
    NotesModel *beta = NotesModel::instanceFor(kBeta);
    QVERIFY(alpha->isPersistent());
    QVERIFY(beta->isPersistent());
    QVERIFY(alpha->storageFile().contains(
        QLatin1String("/profiles/alpha/")));
    QVERIFY(beta->storageFile().contains(
        QLatin1String("/profiles/beta/")));

    const int before = alpha->rowCount();
    alpha->addNote(QStringLiteral("alpha note"),
                   QStringLiteral("secret body"));
    QCOMPARE(alpha->rowCount(), before + 1);
    for (int i = 0; i < beta->rowCount(); ++i)
        QVERIFY(beta->noteBody(i) != QLatin1String("secret body"));
}

void tst_ProfileManagers::secureStoreIsolation()
{
    if (SecureStore::passphraseProtectionEnabled(kAlpha)
        || SecureStore::passphraseProtectionEnabled(kBeta))
        QSKIP("passphrase protection is enabled for a test profile");

    const QByteArray plain = QByteArrayLiteral("alpha secret payload");
    const QByteArray blob = SecureStore::seal(kAlpha, plain);
    QVERIFY(!blob.isEmpty());

    bool ok = false;
    QCOMPARE(SecureStore::open(kAlpha, blob, &ok), plain);
    QVERIFY(ok);

    // Sealed under alpha's key — beta's store cannot open it.
    ok = true;
    const QByteArray opened = SecureStore::open(kBeta, blob, &ok);
    QVERIFY(!ok);
    QVERIFY(opened.isEmpty());

    // Alpha's key material lives under alpha's persona dir.
    QVERIFY(QFile::exists(BrowserPaths::profileDataFilePath(
        kAlpha, QLatin1String("securestore.key"))));
}

void tst_ProfileManagers::siteDecisionIsolation()
{
#if defined(ARKTOS_RUSTCORE)
    const QString host = QStringLiteral("alpha-cookie.example");
    QVERIFY(SiteDecisionStore::set(kAlpha,
                                   SiteDecisionStore::KindCookie,
                                   host, QStringLiteral("block")));

    QString value;
    QVERIFY(SiteDecisionStore::get(kAlpha,
                                   SiteDecisionStore::KindCookie,
                                   host, &value));
    QCOMPARE(value, QStringLiteral("block"));

    // The same kind + key under beta resolves to an empty slot — the
    // process-global Rust store was re-anchored, not shared.
    QVERIFY(!SiteDecisionStore::get(kBeta,
                                    SiteDecisionStore::KindCookie,
                                    host, &value));

    // On-disk: only alpha's sitedecisions.json carries the row.
    const QString alphaStore = BrowserPaths::profileDataFilePath(
        kAlpha, QLatin1String("sitedecisions.json"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(alphaStore), 5000);
    QFile alphaFile(alphaStore);
    QVERIFY(alphaFile.open(QIODevice::ReadOnly));
    QVERIFY(alphaFile.readAll().contains("alpha-cookie.example"));

    const QString betaStore = BrowserPaths::profileDataFilePath(
        kBeta, QLatin1String("sitedecisions.json"));
    if (QFile::exists(betaStore)) {
        QFile betaFile(betaStore);
        QVERIFY(betaFile.open(QIODevice::ReadOnly));
        QVERIFY(!betaFile.readAll().contains("alpha-cookie.example"));
    }
#else
    QSKIP("site decision store requires ARKTOS_RUSTCORE");
#endif
}

void tst_ProfileManagers::offTheRecordVolatile()
{
    QWebEngineProfile *otr = BrowserProfile::privateProfile();
    QVERIFY(otr->isOffTheRecord());

    // Notes for an off-the-record profile are a session-only model —
    // nothing is written to any persona's store.
    NotesModel *notes = NotesModel::instance(otr);
    QVERIFY(notes);
    QVERIFY(!notes->isPersistent());
    QVERIFY(notes->storageFile().isEmpty());
    QCOMPARE(NotesModel::instance(otr), notes);

    // A private jar reports private and never persists policy.
    CookieJar *jar = CookieJar::instance(otr);
    QVERIFY(jar->isPrivate());
}

void tst_ProfileManagers::releasePersonaDestroysManagers()
{
    // Materialize a persona's whole live world, then release it —
    // every instanceFor() map entry, the jar and the named engine
    // profile die with it.
    const QString id = QStringLiteral("release");
    QPointer<QWebEngineProfile> profile = BrowserProfile::profileFor(id);
    QVERIFY(profile);

    QPointer<HistoryManager> history = HistoryManager::instanceFor(id);
    QPointer<BookmarksManager> bookmarks =
        BookmarksManager::instanceFor(id);
    QPointer<AutoFillManager> autofill = AutoFillManager::instanceFor(id);
    QPointer<AdBlockManager> adblock = AdBlockManager::instanceFor(id);
    QPointer<ContainerManager> containers =
        ContainerManager::instanceFor(id);
    QPointer<ExtensionManager> extensions =
        ExtensionManager::instanceFor(id);
    QPointer<DownloadManager> downloads =
        DownloadManager::instanceFor(id);
    QPointer<NotesModel> notes = NotesModel::instanceFor(id);
    QPointer<ProfileIdentity> identity = ProfileIdentity::instanceFor(id);
    QPointer<CookieJar> jar = CookieJar::instanceFor(id);
    QVERIFY(history && bookmarks && autofill && adblock && containers
            && extensions && downloads && notes && identity && jar);

    QVERIFY(BrowserProfile::releasePersona(id));

    QVERIFY(!profile);
    QVERIFY(!history);
    QVERIFY(!bookmarks);
    QVERIFY(!autofill);
    QVERIFY(!adblock);
    QVERIFY(!containers);
    QVERIFY(!extensions);
    QVERIFY(!downloads);
    QVERIFY(!notes);
    QVERIFY(!identity);
    QVERIFY(!jar);
    QVERIFY(!BrowserProfile::personaProfileIfCreated(id));

    // Re-materializing the persona builds a fresh generation — the
    // released objects are never revived or aliased.
    QVERIFY(BrowserProfile::profileFor(id));
    QVERIFY(HistoryManager::instanceFor(id) != history.data());
    QVERIFY(BrowserProfile::releasePersona(id));
}

void tst_ProfileManagers::releasePersonaChurn()
{
    // Many personas materialized and released in sequence — every
    // generation must die on release, and a second round on the same
    // ids re-materializes fresh objects rather than reviving stale
    // ones.
    for (int round = 0; round < 2; ++round) {
        QList<QPointer<ProfileIdentity>> generation;
        for (int i = 0; i < 8; ++i) {
            const QString id = QStringLiteral("churn%1").arg(i);
            generation.append(ProfileIdentity::instanceFor(id));
            QVERIFY(HistoryManager::instanceFor(id));
        }
        for (int i = 0; i < 8; ++i)
            QVERIFY(BrowserProfile::releasePersona(
                QStringLiteral("churn%1").arg(i)));
        for (const QPointer<ProfileIdentity> &identity : generation)
            QVERIFY(!identity);
    }
}

void tst_ProfileManagers::releasePersonaRefusesUnsafe()
{
    // "default" is the app's own persona — its stores never release.
    QPointer<HistoryManager> history =
        HistoryManager::instanceFor(QStringLiteral("default"));
    QVERIFY(history);
    QVERIFY(!BrowserProfile::releasePersona(QStringLiteral("default")));
    QVERIFY(history);
    // Non-slug ids are refused outright — no path through.
    QVERIFY(!BrowserProfile::releasePersona(QString()));
    QVERIFY(!BrowserProfile::releasePersona(
        QStringLiteral("../escape")));
    // A persona that was never materialized is a no-op success.
    QVERIFY(BrowserProfile::releasePersona(
        QStringLiteral("neverexisted")));
}

void tst_ProfileManagers::deleteProfileReleasesPersona()
{
    // The MPROF02 contract end to end: ProfileStore::deleteProfile
    // releases the persona's live objects before the dir goes away.
    ProfileStore *store = ProfileStore::instance();
    const ProfileContext context =
        store->createProfile(QStringLiteral("MEM10 Delete"));
    QVERIFY(context.isValid());
    const QString id = context.id;

    QPointer<HistoryManager> history = HistoryManager::instanceFor(id);
    QPointer<ProfileIdentity> identity = ProfileIdentity::instanceFor(id);
    QVERIFY(history && identity);

    QVERIFY(store->deleteProfile(id));
    QVERIFY(!history);
    QVERIFY(!identity);
    // The MPROF01 registry stopped resolving the deleted persona.
    QVERIFY(!ProfileRegistry::instance()->profile(id).isValid());
}

void tst_ProfileManagers::offTheRecordRelease()
{
    // A short-lived OTR profile: its volatile notes model and its jar
    // must die with it — nothing may linger keyed on the dead address
    // to alias a recycled profile object.
    QWebEngineProfile *otr = new QWebEngineProfile;
    QPointer<NotesModel> notes = NotesModel::instance(otr);
    QPointer<CookieJar> jar = CookieJar::instance(otr);
    QVERIFY(notes && jar);
    QVERIFY(jar->isPrivate());
    delete otr;
    QVERIFY(!notes);
    QVERIFY(!jar);
}

QTEST_MAIN(tst_ProfileManagers)
#include "tst_profilemanagers.moc"
