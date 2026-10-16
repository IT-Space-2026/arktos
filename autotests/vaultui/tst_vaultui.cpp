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

// PVAULT03: dedicated coverage for the vault UX — the facade's state
// transitions (seal/unseal/rewrap against the reversible stub
// engine), the honest error splits (wrong password vs tamper vs
// corrupt), cancel rollback, the dirty-boot offer, auto-lock on
// last-window-close, and the dialog/menu/manager legs.  The engine
// seam itself is the deliverable being tested: everything here runs
// unchanged once the real rv_* FFI lands.

#include "qtest_arora.h"

#include "stubvaultengine.h"

#include "browserapplication.h"
#include "profilebutton.h"
#include "profilemanagerdialog.h"
#include "profilestore.h"
#include "profilevault.h"
#include "vaultdialogs.h"

#include <qcombobox.h>
#include <qdir.h>
#include <qeventloop.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>
#include <qtimer.h>
#include <qtreewidget.h>

class tst_VaultUi : public QObject
{
    Q_OBJECT

private slots:
    void stateTransitions();
    void errorSplit();
    void cancelSealRollsBack();
    void cancelUnsealStaysLocked();
    void passwordChange();
    void passwordDialogLegs();
    void unlockDialogLegs();
    void ensureUnlockedFlow();
    void autoLockOnLastWindow();
    void sessionEnding();
    void dirtyBootOffer();
    void managerDialogLegs();
    void profileMenuLock();
    void deleteWipesVault();
    void engineUnavailable();
};

// Waits for the profile's in-flight op to finish; returns the engine
// status, -1 on timeout or when no op ever started.
static int waitOp(ProfileVault *vault, const QString &id,
                  int timeoutMs = 15000)
{
    if (!vault->isBusy(id))
        return -1;
    int status = -1;
    QEventLoop loop;
    QObject::connect(vault, &ProfileVault::operationFinished, &loop,
        [&](const QString &pid, int, int st, const QString &) {
            if (pid == id) {
                status = st;
                loop.quit();
            }
        });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return status;
}

// Writes a small file tree under the profile dir so a seal has real
// payload (multiple entries so the cancel probe lands mid-op).
static void plantFiles(ProfileStore *store, const QString &id,
                       int count = 4)
{
    QDir().mkpath(store->profileDir(id) + QLatin1String("/sub"));
    for (int i = 0; i < count; ++i) {
        QFile file(store->profileDir(id)
            + QLatin1String("/sub/file") + QString::number(i)
            + QLatin1String(".dat"));
        if (file.open(QIODevice::WriteOnly))
            file.write(QByteArray(256, char('a' + i)));
    }
}

// Repeating modal driver: every tick applies `step` to whichever
// modal widget is up.  QDialogs guard with result()==0 so a
// lingering tick can't re-fire between accept and close, and a
// re-shown prompt (unlock retries reuse the widget) still answers.
// QMessageBox fallback clicks the default button.  Lives for the
// scope of the guarded block.
class ModalDriver
{
public:
    explicit ModalDriver(std::function<void(QWidget *)> step)
        : m_step(step)
    {
        m_timer.setInterval(15);
        QObject::connect(&m_timer, &QTimer::timeout, qApp, [this]() {
            QWidget *widget = QApplication::activeModalWidget();
            if (!widget)
                return;
            if (QMessageBox *box =
                    qobject_cast<QMessageBox *>(widget)) {
                if (box->result() == 0)
                    box->accept();
                return;
            }
            if (QDialog *dialog = qobject_cast<QDialog *>(widget)) {
                if (dialog->result() != 0)
                    return;
                m_step(dialog);
                return;
            }
            m_step(widget);
        });
        m_timer.start();
    }
    ~ModalDriver() { m_timer.stop(); }

private:
    std::function<void(QWidget *)> m_step;
    QTimer m_timer;
};

