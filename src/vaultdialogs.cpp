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

#include "vaultdialogs.h"

#include "profilestore.h"
#include "profilevault.h"

#include <qcombobox.h>
#include <qdialogbuttonbox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qmessagebox.h>
#include <qpushbutton.h>

VaultPasswordDialog::VaultPasswordDialog(const QString &profileName,
                                         Mode mode, QWidget *parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
    , currentEdit(nullptr)
{
    setWindowTitle(mode == ChangeMode ? tr("Change Profile Password")
                                      : tr("Set Profile Password"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *explanation = new QLabel(
        tr("A password locks profile \"%1\"'s data at rest — its "
           "history, bookmarks, cookies and settings are encrypted "
           "on disk and only open again with this password.  Losing "
           "the password means the data cannot be recovered.")
            .arg(profileName));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    QGridLayout *fields = new QGridLayout;
    int row = 0;
    if (mode == ChangeMode) {
        fields->addWidget(new QLabel(tr("Current password:")), row, 0);
        currentEdit = new QLineEdit;
        currentEdit->setObjectName(QLatin1String("vaultCurrentEdit"));
        currentEdit->setEchoMode(QLineEdit::Password);
        fields->addWidget(currentEdit, row, 1);
        ++row;
    }
    fields->addWidget(
        new QLabel(mode == ChangeMode ? tr("New password:")
                                      : tr("Password:")),
        row, 0);
    passwordEdit = new QLineEdit;
    passwordEdit->setObjectName(QLatin1String("vaultPasswordEdit"));
    passwordEdit->setEchoMode(QLineEdit::Password);
    fields->addWidget(passwordEdit, row, 1);
    ++row;
    fields->addWidget(new QLabel(tr("Confirm password:")), row, 0);
    confirmEdit = new QLineEdit;
    confirmEdit->setObjectName(QLatin1String("vaultConfirmEdit"));
    confirmEdit->setEchoMode(QLineEdit::Password);
    fields->addWidget(confirmEdit, row, 1);
    ++row;
    fields->addWidget(new QLabel(tr("Cipher:")), row, 0);
    algoCombo = new QComboBox;
    algoCombo->setObjectName(QLatin1String("vaultAlgoCombo"));
    // Honest labels: the default names what it is and that it's the
    // recommended one; the opt-in lane carries its caveat in the
    // label because the user asked for it by choosing it.
    algoCombo->addItem(
        tr("AES-256-GCM + HMAC-SHA-512 (recommended)"),
        ProfileVault::AlgoAes256Gcm);
    algoCombo->addItem(
        tr("Threefish-1024 XEX + HMAC-Skein-512 "
           "(implementation not externally audited)"),
        ProfileVault::AlgoThreefishXex);
    fields->addWidget(algoCombo, row, 1);
    layout->addLayout(fields);

    strengthLabel = new QLabel;
    strengthLabel->setObjectName(QLatin1String("vaultStrengthLabel"));
    layout->addWidget(strengthLabel);
    matchLabel = new QLabel;
    matchLabel->setObjectName(QLatin1String("vaultMatchLabel"));
    matchLabel->setWordWrap(true);
    layout->addWidget(matchLabel);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addStretch(1);
    layout->addWidget(buttons);

    const auto refresh = [this]() {
        strengthLabel->setText(strengthText(passwordEdit->text()));
        const bool match =
            !confirmEdit->text().isEmpty()
            && confirmEdit->text() == passwordEdit->text();
        matchLabel->setText(
            confirmEdit->text().isEmpty() || match
                ? QString()
                : tr("The passwords do not match."));
        okButton->setEnabled(!passwordEdit->text().isEmpty() && match);
    };
    connect(passwordEdit, &QLineEdit::textChanged,
            this, refresh);
    connect(confirmEdit, &QLineEdit::textChanged,
            this, refresh);
}

QByteArray VaultPasswordDialog::password() const
{
    return passwordEdit->text().toUtf8();
}

QByteArray VaultPasswordDialog::currentPassword() const
{
    return currentEdit ? currentEdit->text().toUtf8() : QByteArray();
}

int VaultPasswordDialog::algo() const
{
    return algoCombo->currentData().toInt();
}

QString VaultPasswordDialog::strengthText(const QString &password)
{
    // Advisory only — a length-based hint, never a fake entropy score.
    const int length = password.size();
    if (length == 0)
        return QString();
    if (length < 8)
        return tr("Short — a passphrase of several words is harder "
                  "to break.");
    if (length < 12)
        return tr("Okay — longer is stronger.");
    return tr("Strong password length.");
}

// -------------------------------------------------------------------

VaultUnlockDialog::VaultUnlockDialog(const QString &profileName,
                                     const QString &purpose,
                                     QWidget *parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
{
    setWindowTitle(tr("Profile Locked"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *intro = new QLabel(
        purpose.isEmpty()
            ? tr("Profile \"%1\" is locked.  Enter its password to "
                 "open it — canceling leaves the data sealed.")
                  .arg(profileName)
            : purpose.arg(profileName));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    passwordEdit = new QLineEdit;
    passwordEdit->setObjectName(QLatin1String("vaultUnlockEdit"));
    passwordEdit->setEchoMode(QLineEdit::Password);
    layout->addWidget(passwordEdit);

    errorLabel = new QLabel;
    errorLabel->setObjectName(QLatin1String("vaultUnlockError"));
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    layout->addWidget(errorLabel);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setText(tr("Unlock"));
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addStretch(1);
    layout->addWidget(buttons);
}

QByteArray VaultUnlockDialog::password() const
{
    return passwordEdit->text().toUtf8();
}

void VaultUnlockDialog::setError(int status)
{
    errorLabel->setText(ProfileVault::statusText(status));
    errorLabel->setVisible(status != ProfileVaultEngine::EngineOk);
}

// -------------------------------------------------------------------

VaultDirtyOfferDialog::VaultDirtyOfferDialog(const QString &profileName,
                                             QWidget *parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
{
    setWindowTitle(tr("Profile Left Unencrypted"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *text = new QLabel(
        tr("Arora didn't shut down cleanly while profile \"%1\" was "
           "unlocked.  Your data was left unencrypted — re-seal now?")
            .arg(profileName));
    text->setWordWrap(true);
    layout->addWidget(text);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel);
    keepButton = buttons->button(QDialogButtonBox::Cancel);
    keepButton->setText(tr("Stay Unlocked"));
    keepButton->setObjectName(QLatin1String("vaultKeepUnlocked"));
    resealButton = buttons->addButton(
        tr("Re-seal Now"), QDialogButtonBox::AcceptRole);
    resealButton->setObjectName(QLatin1String("vaultResealNow"));
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addStretch(1);
    layout->addWidget(buttons);
}

// -------------------------------------------------------------------

VaultProgressDialog::VaultProgressDialog(ProfileVault *vault,
                                         const QString &profileId,
                                         const QString &title,
                                         QWidget *parent)
    : QProgressDialog(title, tr("Cancel"), 0, 100, parent)
{
    setObjectName(QLatin1String("vaultProgressDialog"));
    setWindowTitle(title);
    setMinimumDuration(0);
    setAutoClose(false);
    setAutoReset(false);
    connect(vault, &ProfileVault::progress, this,
            [this, profileId](const QString &id, quint64 done,
                              quint64 total) {
        if (id != profileId)
            return;
        if (total > 0) {
            setMaximum(100);
            setValue(int(done * 100 / total));
        } else {
            // Unknown total — bounce rather than fake a fraction.
            setMaximum(0);
        }
    });
    connect(this, &QProgressDialog::canceled, this,
            [vault, profileId]() {
        vault->requestCancel(profileId);
    });
    connect(vault, &ProfileVault::operationFinished, this,
            [this, profileId](const QString &id, int, int,
                              const QString &) {
        if (id == profileId)
            done(QDialog::Accepted);
    });
}

// -------------------------------------------------------------------

int VaultUi::runWithProgress(ProfileVault *vault,
                             const QString &profileId,
                             const QString &title, QWidget *parent)
{
    int result = ProfileVaultEngine::EngineUnavailable;
    VaultProgressDialog dialog(vault, profileId, title, parent);
    QObject::connect(vault, &ProfileVault::operationFinished, &dialog,
        [&](const QString &id, int, int status, const QString &) {
            if (id == profileId)
                result = status;
        });
    if (!vault->isBusy(profileId))
        return ProfileVaultEngine::EngineUnavailable;
    dialog.exec();
    return result;
}

bool VaultUi::ensureUnlocked(ProfileVault *vault,
                             const QString &profileId,
                             QWidget *parent)
{
    if (!vault->isLocked(profileId))
        return true;
    const QString name = vault->store()->profile(profileId).name;
    int lastStatus = ProfileVaultEngine::EngineOk;
    while (vault->isLocked(profileId)) {
        VaultUnlockDialog dialog(name, QString(), parent);
        if (lastStatus != ProfileVaultEngine::EngineOk)
            // A wrong password comes back as an inline error on the
            // re-prompt, not a separate popup.
            dialog.setError(lastStatus);
        if (dialog.exec() != QDialog::Accepted)
            return false;
        if (!vault->startUnseal(profileId, dialog.password())) {
            QMessageBox::warning(
                parent, QObject::tr("Profile Locked"),
                ProfileVault::statusText(
                    ProfileVaultEngine::EngineUnavailable));
            return false;
        }
        lastStatus = runWithProgress(
            vault, profileId,
            QObject::tr("Unlocking profile \"%1\"...").arg(name), parent);
        if (lastStatus == ProfileVaultEngine::EngineOk)
            return true;
        if (lastStatus == ProfileVaultEngine::Canceled)
            return false;
        // Integrity/corruption failures deserve their own modal —
        // they're not a typo the user can fix by retrying.
        if (lastStatus != ProfileVaultEngine::WrongPassword)
            QMessageBox::warning(parent, QObject::tr("Profile Locked"),
                ProfileVault::statusText(lastStatus));
    }
    return true;
}

bool VaultUi::lockNow(ProfileVault *vault, const QString &profileId,
                      QWidget *parent)
{
    QString reason;
    if (!vault->canLockNow(profileId, &reason)) {
        QMessageBox::information(parent, QObject::tr("Lock Profile"), reason);
        return false;
    }
    const QString name = vault->store()->profile(profileId).name;
    QByteArray password = vault->sessionPassword(profileId);
    if (password.isEmpty()) {
        // Sealing needs the password to wrap the vault key — a
        // profile never unlocked this session has none cached.
        VaultUnlockDialog dialog(
            name, QObject::tr("Enter the password for profile \"%1\" to lock "
                     "it — canceling leaves it unlocked."), parent);
        if (dialog.exec() != QDialog::Accepted)
            return false;
        password = dialog.password();
        if (password.isEmpty())
            return false;
    }
    if (!vault->startSeal(profileId, password))
        return false;
    const int status = runWithProgress(
        vault, profileId,
        QObject::tr("Locking profile \"%1\"...").arg(name), parent);
    if (status == ProfileVaultEngine::EngineOk)
        return true;
    if (status != ProfileVaultEngine::Canceled)
        QMessageBox::warning(parent, QObject::tr("Lock Profile"),
                             ProfileVault::statusText(status));
    return false;
}

bool VaultUi::changePassword(ProfileVault *vault,
                             const QString &profileId,
                             QWidget *parent)
{
    if (!vault->engineAvailable()) {
        QMessageBox::information(
            parent, QObject::tr("Profile Password"),
            ProfileVault::statusText(
                ProfileVaultEngine::EngineUnavailable));
        return false;
    }
    const QString name = vault->store()->profile(profileId).name;
    const bool vaulted = vault->isVaulted(profileId);
    VaultPasswordDialog dialog(
        name, vaulted ? VaultPasswordDialog::ChangeMode
                      : VaultPasswordDialog::SetMode, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    if (vaulted && vault->isLocked(profileId)) {
        // Sealed vault: rewrap re-wraps the vault key — the payload
        // never moves.
        if (!vault->startRewrap(profileId, dialog.currentPassword(),
                               dialog.password())) {
            QMessageBox::warning(
                parent, QObject::tr("Profile Password"),
                ProfileVault::statusText(
                    ProfileVaultEngine::IoError));
            return false;
        }
        const int status = runWithProgress(
            vault, profileId,
            QObject::tr("Changing the password for \"%1\"...").arg(name),
            parent);
        if (status != ProfileVaultEngine::EngineOk) {
            QMessageBox::warning(parent, QObject::tr("Profile Password"),
                                 ProfileVault::statusText(status));
            return false;
        }
        return true;
    }

    // Unlocked (or never sealed): the password lives in the session
    // cache until the lock runs — nothing plaintext sits in the conf.
    vault->setPassword(profileId, dialog.password(), dialog.algo());
    if (vaulted)
        return true;

    QString reason;
    if (vault->canLockNow(profileId, &reason)) {
        const int choice = QMessageBox::question(
            parent, QObject::tr("Profile Password"),
            QObject::tr("Lock profile \"%1\" now?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes)
            return lockNow(vault, profileId, parent);
        return true;
    }
    QMessageBox::information(
        parent, QObject::tr("Profile Password"),
        QObject::tr("The password is set.  %1 — the profile locks when its "
           "last window closes.")
            .arg(reason));
    return true;
}

bool VaultUi::offerReseal(ProfileVault *vault,
                          const QString &profileId,
                          QWidget *parent)
{
    const QString name = vault->store()->profile(profileId).name;
    VaultDirtyOfferDialog dialog(name, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    // Re-sealing needs the password — the crashed session's cache is
    // gone, so prompt unless this session already holds it.
    QByteArray password = vault->sessionPassword(profileId);
    if (password.isEmpty()) {
        VaultUnlockDialog prompt(
            name,
            QObject::tr("Enter the password for profile \"%1\" to re-seal it "
               "— canceling leaves it unencrypted."), parent);
        if (prompt.exec() != QDialog::Accepted
            || prompt.password().isEmpty())
            return false;
        password = prompt.password();
    }
    // Keep the password for the session so a deferred re-seal (open
    // windows) can still run.
    vault->setPassword(profileId, password, vault->algo(profileId));

    QString reason;
    if (vault->canLockNow(profileId, &reason)) {
        if (!vault->startSeal(profileId, password))
            return false;
        const int status = runWithProgress(
            vault, profileId,
            QObject::tr("Re-sealing profile \"%1\"...").arg(name), parent);
        if (status == ProfileVaultEngine::EngineOk)
            return true;
        if (status != ProfileVaultEngine::Canceled)
            QMessageBox::warning(parent, QObject::tr("Re-seal Profile"),
                                 ProfileVault::statusText(status));
        return false;
    }
    // The profile has open windows — the seal can't run under them.
    // Arm auto-lock so it happens the moment the last window closes
    // and say so plainly rather than pretending it re-sealed.
    vault->setAutoLock(profileId, true);
    QMessageBox::information(
        parent, QObject::tr("Re-seal Profile"),
        QObject::tr("Profile \"%1\" has open windows, so it can't be sealed "
           "right now.  It will re-seal automatically when its last "
           "window closes.").arg(name));
    return true;
}
