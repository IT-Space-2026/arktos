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

#include "profilemanagerdialog.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "profilestore.h"

#include <qdialogbuttonbox.h>
#include <qfile.h>
#include <qfiledialog.h>
#include <qfileinfo.h>
#include <qframe.h>
#include <qheaderview.h>
#include <qinputdialog.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qtreewidget.h>

QPointer<ProfileManagerDialog> ProfileManagerDialog::s_instance;

namespace {

// 'New Profile' — display name + an optional starting picture.  The
// picture picker is the plain-file seam: PROF02's crop editor hasn't
// merged into this lane, so this writes the scaled PNG directly and
// the integration pass swaps the picker for the real dialog.
class NewProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewProfileDialog(QWidget *parent = nullptr)
        : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
    {
        setWindowTitle(tr("New Profile"));

        QVBoxLayout *layout = new QVBoxLayout(this);
        QHBoxLayout *nameRow = new QHBoxLayout;
        nameRow->addWidget(new QLabel(tr("Name:")));
        nameEdit = new QLineEdit;
        nameEdit->setObjectName(QLatin1String("newProfileName"));
        nameEdit->setPlaceholderText(tr("Profile name"));
        nameRow->addWidget(nameEdit, 1);
        layout->addLayout(nameRow);

        QHBoxLayout *pictureRow = new QHBoxLayout;
        pictureButton = new QPushButton(tr("Choose &Picture..."));
        pictureButton->setObjectName(
            QLatin1String("newProfilePictureButton"));
        pictureButton->setSizePolicy(QSizePolicy::Maximum,
                                     QSizePolicy::Fixed);
        pictureLabel = new QLabel(tr("No picture — the default "
                                     "silhouette is used."));
        pictureLabel->setObjectName(
            QLatin1String("newProfilePictureLabel"));
        pictureRow->addWidget(pictureButton);
        pictureRow->addWidget(pictureLabel, 1);
        layout->addLayout(pictureRow);

        QDialogButtonBox *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        okButton = buttons->button(QDialogButtonBox::Ok);
        okButton->setEnabled(false);
        connect(nameEdit, &QLineEdit::textChanged, this, [this]() {
            okButton->setEnabled(!nameEdit->text().trimmed().isEmpty());
        });
        connect(pictureButton, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Choose Profile Picture"), QString(),
                tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.svg)"));
            if (!path.isEmpty())
                setPicturePath(path);
        });
        connect(buttons, &QDialogButtonBox::accepted,
                this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        layout->addStretch(1);
        layout->addWidget(buttons);
    }

    QString profileName() const { return nameEdit->text().trimmed(); }
    QString picturePath() const { return m_picturePath; }

    // Also the file-dialog target — tests set the path directly and
    // skip the modal picker.
    void setPicturePath(const QString &path)
    {
        m_picturePath = path;
        pictureLabel->setText(QFileInfo(path).fileName());
    }

    QLineEdit *nameEdit;
    QPushButton *pictureButton;
    QPushButton *okButton;
    QLabel *pictureLabel;
    QString m_picturePath;
};