void tst_VaultUi::stateTransitions()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(vault.engineAvailable());

    QVERIFY(store.createProfile(QLatin1String("Work")).isValid());
    const QString id = QLatin1String("work");
    const QString profileDir = store.profileDir(id);
    plantFiles(&store, id);

    QVERIFY(!vault.isVaulted(id));
    QVERIFY(!vault.isLocked(id));

    // Set-password vaults without sealing — dir still live.
    vault.setPassword(id, "hunter2-hunter2", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.isVaulted(id));
    QVERIFY(vault.isUnlocked(id));
    QVERIFY(vault.hasSessionPassword(id));
    QVERIFY(vault.autoLock(id));            // default ON for vaulted
    QCOMPARE(vault.idleLockMinutes(id), 0);
    QVERIFY(QFile::exists(
        ProfileVault::confPath(profileDir)));

    // Seal -> blob + lock marker + dir gone.
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    QVERIFY(vault.isLocked(id));
    QVERIFY(!vault.isUnlocked(id));
    QVERIFY(!vault.hasSessionPassword(id));
    QVERIFY(QFile::exists(ProfileVault::blobPath(profileDir)));
    QVERIFY(QFile::exists(ProfileVault::lockMarkerPath(profileDir)));
    QVERIFY(!QFile::exists(profileDir));

    // The blob header is inspectable without the password.
    const QJsonObject header = QJsonDocument::fromJson(
        vault.inspect(id)).object();
    QCOMPARE(header.value(QLatin1String("algo")).toInt(),
             int(ProfileVault::AlgoAes256Gcm));

    // Unseal wrong -> still locked, honest WrongPassword.
    QVERIFY(vault.startUnseal(id, "wrong-pass"));
    QCOMPARE(waitOp(&vault, id),
             int(ProfileVaultEngine::WrongPassword));
    QVERIFY(vault.isLocked(id));
    QVERIFY(!QFile::exists(profileDir));

    // Unseal right -> plaintext back, dirty marker armed, session
    // password cached for auto-lock.
    QVERIFY(vault.startUnseal(id, "hunter2-hunter2"));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    QVERIFY(vault.isUnlocked(id));
    QVERIFY(vault.hasSessionPassword(id));
    QVERIFY(QFile::exists(
        ProfileVault::dirtyMarkerPath(profileDir)));
    QVERIFY(QFile::exists(profileDir + QLatin1String("/sub/file0.dat")));
}

void tst_VaultUi::errorSplit()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id);
    vault.setPassword(id, "correct horse", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));

    const QString blob = ProfileVault::blobPath(store.profileDir(id));

    // Wrong password — distinct from integrity failures.
    StubVaultEngine probe;
    QCOMPARE(probe.verify(blob, "nope"),
             ProfileVaultEngine::WrongPassword);

    // Tamper inside the JSON payload — structure still parses, MAC
    // fails.  Find a "data" value and flip a base64 char.
    QFile file(blob);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray body = file.readAll();
    file.close();
    const int at = body.indexOf("\"data\":\"");
    QVERIFY(at > 0);
    const int pos = at + 10;
    body[pos] = body[pos] == 'A' ? 'B' : 'A';
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(body);
    file.close();

    QCOMPARE(probe.verify(blob, "correct horse"),
             ProfileVaultEngine::Tampered);
    QVERIFY(vault.startUnseal(id, "correct horse"));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::Tampered));
    QVERIFY(vault.isLocked(id));

    // Structural garbage -> Corrupt.
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not json at all");
    file.close();
    QCOMPARE(probe.verify(blob, "correct horse"),
             ProfileVaultEngine::Corrupt);

    // The honest strings differ per failure — the unlock prompt can
    // never blur them.
    QVERIFY(ProfileVault::statusText(ProfileVaultEngine::WrongPassword)
            != ProfileVault::statusText(ProfileVaultEngine::Tampered));
    QVERIFY(ProfileVault::statusText(ProfileVaultEngine::WrongPassword)
            .contains(QLatin1String("incorrect")));
    QVERIFY(ProfileVault::statusText(ProfileVaultEngine::Tampered)
            .contains(QLatin1String("integrity")));
}

