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

#ifndef VAULTDIALOGS_H
#define VAULTDIALOGS_H

#include <qdialog.h>
#include <qprogressdialog.h>
#include <qstring.h>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class ProfileVault;

// PVAULT03: the vault-facing dialogs.  Every one is honest about
// state — the algo picker names the unaudited option's caveat, the
// unlock prompt distinguishes a wrong password from a failed
// integrity check, and the dirty-boot offer never re-seals silently.
// Public widget members follow the codebase's autotest convention
// (SettingsDialog, ProfileManagerDialog) so dedicated legs can drive
// them.

// Set/change the vault password: new + confirm fields, a strength
// hint, the algo picker and a plain-language explanation of what
// locking does.  ChangeMode adds a current-password field.
class VaultPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    enum Mode { SetMode, ChangeMode };

    VaultPasswordDialog(const QString &profileName, Mode mode,
                        QWidget *parent = nullptr);

    QByteArray password() const;
    QByteArray currentPassword() const;
    int algo() const;

    // The advisory strength line — honest wording, no score theater.
    static QString strengthText(const QString &password);

    QLineEdit *currentEdit;      // ChangeMode only
    QLineEdit *passwordEdit;
    QLineEdit *confirmEdit;
    QComboBox *algoCombo;
    QLabel *strengthLabel;
    QLabel *matchLabel;
    QPushButton *okButton;
};

// Unlock prompt for a sealed profile — also reused as the "enter the
// password to lock/re-seal" prompt (sealing needs the password to
// wrap the vault key).  `purpose` overrides the intro line for those
// non-unlock uses.
class VaultUnlockDialog : public QDialog
{
    Q_OBJECT

public:
    VaultUnlockDialog(const QString &profileName,
                      const QString &purpose = QString(),
                      QWidget *parent = nullptr);

    QByteArray password() const;
    // Shows the honest failure — WrongPassword vs Tampered read
    // differently on purpose.
    void setError(int status);

    QLineEdit *passwordEdit;
    QLabel *errorLabel;
    QPushButton *okButton;
};

// Dirty-boot offer (spec wording): "Arora didn't shut down cleanly
// while this profile was unlocked.  Your data was left unencrypted —
// re-seal now?"
class VaultDirtyOfferDialog : public QDialog
{
    Q_OBJECT

public:
    VaultDirtyOfferDialog(const QString &profileName,
                          QWidget *parent = nullptr);

    QPushButton *resealButton;
    QPushButton *keepButton;
};

// Modal progress bar bound to a running vault operation — Cancel
// routes to ProfileVault::requestCancel and the dialog closes when
// the op's operationFinished lands.
class VaultProgressDialog : public QProgressDialog
{
    Q_OBJECT

public:
    VaultProgressDialog(ProfileVault *vault, const QString &profileId,
                        const QString &title, QWidget *parent = nullptr);
};

// Modal flow helpers — the UI entry points (profile menu, Profile
// Manager, startup) call these; each returns the honest end state.
namespace VaultUi {

// True when the profile ends up usable: already unlocked, or the
// user unsealed it.  A cancel leaves it sealed and returns false.
bool ensureUnlocked(ProfileVault *vault, const QString &profileId,
                    QWidget *parent);

// User-triggered Lock: prompts for the password when the session
// cache doesn't have one, then seals under a progress dialog.
// False when the lock can't run (the reason went to the user).
bool lockNow(ProfileVault *vault, const QString &profileId,
             QWidget *parent);

// Set or change the profile's vault password.  For a profile with
// no open windows the follow-up "lock now" offer runs the first
// seal; with open windows the password waits in the session for
// auto-lock — the dialog says which.
bool changePassword(ProfileVault *vault, const QString &profileId,
                    QWidget *parent);

// The dirty-boot offer for one profile; true when re-sealed.
bool offerReseal(ProfileVault *vault, const QString &profileId,
                 QWidget *parent);

// Waits for the in-flight op on `profileId` under a
// VaultProgressDialog; returns the engine Status.
int runWithProgress(ProfileVault *vault, const QString &profileId,
                    const QString &title, QWidget *parent);

} // namespace VaultUi

#endif // VAULTDIALOGS_H