// 'Delete Profile' — type-to-confirm gate: the button stays disabled
// until the edit holds the profile's exact display name, so the
// destructive step can never be a misclick or a wrong-profile slip.
class DeleteProfileDialog : public QDialog
{
    Q_OBJECT

public:
    DeleteProfileDialog(const QString &profileName,
                        const QString &dataDir, qint64 dataBytes,
                        QWidget *parent = nullptr)
        : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
    {
        setWindowTitle(tr("Delete Profile"));

        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *warning = new QLabel(
            tr("Deleting profile \"%1\" permanently removes its data "
               "folder and everything it holds — history, bookmarks, "
               "cookies and settings stored under:\n%2")
                .arg(profileName, dataDir));
        warning->setWordWrap(true);
        layout->addWidget(warning);
        if (dataBytes >= 0) {
            QLabel *sizeLabel = new QLabel(
                tr("Data on disk: %1")
                    .arg(formatDataSize(dataBytes)));
            layout->addWidget(sizeLabel);
        }
        QLabel *prompt = new QLabel(
            tr("Type the profile name \"%1\" to confirm:")
                .arg(profileName));
        prompt->setWordWrap(true);
        layout->addWidget(prompt);

        phraseEdit = new QLineEdit;
        phraseEdit->setObjectName(
            QLatin1String("deleteProfilePhraseEdit"));
        phraseEdit->setPlaceholderText(profileName);
        layout->addWidget(phraseEdit);

        QDialogButtonBox *buttons = new QDialogButtonBox(
            QDialogButtonBox::Cancel);
        confirmButton = buttons->addButton(
            tr("Delete Profile"), QDialogButtonBox::DestructiveRole);
        confirmButton->setObjectName(
            QLatin1String("deleteProfileConfirmButton"));
        confirmButton->setEnabled(false);
        connect(phraseEdit, &QLineEdit::textChanged, this,
                [this, profileName]() {
            confirmButton->setEnabled(
                phraseEdit->text().trimmed() == profileName);
        });
        // DestructiveRole buttons don't emit QDialogButtonBox::accepted
        // — wire the confirmation straight through to accept().
        connect(confirmButton, &QPushButton::clicked,
                this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::accepted,
                this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        layout->addStretch(1);
        layout->addWidget(buttons);
    }

    static QString formatDataSize(qint64 bytes)
    {
        if (bytes < 1024)
            return tr("%1 bytes").arg(bytes);
        const QStringList units = {
            QStringLiteral("KB"), QStringLiteral("MB"),
            QStringLiteral("GB"), QStringLiteral("TB") };
        double value = bytes;
        int unit = -1;
        do {
            value /= 1024.0;
            ++unit;
        } while (value >= 1024.0 && unit < units.size() - 1);
        return tr("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unit));
    }

    QLineEdit *phraseEdit;
    QPushButton *confirmButton;
};

} // namespace

ProfileManagerDialog::ProfileManagerDialog(ProfileStore *store,
                                           const QString &windowProfileId,
                                           QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_windowProfileId(windowProfileId.isEmpty()
                            ? ProfileStore::activeProfileId()
                            : windowProfileId)
{
    setObjectName(QLatin1String("profileManagerDialog"));
    setWindowTitle(tr("Profile Manager"));
    // The singleton is a QPointer — closing drops it so a reopen
    // starts from a fresh registry read.
    setAttribute(Qt::WA_DeleteOnClose);
    resize(640, 360);

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *intro = new QLabel(
        tr("Profiles keep separate browsing identities — each profile "
           "has its own history, bookmarks, cookies and settings.  "
           "A \"New Window\" stays on the profile that opened it; "
           "pick a profile below to open a window bound to it."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QHBoxLayout *body = new QHBoxLayout;
    profileTree = new QTreeWidget;
    profileTree->setObjectName(QLatin1String("profileManagerTree"));
    profileTree->setRootIsDecorated(false);
    profileTree->setUniformRowHeights(true);
    profileTree->setAllColumnsShowFocus(true);
    profileTree->setSelectionMode(QAbstractItemView::SingleSelection);
    profileTree->setHeaderLabels(
        { tr("Profile"), tr("Status"), tr("Windows"), tr("Data") });
    profileTree->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    profileTree->setIconSize(QSize(24, 24));
    body->addWidget(profileTree, 1);

    QVBoxLayout *buttons = new QVBoxLayout;
    newButton = new QPushButton(tr("&New Profile..."));
    newButton->setObjectName(QLatin1String("profileManagerNew"));
    renameButton = new QPushButton(tr("&Rename..."));
    renameButton->setObjectName(QLatin1String("profileManagerRename"));
    pictureButton = new QPushButton(tr("Set &Picture..."));
    pictureButton->setObjectName(QLatin1String("profileManagerPicture"));
    removePictureButton = new QPushButton(tr("R&emove Picture"));
    removePictureButton->setObjectName(
        QLatin1String("profileManagerRemovePicture"));
    deleteButton = new QPushButton(tr("&Delete..."));
    deleteButton->setObjectName(QLatin1String("profileManagerDelete"));
    QFrame *rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    openWindowButton = new QPushButton(tr("&Open Window"));
    openWindowButton->setObjectName(
        QLatin1String("profileManagerOpenWindow"));
    closeButton = new QPushButton(tr("&Close"));
    closeButton->setObjectName(QLatin1String("profileManagerClose"));
    const QSizePolicy fixedWidth(QSizePolicy::Maximum,
                                 QSizePolicy::Fixed);
    for (QPushButton *button :
         { newButton, renameButton, pictureButton, removePictureButton,
           deleteButton, openWindowButton, closeButton })
        button->setSizePolicy(fixedWidth);
    buttons->addWidget(newButton);
    buttons->addWidget(renameButton);
    buttons->addWidget(pictureButton);
    buttons->addWidget(removePictureButton);
    buttons->addWidget(deleteButton);
    buttons->addWidget(rule);
    buttons->addStretch(1);
    buttons->addWidget(openWindowButton);
    buttons->addWidget(closeButton);
    body->addLayout(buttons);
    layout->addLayout(body, 1);

    connect(newButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::newProfile);
    connect(renameButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::renameSelected);
    connect(pictureButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::choosePicture);
    connect(removePictureButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::removePicture);
    connect(deleteButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::deleteSelected);
    connect(openWindowButton, &QPushButton::clicked,
            this, &ProfileManagerDialog::openWindow);
    connect(closeButton, &QPushButton::clicked,
            this, &QDialog::accept);
    connect(profileTree, &QTreeWidget::itemSelectionChanged,
            this, &ProfileManagerDialog::updateButtons);
    connect(profileTree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *, int) { openWindow(); });
    connect(m_store, &ProfileStore::profilesChanged,
            this, &ProfileManagerDialog::reloadProfiles);
    connect(m_store, &ProfileStore::profileChanged,
            this, &ProfileManagerDialog::reloadProfiles);

    reloadProfiles();
}

void ProfileManagerDialog::showManager(QWidget *parent,
                                       const QString &profileId)
{
    if (!s_instance)
        s_instance = new ProfileManagerDialog(
            ProfileStore::instance(), profileId, parent);
    s_instance->show();
    s_instance->raise();
    s_instance->activateWindow();
}

ProfileManagerDialog *ProfileManagerDialog::instance()
{
    return s_instance;
}

QString ProfileManagerDialog::selectedProfileId() const
{
    const QList<QTreeWidgetItem *> selected =
        profileTree->selectedItems();
    if (selected.isEmpty())
        return QString();
    return selected.first()->data(0, Qt::UserRole).toString();
}

int ProfileManagerDialog::openWindowCount(const QString &id) const
{
    // MPROF03: real per-persona counts — each window reports its bound
    // profile id.
    if (!BrowserApplication::instance())
        return 0;
    int count = 0;
    for (BrowserMainWindow *window
         : BrowserApplication::instance()->mainWindows()) {
        if (window && window->profileId() == id)
            ++count;
    }
    return count;
}

void ProfileManagerDialog::reloadProfiles()
{
    const QString keep = selectedProfileId();
    profileTree->clear();
    for (const ProfileContext &context : m_store->profiles()) {
        QTreeWidgetItem *row = new QTreeWidgetItem(profileTree);
        row->setData(0, Qt::UserRole, context.id);
        row->setIcon(0, QIcon(m_store->avatarPixmap(context, 24)));
        QString name = context.name.isEmpty() ? context.id : context.name;
        row->setText(0, name);
        QString status;
        if (context.id == m_windowProfileId)
            status = tr("This window");
        else if (openWindowCount(context.id) > 0)
            status = tr("In use");
        row->setText(1, status);
        row->setText(2, QString::number(openWindowCount(context.id)));
        const qint64 bytes = m_store->profileDataSize(context.id);
        row->setText(3, bytes < 0 ? tr("—")
                                : DeleteProfileDialog::formatDataSize(
                                      bytes));
        row->setToolTip(0, tr("Profile id: %1\nData folder: %2")
            .arg(context.id, m_store->profileDir(context.id)));
        if (context.id == keep)
            row->setSelected(true);
    }
    if (profileTree->selectedItems().isEmpty()
        && profileTree->topLevelItemCount() > 0)
        profileTree->topLevelItem(0)->setSelected(true);
    updateButtons();
}

void ProfileManagerDialog::updateButtons()
{
    const QString id = selectedProfileId();
    const bool hasSelection = !id.isEmpty();
    renameButton->setEnabled(hasSelection);
    pictureButton->setEnabled(hasSelection);
    deleteButton->setEnabled(
        hasSelection && id != ProfileStore::defaultProfileId());
    deleteButton->setToolTip(
        id == ProfileStore::defaultProfileId()
            ? tr("The default profile cannot be removed here — "
                 "deleting it would erase all browser data.")
            : QString());
    const ProfileContext context = m_store->profile(id);
    const QString avatarName = context.avatar.isEmpty()
        ? QLatin1String("profile-avatar.png") : context.avatar;
    removePictureButton->setEnabled(
        hasSelection
        && QFile::exists(m_store->profileFilePath(id, avatarName)));
    // MPROF03: per-persona window binding is live — any registered
    // profile can host a window.
    const bool canOpen = hasSelection
        && (id == m_windowProfileId
            || ProfileStore::windowBindingAvailable());
    openWindowButton->setEnabled(canOpen);
    openWindowButton->setToolTip(
        canOpen ? QString()
                : tr("Opening a window on another profile needs "
                     "per-profile window support — not available yet."));
}

void ProfileManagerDialog::newProfile()
{
    NewProfileDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const ProfileContext context =
        m_store->createProfile(dialog.profileName());
    if (!context.isValid()) {
        QMessageBox::warning(this, tr("New Profile"),
                             tr("The profile could not be created."));
        return;
    }
    if (!dialog.picturePath().isEmpty()
        && !m_store->setAvatarFile(context.id, dialog.picturePath()))
        QMessageBox::warning(
            this, tr("New Profile"),
            tr("The profile was created, but the picture file could "
               "not be read."));
    for (int i = 0; i < profileTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = profileTree->topLevelItem(i);
        if (row->data(0, Qt::UserRole).toString() == context.id) {
            profileTree->setCurrentItem(row);
            break;
        }
    }
}

void ProfileManagerDialog::renameSelected()
{
    const QString id = selectedProfileId();
    const ProfileContext context = m_store->profile(id);
    if (!context.isValid())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Profile"), tr("Profile name:"),
        QLineEdit::Normal, context.name, &ok);
    if (ok)
        m_store->renameProfile(id, name);
}

void ProfileManagerDialog::choosePicture()
{
    const QString id = selectedProfileId();
    if (id.isEmpty())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose Profile Picture"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.svg)"));
    if (!path.isEmpty()
        && !m_store->setAvatarFile(id, path))
        QMessageBox::warning(this, tr("Profile Picture"),
                             tr("The picture file could not be read."));
}