void tst_VaultUi::cancelSealRollsBack()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id, 12);
    vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);

    // Cancel on the first progress tick — the probe lands mid-seal.
    connect(&vault, &ProfileVault::progress, &vault,
            [&vault, id](const QString &pid, quint64, quint64) {
        if (pid == id)
            vault.requestCancel(id);
    });
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::Canceled));

    // Rollback is honest: plaintext dir intact, no blob, not locked.
    QVERIFY(QFile::exists(store.profileDir(id)));
    QVERIFY(!QFile::exists(
        ProfileVault::blobPath(store.profileDir(id))));
    QVERIFY(!vault.isLocked(id));
    QVERIFY(vault.isUnlocked(id));
}

void tst_VaultUi::cancelUnsealStaysLocked()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id, 12);
    vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    QVERIFY(vault.isLocked(id));

    connect(&vault, &ProfileVault::progress, &vault,
            [&vault, id](const QString &pid, quint64, quint64) {
        if (pid == id)
            vault.requestCancel(id);
    });
    QVERIFY(vault.startUnseal(id, "pw"));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::Canceled));

    // Discard path: no plaintext dir survives a canceled unseal, the
    // blob is still there, and the profile reads locked.
    QVERIFY(vault.isLocked(id));
    QVERIFY(QFile::exists(
        ProfileVault::blobPath(store.profileDir(id))));
}

void tst_VaultUi::passwordChange()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id);
    vault.setPassword(id, "old-pass", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));

    // Rewrap with the wrong old password refuses honestly.
    QVERIFY(vault.startRewrap(id, "wrong", "new-pass"));
    QCOMPARE(waitOp(&vault, id),
             int(ProfileVaultEngine::WrongPassword));

    // Correct old -> new opens the vault afterwards.
    QVERIFY(vault.startRewrap(id, "old-pass", "new-pass"));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    QVERIFY(vault.isLocked(id));
    QVERIFY(vault.hasSessionPassword(id));   // new password cached
    StubVaultEngine probe;
    QCOMPARE(probe.verify(
                 ProfileVault::blobPath(store.profileDir(id)),
                 "new-pass"),
             ProfileVaultEngine::EngineOk);
}

void tst_VaultUi::passwordDialogLegs()
{
    VaultPasswordDialog dialog(QLatin1String("Work"),
                               VaultPasswordDialog::SetMode);
    dialog.show();

    // Empty -> no OK; mismatch -> no OK + honest note.
    QVERIFY(!dialog.okButton->isEnabled());
    dialog.passwordEdit->setText(QLatin1String("abc"));
    dialog.confirmEdit->setText(QLatin1String("abd"));
    QVERIFY(!dialog.okButton->isEnabled());
    QVERIFY(!dialog.matchLabel->text().isEmpty());
    QVERIFY(dialog.strengthLabel->text().contains(
        QLatin1String("Short")));

    dialog.confirmEdit->setText(QLatin1String("abc"));
    QVERIFY(dialog.okButton->isEnabled());
    QCOMPARE(dialog.password(), QByteArray("abc"));

    // Algo picker: recommended default + honest unaudited opt-in.
    QCOMPARE(dialog.algoCombo->count(), 2);
    QCOMPARE(dialog.algo(), int(ProfileVault::AlgoAes256Gcm));
    QVERIFY(dialog.algoCombo->itemText(0).contains(
        QLatin1String("recommended")));
    QVERIFY(dialog.algoCombo->itemText(1).contains(
        QLatin1String("not externally audited")));
    QVERIFY(dialog.strengthText(QLatin1String("a-very-long-passphrase"))
            .contains(QLatin1String("Strong")));

    // Change mode adds the current-password field.
    VaultPasswordDialog change(QLatin1String("Work"),
                               VaultPasswordDialog::ChangeMode);
    QVERIFY(change.currentEdit != nullptr);
}

void tst_VaultUi::unlockDialogLegs()
{
    VaultUnlockDialog dialog(QLatin1String("Work"));
    dialog.show();
    QVERIFY(!dialog.errorLabel->isVisible());
    dialog.setError(ProfileVaultEngine::WrongPassword);
    QVERIFY(dialog.errorLabel->isVisible());
    QVERIFY(dialog.errorLabel->text().contains(
        QLatin1String("incorrect")));
    dialog.setError(ProfileVaultEngine::Tampered);
    QVERIFY(dialog.errorLabel->text().contains(
        QLatin1String("integrity")));
    dialog.passwordEdit->setText(QLatin1String("pw"));
    QCOMPARE(dialog.password(), QByteArray("pw"));
}

