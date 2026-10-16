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

// MPROF05: dedicated coverage for the Profile Manager surface and its
// lane-local registry — the create/rename/delete/open-window legs,
// the default-undeletable guard, and per-profile identity storage.
// The store ctor takes an explicit profiles.json path so every leg
// runs against a hermetic tree under QTemporaryDir.

#include "qtest_arora.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "profilecontext.h"
#include "profileidentity.h"
#include "profilemanagerdialog.h"
#include "profilestore.h"

#include <qapplication.h>
#include <qdir.h>
#include <qinputdialog.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlineedit.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>
#include <qtreewidget.h>

class tst_ProfileManager : public QObject
{
    Q_OBJECT

private slots:
    void registryBasics();
    void createRenameDelete();
    void defaultUndeletable();
    void avatarPerProfile();
    void identitySync();
    void managerDialogLegs();
};

// Reads the persisted registry straight off disk — the layout must
// match what MPROF01's real ProfileRegistry reads after the merge.
static QJsonObject registryJson(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

void tst_ProfileManager::registryBasics()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString registryPath = dir.filePath(QLatin1String("profiles.json"));
    ProfileStore store(registryPath);

    // Always at least the default profile — a missing file
    // materializes the registry on first read.
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.profiles().first().id, QLatin1String("default"));
    QVERIFY(QFile::exists(registryPath));

    // On-disk contract: {"version":1,"profiles":[{id,name,avatar,
    // createdAt}]} — byte-compatible with MPROF01's writer.
    const QJsonObject root = registryJson(registryPath);
    QCOMPARE(root.value(QLatin1String("version")).toInt(), 1);
    const QJsonArray entries =
        root.value(QLatin1String("profiles")).toArray();
    QCOMPARE(entries.size(), 1);
    const QJsonObject entry = entries.first().toObject();
    QCOMPARE(entry.value(QLatin1String("id")).toString(),
             QLatin1String("default"));
    QVERIFY(entry.contains(QLatin1String("name")));
    QVERIFY(entry.contains(QLatin1String("avatar")));
    QVERIFY(entry.contains(QLatin1String("createdAt")));

    // Slug + id rules.
    QCOMPARE(ProfileStore::slugify(QLatin1String("Work Stuff!")),
             QLatin1String("work-stuff"));
    QCOMPARE(ProfileStore::slugify(QLatin1String("!!!")),
             QLatin1String("profile"));
    QVERIFY(ProfileStore::isValidProfileId(QLatin1String("a-b_1")));
    QVERIFY(!ProfileStore::isValidProfileId(QLatin1String("../etc")));
    QVERIFY(!ProfileStore::isValidProfileId(QLatin1String("Caps")));
    QVERIFY(!ProfileStore::isValidProfileId(QString()));

    // Path seam: profiles/<id>/ under the registry's root; a bad id
    // falls back to default instead of escaping the tree.
    QVERIFY(store.profileDir(QLatin1String("default"))
            .endsWith(QLatin1String("/profiles/default")));
    QCOMPARE(store.profileDir(QLatin1String("../escape")),
             store.profileDir(ProfileStore::defaultProfileId()));

    // The seams MPROF03 rewires are honest about this lane's limits.
    QVERIFY(!ProfileStore::windowBindingAvailable());
    QCOMPARE(ProfileStore::activeProfileId(), QLatin1String("default"));
    QVERIFY(store.activeProfile().isValid());
}

void tst_ProfileManager::createRenameDelete()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    QSignalSpy listSpy(&store, &ProfileStore::profilesChanged);
    QSignalSpy changeSpy(&store, &ProfileStore::profileChanged);

    // Create: registry row + profiles/<id>/ land together.
    const ProfileContext work = store.createProfile(QLatin1String("Work"));
    QVERIFY(work.isValid());
    QCOMPARE(work.id, QLatin1String("work"));
    QVERIFY(QFile::exists(store.profileDir(QLatin1String("work"))));
    QCOMPARE(listSpy.count(), 1);
    store.reload();
    QCOMPARE(store.profile(QLatin1String("work")).name,
             QLatin1String("Work"));

    // A duplicate display name still gets a unique id.
    const ProfileContext dup = store.createProfile(QLatin1String("Work"));
    QVERIFY(dup.isValid());
    QCOMPARE(dup.id, QLatin1String("work-2"));

    // A blank name produces nothing — no row, no dir.
    QVERIFY(!store.createProfile(QLatin1String("   ")).isValid());
    QVERIFY(!QFile::exists(dir.filePath(
        QLatin1String("profiles/profile"))));

    // Rename persists to disk.
    QVERIFY(store.renameProfile(QLatin1String("work"),
                                QLatin1String("Day Job")));
    QCOMPARE(changeSpy.count(), 1);
    store.reload();
    QCOMPARE(store.profile(QLatin1String("work")).name,
             QLatin1String("Day Job"));

    // Rename of an unknown id fails without writing.
    QVERIFY(!store.renameProfile(QLatin1String("ghost"),
                                 QLatin1String("x")));

    // Delete removes row and data dir, and reload confirms.
    QVERIFY(store.deleteProfile(QLatin1String("work-2")));
    QVERIFY(!store.profile(QLatin1String("work-2")).isValid());
    QVERIFY(!QFile::exists(store.profileDir(QLatin1String("work-2"))));
    QVERIFY(!store.deleteProfile(QLatin1String("work-2")));
    QVERIFY(!store.deleteProfile(QLatin1String("not a slug")));
}

