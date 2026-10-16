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

#include "profilemenuactions.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "clearprivatedata.h"
#include "downloadmanager.h"
#include "tabwidget.h"

#include <qapplication.h>
#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qmainwindow.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qprocess.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qstatusbar.h>
#include <qthread.h>
#include <qwidgetaction.h>

#if defined(Q_OS_UNIX)
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

QString ProfileWipeConfirmDialog::confirmPhrase()
{
    return QLatin1String("DELETE");
}

ProfileWipeConfirmDialog::ProfileWipeConfirmDialog(QWidget *parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
{
    setWindowTitle(tr("Delete Profile"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("This is the last step.  To permanently erase the profile "
           "and restart Arora, type %1 below:")
            .arg(confirmPhrase())));

    phraseEdit = new QLineEdit;
    phraseEdit->setPlaceholderText(confirmPhrase());
    layout->addWidget(phraseEdit);

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch(1);
    QPushButton *cancelButton = new QPushButton(tr("&Cancel"));
    wipeButton = new QPushButton(tr("Delete and Restart Arora"));
    wipeButton->setEnabled(false);
    wipeButton->setStyleSheet(QLatin1String(
        "QPushButton { color: #ffffff; background: #c62828;"
        " border: none; padding: 6px 14px; border-radius: 3px;"
        " font-weight: 600; }"
        "QPushButton:disabled { color: rgba(255,255,255,0.55);"
        " background: rgba(198,40,40,0.35); }"
        "QPushButton:hover:!disabled { background: #b71c1c; }"));
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(wipeButton);
    layout->addStretch(1);
    layout->addLayout(buttonLayout);

    connect(phraseEdit, &QLineEdit::textChanged, this, [this]() {
        wipeButton->setEnabled(phraseEdit->text() == confirmPhrase());
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(wipeButton, &QPushButton::clicked, this, &QDialog::accept);
}

ProfileMenuActions::ProfileMenuActions(QWidget *parent)
    : QObject(parent)
    , m_parent(parent)
    , m_clearHistoryAction(nullptr)
    , m_clearCookiesAction(nullptr)
    , m_clearCacheAction(nullptr)
    , m_clearPrivateDataAction(nullptr)
    , m_deleteProfileAction(nullptr)
{
}

void ProfileMenuActions::runClear(quint32 categories,
                                  const QString &confirmText,
                                  const QString &doneMessage)
{
    QWidget *parent = m_parent ? m_parent->window() : nullptr;
    if (QMessageBox::warning(parent, QString(), confirmText,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return;

    // The CLR01 plan engine decides which stores die; a private
    // session is deliberately NOT scoped in — these are profile-menu
    // hygiene actions against the persistent profile, so the plan is
    // the full persistent set for the category.
    ClearPrivateData::executePlan(ClearPrivateData::plan(
        QLatin1String("everything"), categories, /*otr=*/false));

    if (categories & ClearPrivateData::ClearHistory) {
        // TABS05: the closed-tab undo stacks still hold just-cleared
        // urls — drop them in every window, the same removal the full
        // clear-private-data dialog performs.
        if (BrowserApplication *application =
                BrowserApplication::instance()) {
            const QList<BrowserMainWindow*> windows =
                application->mainWindows();
            for (BrowserMainWindow *window : windows) {
                if (window && window->tabWidget())
                    window->tabWidget()->clearRecentlyClosedTabs();
            }
        }
    }

    if (QMainWindow *window = qobject_cast<QMainWindow*>(parent))
        window->statusBar()->showMessage(doneMessage, 5000);
}

QAction *ProfileMenuActions::clearHistoryAction()
{
    if (!m_clearHistoryAction) {
        m_clearHistoryAction = new QAction(tr("Clear History…"), this);
        connect(m_clearHistoryAction, &QAction::triggered, this, [this]() {
            runClear(ClearPrivateData::ClearHistory,
                     tr("Clear browsing history for all time?\n"
                        "This cannot be undone."),
                     tr("Browsing history cleared."));
        });
    }
    return m_clearHistoryAction;
}

QAction *ProfileMenuActions::clearCookiesAction()
{
    if (!m_clearCookiesAction) {
        m_clearCookiesAction = new QAction(tr("Clear Cookies…"), this);
        connect(m_clearCookiesAction, &QAction::triggered, this, [this]() {
            runClear(ClearPrivateData::ClearCookies,
                     tr("Delete all cookies?\nThis cannot be undone."),
                     tr("Cookies cleared."));
        });
    }
    return m_clearCookiesAction;
}

QAction *ProfileMenuActions::clearCacheAction()
{
    if (!m_clearCacheAction) {
        m_clearCacheAction = new QAction(tr("Clear Cache…"), this);
        connect(m_clearCacheAction, &QAction::triggered, this, [this]() {
            runClear(ClearPrivateData::ClearCache,
                     tr("Clear cached web pages for all time?\n"
                        "This cannot be undone."),
                     tr("Cached web pages cleared."));
        });
    }
    return m_clearCacheAction;
}

QAction *ProfileMenuActions::clearPrivateDataAction()
{
    if (!m_clearPrivateDataAction) {
        m_clearPrivateDataAction =
            new QAction(tr("Clear Private Data…"), this);
        connect(m_clearPrivateDataAction, &QAction::triggered, this,
                [this]() {
            ClearPrivateData dialog(m_parent);
            dialog.exec();
        });
    }
    return m_clearPrivateDataAction;
}

QList<QAction *> ProfileMenuActions::hygieneActions()
{
    return {clearHistoryAction(), clearCookiesAction(), clearCacheAction(),
            clearPrivateDataAction()};
}

QAction *ProfileMenuActions::deleteProfileAction()
{
    if (!m_deleteProfileAction) {
        // The red danger row: a QWidgetAction so the menu paints it
        // danger-accented — a plain QAction cannot carry per-item
        // styling inside a QMenu.
        QWidgetAction *action = new QWidgetAction(this);
        QPushButton *row = new QPushButton(tr("Delete Profile…"));
        row->setFlat(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QLatin1String(
            "QPushButton { color: #c62828; text-align: left;"
            " border: none; background: transparent;"
            " padding: 6px 24px 6px 30px; font-weight: 600; }"
            "QPushButton:hover { background: rgba(198,40,40,0.14); }"));
        connect(row, &QPushButton::clicked, action, [action, row]() {
            // Dismiss the owning menu before the confirms exec — a
            // modal dialog behind an open menu reads as a hang.
            if (QMenu *menu = qobject_cast<QMenu*>(row->parentWidget()))
                menu->close();
            action->trigger();
        });
        action->setDefaultWidget(row);
        connect(action, &QAction::triggered, this, [this]() {
            requestProfileWipe(m_parent);
        });
        m_deleteProfileAction = action;
    }
    return m_deleteProfileAction;
}

QString ProfileMenuActions::wipeMarkerPath()
{
    return BrowserPaths::dataFilePath(
        QLatin1String("arora-profile-wipe.pending"));
}

bool ProfileMenuActions::wipePending()
{
    return QFile::exists(wipeMarkerPath());
}

bool ProfileMenuActions::scheduleProfileWipe()
{
    // The marker carries this process's pid so the starter can wait
    // out its teardown before wiping — exit-time writes (the session
    // flush, settings syncs, deferred clear sentinels) must land
    // before the stores are deleted, never into the fresh tree.
    QFile marker(wipeMarkerPath());
    if (!marker.open(QIODevice::WriteOnly))
        return false;
    marker.write(QByteArray::number(QCoreApplication::applicationPid()));
    marker.write("\n");
    marker.close();
    return true;
}

static bool processAlive(qint64 pid)
{
#if defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno != ESRCH;
#elif defined(Q_OS_WIN)
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return false;
    DWORD code = 0;
    const bool alive =
        GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    return true;
#endif
}

void ProfileMenuActions::waitForProcessExit(qint64 pid, int timeoutMs)
{
    if (pid <= 0)
        return;
#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs && processAlive(pid))
        QThread::msleep(25);
    if (processAlive(pid))
        qWarning() << "profile wipe: exiting process" << pid
                   << "still alive after" << timeoutMs
                   << "ms — wiping anyway";
#else
    // No liveness probe on this platform — a bounded settle covers
    // the exiting process's teardown window in practice.
    Q_UNUSED(timeoutMs);
    QThread::msleep(800);
#endif
}

bool ProfileMenuActions::applyPendingProfileWipe()
{
    const QString markerPath = wipeMarkerPath();
    if (!QFile::exists(markerPath))
        return false;

    // The marker's pid waits out the process that armed the wipe.
    // A stale marker's pid is long dead (the wait returns at once);
    // a recycled pid worst-cases the timeout, then the wipe runs.
    {
        QFile marker(markerPath);
        if (marker.open(QIODevice::ReadOnly)) {
            const qint64 ownerPid = QString::fromUtf8(marker.readAll())
                .trimmed().toLongLong();
            if (ownerPid > 0
                    && ownerPid != QCoreApplication::applicationPid())
                waitForProcessExit(ownerPid, 10000);
        }
    }

    // At boot nothing has constructed a profile, a manager or a
    // WebContents — the ClearFullWipe plan's coverage collapses to
    // plain removal: the named profile's storage (<data root>/<name>
    // — cookies under Network/, cache dirs, DOM storage, visited
    // links), the container roots, and every app-side store (history,
    // bookmarks, sessions, site decisions, extension data, avatar,
    // adblock lists, secure store) all live inside the data root;
    // the http cache and the fetch cache live under the cache root.
    // Downloads (user files), packages and the system tor binary sit
    // outside both and are deliberately kept.
    const QString dataRoot = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    const QString cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);

    // Never removeRecursively a root that does not look like Arora's
    // own store — an empty/misresolved QStandardPaths answer would
    // otherwise aim the wipe at a shared location.
    const auto ownTree = [](const QString &path) {
        return !path.isEmpty()
            && path != QDir::homePath()
            && path.contains(QCoreApplication::applicationName());
    };

    bool ok = true;
    if (ownTree(dataRoot)) {
        ok &= QDir(dataRoot).removeRecursively();
    } else {
        ok = false;
        qWarning() << "profile wipe: refusing unexpected data root"
                   << dataRoot;
    }
    if (!cacheRoot.isEmpty() && cacheRoot != dataRoot) {
        if (ownTree(cacheRoot)) {
            ok &= QDir(cacheRoot).removeRecursively();
        } else {
            ok = false;
            qWarning() << "profile wipe: refusing unexpected cache root"
                       << cacheRoot;
        }
    }

    // Every user setting dies with the profile — clear the store and
    // remove the backing file so nothing migrates a survivor forward.
    {
        QSettings settings;
        const QString settingsFile = settings.fileName();
        settings.clear();
        settings.sync();
        if (!settingsFile.isEmpty())
            QFile::remove(settingsFile);
    }

    // Redundant with the data-root removal unless the guard refused —
    // either way the marker must not outlive the wipe.
    QFile::remove(markerPath);
    qInfo() << "arora: profile wipe applied — booting a fresh profile";
    return ok;
}

bool ProfileMenuActions::requestProfileWipe(QWidget *parent)
{
    // Gate 1 — itemize exactly what dies.
    QMessageBox confirmBox(QMessageBox::Warning, tr("Delete Profile"),
        tr("<p>This will permanently delete the Arora profile:</p>"
           "<ul>"
           "<li>All settings and preferences</li>"
           "<li>Browsing, download and search history</li>"
           "<li>Bookmarks</li>"
           "<li>Cookies and site data</li>"
           "<li>The web cache</li>"
           "<li>Saved sessions</li>"
           "<li>Site permissions and decisions</li>"
           "<li>Extension data</li>"
           "<li>The profile name and avatar</li>"
           "</ul>"
           "<p>Arora then restarts with a fresh profile.  "
           "Downloaded files are not touched.</p>"),
        QMessageBox::NoButton, parent);
    QPushButton *continueButton =
        confirmBox.addButton(tr("Continue…"), QMessageBox::AcceptRole);
    QPushButton *cancelButton =
        confirmBox.addButton(QMessageBox::Cancel);
    confirmBox.setDefaultButton(cancelButton);
    confirmBox.exec();
    if (confirmBox.clickedButton() != continueButton)
        return false;

    // Gate 2 — type-to-confirm: no accidental click wipes a profile.
    ProfileWipeConfirmDialog confirmDialog(parent);
    if (confirmDialog.exec() != QDialog::Accepted)
        return false;

    // A live download would die with the quit — the same prompt the
    // normal quit path shows.  A decline leaves nothing behind.
    if (BrowserApplication::instance()
            && BrowserApplication::downloadManager()
            && !BrowserApplication::downloadManager()->allowQuit())
        return false;

    if (!scheduleProfileWipe()) {
        QMessageBox::warning(parent, tr("Delete Profile"),
            tr("The profile wipe could not be scheduled — "
               "nothing was deleted."));
        return false;
    }

    // Test seam (autotests + smokes): the marker is the observable
    // contract of this leg — the respawn and the exit run only in the
    // real binary.
    if (qEnvironmentVariableIsSet("ARORA_WIPE_NO_QUIT"))
        return true;

    // Respawn before dying: the fresh process sees the marker, waits
    // out this process's teardown, wipes and boots a new window — the
    // restart is part of the honest fix.
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                               QStringList()))
        qWarning() << "profile wipe: failed to spawn the restart process";

    // Quiesce like a signal-driven shutdown — pending AutoSaver
    // debounces flush, then the exit is recorded as clean so the
    // crash-loop counter never scores a deliberate wipe.
    if (BrowserApplication *application = BrowserApplication::instance())
        application->flushSessionsForTermination(0);
    else
        BrowserProfile::markSessionCleanExit(0);
    ::exit(0);
    return true; // unreachable
}