void ProfileManagerDialog::removePicture()
{
    const QString id = selectedProfileId();
    if (!id.isEmpty())
        m_store->removeAvatar(id);
}

void ProfileManagerDialog::deleteSelected()
{
    const QString id = selectedProfileId();
    const ProfileContext context = m_store->profile(id);
    if (!context.isValid() || id == ProfileStore::defaultProfileId())
        return;
    const int windows = openWindowCount(id);
    DeleteProfileDialog dialog(context.name, m_store->profileDir(id),
                               m_store->profileDataSize(id), this);
    if (windows > 0)
        dialog.setWindowTitle(
            tr("Delete Profile — %n window(s) will close",
               nullptr, windows));
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!m_store->deleteProfile(id))
        QMessageBox::warning(this, tr("Delete Profile"),
                             tr("The profile could not be deleted — "
                                "its data folder may still be in use."));
}

void ProfileManagerDialog::openWindow()
{
    const QString id = selectedProfileId();
    if (id.isEmpty())
        return;
    // MPROF03: the window binds to the SELECTED persona — its tabs
    // browse on that profile's named engine profile and its stores
    // follow.  An id the registry no longer carries binds "default"
    // honestly inside newMainWindow().
    if (BrowserApplication *application =
            BrowserApplication::instance()) {
        BrowserMainWindow *window = application->newMainWindow(id);
        window->goHome();
    }
}

#include "profilemanagerdialog.moc"