void tst_ProfileManager::defaultUndeletable()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    QVERIFY(!store.deleteProfile(QLatin1String("default")));
    QVERIFY(!store.deleteProfile(QLatin1String("./default")));
    QVERIFY(store.profile(QLatin1String("default")).isValid());
}

void tst_ProfileManager::avatarPerProfile()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    QVERIFY(store.createProfile(QLatin1String("Alpha")).isValid());
    QVERIFY(store.createProfile(QLatin1String("Beta")).isValid());

    // A real source image on disk.
    QImage image(64, 64, QImage::Format_ARGB32);
    image.fill(QColor(200, 30, 30));
    const QString source = dir.filePath(QLatin1String("source.png"));
    QVERIFY(image.save(source));

    QVERIFY(store.setAvatarFile(QLatin1String("alpha"), source));
    QVERIFY(QFile::exists(store.profileFilePath(
        QLatin1String("alpha"), QLatin1String("profile-avatar.png"))));
    QCOMPARE(store.profile(QLatin1String("alpha")).avatar,
             QLatin1String("profile-avatar.png"));

    // Beta untouched — per-profile avatar storage is independent.
    QVERIFY(store.profile(QLatin1String("beta")).avatar.isEmpty());
    QVERIFY(!QFile::exists(store.profileFilePath(
        QLatin1String("beta"), QLatin1String("profile-avatar.png"))));

    // Painted faces differ: alpha's custom disc vs beta's silhouette.
    const QPixmap alphaFace =
        store.avatarPixmap(store.profile(QLatin1String("alpha")), 32);
    const QPixmap betaFace =
        store.avatarPixmap(store.profile(QLatin1String("beta")), 32);
    QVERIFY(!alphaFace.isNull());
    QVERIFY(!betaFace.isNull());
    QVERIFY(alphaFace.toImage() != betaFace.toImage());

    // An unreadable source and an unknown id both refuse without a
    // write.
    QVERIFY(!store.setAvatarFile(
        QLatin1String("alpha"), dir.filePath(QLatin1String("missing.png"))));
    QVERIFY(!store.setAvatarFile(QLatin1String("ghost"), source));

    QVERIFY(store.removeAvatar(QLatin1String("alpha")));
    QVERIFY(store.profile(QLatin1String("alpha")).avatar.isEmpty());
    QVERIFY(!QFile::exists(store.profileFilePath(
        QLatin1String("alpha"), QLatin1String("profile-avatar.png"))));
}

void tst_ProfileManager::identitySync()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));

    // The PROF01 rename path (QSettings through ProfileIdentity)
    // mirrors into the default row — the manager shows one truth.
    ProfileIdentity::instance()->setName(QLatin1String("Synced"));
    QTRY_COMPARE(store.profile(QLatin1String("default")).name,
                 QLatin1String("Synced"));

    // Renaming the default profile through the store routes back
    // through the identity so the nav-button header repaints.
    QVERIFY(store.renameProfile(QLatin1String("default"),
                                QLatin1String("Managed")));
    QCOMPARE(ProfileIdentity::instance()->name(),
             QLatin1String("Managed"));
    {
        QSettings settings;
        QCOMPARE(settings.value(QLatin1String("profile/name")).toString(),
                 QLatin1String("Managed"));
    }
    store.reload();
    QCOMPARE(store.profile(QLatin1String("default")).name,
             QLatin1String("Managed"));

    // Renaming a non-default profile never touches the live identity.
    QVERIFY(store.createProfile(QLatin1String("Side")).isValid());
    QVERIFY(store.renameProfile(QLatin1String("side"),
                                QLatin1String("Side Project")));
    QCOMPARE(ProfileIdentity::instance()->name(),
             QLatin1String("Managed"));

    ProfileIdentity::instance()->setName(QLatin1String("Profile"));
}