void tst_VaultUi::ensureUnlockedFlow()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id);
    vault.setPassword(id, "sekrit-pw", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));

    // Scripted modal drive: wrong password (error label reappears on
    // the re-prompt), then the right one.
    QStringList attempts;
    attempts << QLatin1String("bad") << QLatin1String("sekrit-pw");
    QStringList seenErrors;
    {
        ModalDriver driver([&](QWidget *widget) {
            VaultUnlockDialog *dialog =
                qobject_cast<VaultUnlockDialog *>(widget);
            if (!dialog)
                return;
            seenErrors << dialog->errorLabel->text();
            if (attempts.isEmpty()) {
                dialog->reject();
                return;
            }
            dialog->passwordEdit->setText(attempts.takeFirst());
            dialog->accept();
        });
        QVERIFY(VaultUi::ensureUnlocked(&vault, id, nullptr));
    }
    QVERIFY(vault.isUnlocked(id));
    // Second prompt carried the honest failure inline.
    QCOMPARE(seenErrors.size(), 2);
    QVERIFY(seenErrors.at(0).isEmpty());
    QVERIFY(seenErrors.at(1).contains(QLatin1String("incorrect")));

    // Cancel leaves the profile sealed.
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    {
        ModalDriver driver([](QWidget *widget) {
            VaultUnlockDialog *dialog =
                qobject_cast<VaultUnlockDialog *>(widget);
            if (dialog)
                dialog->reject();
        });
        QVERIFY(!VaultUi::ensureUnlocked(&vault, id, nullptr));
    }
    QVERIFY(vault.isLocked(id));
}

void tst_VaultUi::autoLockOnLastWindow()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id);
    vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);

    // autoLock on + session password + zero open windows -> seal.
    vault.noteWindowClosed(id, nullptr);
    QVERIFY(vault.isLocked(id));
    QVERIFY(QFile::exists(
        ProfileVault::blobPath(store.profileDir(id))));

    // autoLock off -> the profile stays unlocked and keeps its dirty
    // marker until a clean session end clears it.
    QVERIFY(vault.startUnseal(id, "pw"));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    vault.setAutoLock(id, false);
    vault.noteWindowClosed(id, nullptr);
    QVERIFY(vault.isUnlocked(id));
    QVERIFY(QFile::exists(
        ProfileVault::dirtyMarkerPath(store.profileDir(id))));

    // Idle-deadline pref round-trips through vault.conf.
    vault.setIdleLockMinutes(id, 15);
    QCOMPARE(vault.idleLockMinutes(id), 15);
    vault.setAutoLock(id, true);
    QVERIFY(vault.autoLock(id));
}

void tst_VaultUi::sessionEnding()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("Keep")).isValid());
    QVERIFY(store.createProfile(QLatin1String("Seal")).isValid());
    plantFiles(&store, QLatin1String("keep"));
    plantFiles(&store, QLatin1String("seal"));

    // "keep" was sealed once then reopened — its dirty marker is
    // armed — but autoLock off means the user leaves it open by
    // choice.  "seal" is vaulted+unlocked with autoLock on.
    vault.setPassword(QLatin1String("keep"), "pw1",
                      ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(QLatin1String("keep"),
                            vault.sessionPassword(QLatin1String("keep"))));
    QCOMPARE(waitOp(&vault, QLatin1String("keep")),
             int(ProfileVaultEngine::EngineOk));
    QVERIFY(vault.startUnseal(QLatin1String("keep"), "pw1"));
    QCOMPARE(waitOp(&vault, QLatin1String("keep")),
             int(ProfileVaultEngine::EngineOk));
    QVERIFY(QFile::exists(ProfileVault::dirtyMarkerPath(
        store.profileDir(QLatin1String("keep")))));
    vault.setAutoLock(QLatin1String("keep"), false);
    vault.setPassword(QLatin1String("seal"), "pw2",
                      ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.isUnlocked(QLatin1String("keep")));
    QVERIFY(vault.isUnlocked(QLatin1String("seal")));
    QVERIFY(vault.hasSessionPassword(QLatin1String("keep")));
    QVERIFY(vault.hasSessionPassword(QLatin1String("seal")));

    vault.noteSessionEnding();
    QVERIFY(vault.isLocked(QLatin1String("seal")));
    QVERIFY(vault.isUnlocked(QLatin1String("keep")));
    QVERIFY(!QFile::exists(ProfileVault::dirtyMarkerPath(
        store.profileDir(QLatin1String("keep")))));
    // Session passwords die with the session.
    QVERIFY(!vault.hasSessionPassword(QLatin1String("keep")));
}