void tst_ProfileManager::managerDialogLegs()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileManagerDialog dialog(&store, QLatin1String("default"));
    dialog.show();
    QApplication::processEvents();

    // The list shows the default profile marked as this window's.
    QCOMPARE(dialog.profileTree->topLevelItemCount(), 1);
    QTreeWidgetItem *row = dialog.profileTree->topLevelItem(0);
    QCOMPARE(row->data(0, Qt::UserRole).toString(),
             QLatin1String("default"));
    QCOMPARE(row->text(1), QLatin1String("This window"));
    QVERIFY(dialog.newButton->isEnabled());
    QVERIFY(dialog.renameButton->isEnabled());
    // The default profile is undeletable — button off, reason in the
    // tooltip.
    QVERIFY(!dialog.deleteButton->isEnabled());
    QVERIFY(!dialog.deleteButton->toolTip().isEmpty());
    // Its own profile can open a window (it is what new windows do).
    QVERIFY(dialog.openWindowButton->isEnabled());

    // Create through the dialog: name in the modal, accept.
    QTimer::singleShot(150, qApp, []() {
        QDialog *sub = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (!sub)
            return;
        QLineEdit *edit = sub->findChild<QLineEdit *>(
            QLatin1String("newProfileName"));
        if (edit)
            edit->setText(QLatin1String("Evening"));
        sub->accept();
    });
    dialog.newButton->click();
    QApplication::processEvents();
    QVERIFY(store.profile(QLatin1String("evening")).isValid());
    QVERIFY(QFile::exists(store.profileDir(QLatin1String("evening"))));
    QCOMPARE(dialog.profileTree->topLevelItemCount(), 2);

    // The new row selects itself — its buttons tell the honest state:
    // deletable, not window-openable (binding hasn't merged).
    QCOMPARE(dialog.selectedProfileId(), QLatin1String("evening"));
    QVERIFY(dialog.deleteButton->isEnabled());
    QVERIFY(!dialog.openWindowButton->isEnabled());
    QVERIFY(!dialog.openWindowButton->toolTip().isEmpty());

    // Rename through the dialog (QInputDialog modal).
    QTimer::singleShot(150, qApp, []() {
        if (QInputDialog *sub = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
            sub->setTextValue(QLatin1String("Night"));
            sub->accept();
        }
    });
    dialog.renameButton->click();
    QCOMPARE(store.profile(QLatin1String("evening")).name,
             QLatin1String("Night"));

    // Delete refuses until the exact name is typed, then removes.
    QTimer::singleShot(150, qApp, []() {
        QDialog *sub = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (!sub)
            return;
        QLineEdit *phrase = sub->findChild<QLineEdit *>(
            QLatin1String("deleteProfilePhraseEdit"));
        QPushButton *confirm = sub->findChild<QPushButton *>(
            QLatin1String("deleteProfileConfirmButton"));
        QVERIFY(phrase);
        QVERIFY(confirm);
        phrase->setText(QLatin1String("wrong"));
        QVERIFY(!confirm->isEnabled());
        phrase->setText(QLatin1String("Night"));
        QVERIFY(confirm->isEnabled());
        confirm->click();
    });
    dialog.deleteButton->click();
    QApplication::processEvents();
    QVERIFY(!store.profile(QLatin1String("evening")).isValid());
    QVERIFY(!QFile::exists(store.profileDir(QLatin1String("evening"))));
    QCOMPARE(dialog.profileTree->topLevelItemCount(), 1);

    // Open Window on the bound (default) profile spawns a real window.
    dialog.profileTree->topLevelItem(0)->setSelected(true);
    QApplication::processEvents();
    const int before =
        BrowserApplication::instance()->mainWindows().count();
    dialog.openWindowButton->click();
    QApplication::processEvents();
    QCOMPARE(BrowserApplication::instance()->mainWindows().count(),
             before + 1);
    // The spawned window is the app's to dispose — closing keeps the
    // test tree tidy.
    BrowserMainWindow *opened =
        BrowserApplication::instance()->mainWindows().last();
    opened->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QTEST_MAIN(tst_ProfileManager)
#include "tst_profilemanager.moc"