void tst_VaultUi::dirtyBootOffer()
{
    QTemporaryDir dir;
    const QString registryPath =
        dir.filePath(QLatin1String("profiles.json"));
    {
        ProfileStore store(registryPath);
        ProfileVault vault(&store, new StubVaultEngine);
        QVERIFY(store.createProfile(QLatin1String("P")).isValid());
        const QString id = QLatin1String("p");
        plantFiles(&store, id);
        vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);
        QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
        QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
        QVERIFY(vault.startUnseal(id, "pw"));
        QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
        // Simulated crash: destructing without noteSessionEnding
        // leaves unlocked.dirty behind.
    }

    // Next boot: a fresh vault over the same tree sees the dirty
    // profile.
    ProfileStore store(registryPath);
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(vault.dirtyProfiles().contains(QLatin1String("p")));
    QVERIFY(vault.isUnlocked(QLatin1String("p")));

    // "Stay Unlocked" -> nothing sealed, marker kept is the honest
    // choice the user made (it clears at THIS clean exit).
    {
        ModalDriver driver([](QWidget *widget) {
            VaultDirtyOfferDialog *dialog =
                qobject_cast<VaultDirtyOfferDialog *>(widget);
            if (dialog)
                dialog->keepButton->click();
        });
        QVERIFY(!VaultUi::offerReseal(&vault, QLatin1String("p"),
                                      nullptr));
    }
    QVERIFY(vault.isUnlocked(QLatin1String("p")));

    // "Re-seal Now" -> password prompt -> seal with progress; the
    // profile has no open windows so it seals immediately.
    {
        ModalDriver driver([](QWidget *widget) {
            if (VaultDirtyOfferDialog *dialog =
                    qobject_cast<VaultDirtyOfferDialog *>(widget)) {
                dialog->resealButton->click();
                return;
            }
            if (VaultUnlockDialog *prompt =
                    qobject_cast<VaultUnlockDialog *>(widget)) {
                prompt->passwordEdit->setText(QLatin1String("pw"));
                prompt->accept();
            }
        });
        QVERIFY(VaultUi::offerReseal(&vault, QLatin1String("p"),
                                     nullptr));
    }
    QVERIFY(vault.isLocked(QLatin1String("p")));
}

void tst_VaultUi::managerDialogLegs()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("Locked")).isValid());
    const QString id = QLatin1String("locked");
    plantFiles(&store, id);
    vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));

    ProfileManagerDialog dialog(&store, QLatin1String("default"),
                                nullptr, &vault);
    dialog.show();
    QApplication::processEvents();

    // Find the locked row: status column reads Locked.
    QTreeWidgetItem *row = nullptr;
    for (int i = 0; i < dialog.profileTree->topLevelItemCount(); ++i) {
        if (dialog.profileTree->topLevelItem(i)
                ->data(0, Qt::UserRole).toString() == id)
            row = dialog.profileTree->topLevelItem(i);
    }
    QVERIFY(row);
    QVERIFY(row->text(1).contains(QLatin1String("Locked")));
    // The Data column prices the blob, not a missing dir.
    QVERIFY(!row->text(3).isEmpty() && row->text(3) != QLatin1String("0 bytes"));

    dialog.profileTree->setCurrentItem(row);
    QCOMPARE(dialog.selectedProfileId(), id);
    QCOMPARE(dialog.passwordButton->text(),
             QString::fromLatin1("Change Pass&word..."));
    QCOMPARE(dialog.vaultButton->text(),
             QString::fromLatin1("&Unlock..."));
    QVERIFY(dialog.vaultButton->isEnabled());

    // Unlock from the manager: scripted password entry.  The click's
    // nested loops run the unseal to completion.
    {
        ModalDriver driver([](QWidget *widget) {
            if (VaultUnlockDialog *prompt =
                    qobject_cast<VaultUnlockDialog *>(widget)) {
                prompt->passwordEdit->setText(QLatin1String("pw"));
                prompt->accept();
            }
        });
        dialog.vaultButton->click();
        QTRY_VERIFY(vault.isUnlocked(id));
    }
    // reloadProfiles rebuilt the rows — re-find before asserting.
    row = nullptr;
    for (int i = 0; i < dialog.profileTree->topLevelItemCount(); ++i) {
        if (dialog.profileTree->topLevelItem(i)
                ->data(0, Qt::UserRole).toString() == id)
            row = dialog.profileTree->topLevelItem(i);
    }
    QVERIFY(row);
    QVERIFY(!row->text(1).contains(QLatin1String("Locked")));

    // Non-vaulted profile: Set Password..., Lock disabled with the
    // set-a-password reason on the tooltip.
    QVERIFY(store.createProfile(QLatin1String("Fresh")).isValid());
    dialog.reloadProfiles();
    for (int i = 0; i < dialog.profileTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *fresh =
            dialog.profileTree->topLevelItem(i);
        if (fresh->data(0, Qt::UserRole).toString()
            == QLatin1String("fresh"))
            dialog.profileTree->setCurrentItem(fresh);
    }
    QCOMPARE(dialog.selectedProfileId(), QLatin1String("fresh"));
    QCOMPARE(dialog.passwordButton->text(),
             QString::fromLatin1("Set Pass&word..."));
    QCOMPARE(dialog.vaultButton->text(), QString::fromLatin1("&Lock"));
    QVERIFY(!dialog.vaultButton->isEnabled());
    QVERIFY(dialog.vaultButton->toolTip().contains(
        QLatin1String("password")));
}

void tst_VaultUi::profileMenuLock()
{
    ProfileButton button;
    QAction *lock = button.profileMenu()->findChild<QAction *>(
        QLatin1String("profileLockAction"));
    QVERIFY(lock);
    QCOMPARE(lock->text(), QLatin1String("Lock Profile"));
    // The singleton vault on this lane carries NullVaultEngine —
    // disabled with the honest reason, never silently missing.
    QVERIFY(!lock->isEnabled());
    QVERIFY(lock->toolTip().contains(QLatin1String("engine")));
}

void tst_VaultUi::deleteWipesVault()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new StubVaultEngine);
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    const QString id = QLatin1String("p");
    plantFiles(&store, id);
    vault.setPassword(id, "pw", ProfileVault::AlgoAes256Gcm);
    QVERIFY(vault.startSeal(id, vault.sessionPassword(id)));
    QCOMPARE(waitOp(&vault, id), int(ProfileVaultEngine::EngineOk));
    const QString profileDir = store.profileDir(id);
    QVERIFY(QFile::exists(ProfileVault::blobPath(profileDir)));

    // Delete works without the password — owner-host honesty: the
    // sealed blob and its sidecars go with the registry row.
    QVERIFY(store.deleteProfile(id));
    QVERIFY(!QFile::exists(ProfileVault::blobPath(profileDir)));
    QVERIFY(!QFile::exists(ProfileVault::lockMarkerPath(profileDir)));
    QVERIFY(!QFile::exists(ProfileVault::confPath(profileDir)));
}

void tst_VaultUi::engineUnavailable()
{
    QTemporaryDir dir;
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    ProfileVault vault(&store, new NullVaultEngine);
    QVERIFY(!vault.engineAvailable());
    QVERIFY(store.createProfile(QLatin1String("P")).isValid());
    plantFiles(&store, QLatin1String("p"));
    QVERIFY(!vault.startSeal(QLatin1String("p"), "pw"));

    QString reason;
    QVERIFY(!vault.canLockNow(QLatin1String("p"), &reason));
    QVERIFY(reason.contains(QLatin1String("engine")));
    QVERIFY(ProfileVault::statusText(
        ProfileVaultEngine::EngineUnavailable)
            .contains(QLatin1String("not available")));
}

QTEST_MAIN(tst_VaultUi)
#include "tst_vaultui.moc"
