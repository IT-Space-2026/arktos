/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef QTEST_ARORA_H
#define QTEST_ARORA_H

#include <qtest.h>

#include <browserapplication.h>

#include <qapplication.h>
#include <qcoreapplication.h>
#include <cstdio>
#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif
#include <qfileinfo.h>
#include <qstandardpaths.h>
#include <qabstractbutton.h>
#include <qdialog.h>
#include <qmessagebox.h>
#include <qtimer.h>

#include "qtry.h"

// COV04: the offscreen QPA still runs real nested modal loops for
// exec()d dialogs, so tests that poke dialog-launching code paths need
// a scheduled dismissal.  acceptModal()/rejectModal() act on whatever
// is the active modal widget when the timer fires — a no-op when the
// dialog never appeared (the "not found" branches still get covered).
inline void acceptModal(int msec = 100)
{
    QTimer::singleShot(msec, qApp, []() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QDialog *dialog = qobject_cast<QDialog*>(widget))
            dialog->accept();
        else if (widget)
            widget->close();
    });
}

inline void rejectModal(int msec = 100)
{
    QTimer::singleShot(msec, qApp, []() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QDialog *dialog = qobject_cast<QDialog*>(widget))
            dialog->reject();
        else if (widget)
            widget->close();
    });
}

// SEC02: clicks a specific standard button on the next modal
// QMessageBox (QDialog::accept() does not map back to one).  A
// repeating timer does the clicking because a modal exec() swallows
// the caller's event pumps — polling between QTest::qWait() calls
// would never run while the dialog is open.
inline bool answerModal(QMessageBox::StandardButton button,
                        int timeoutMs = 5000)
{
    bool clicked = false;
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, qApp,
                     [button, &clicked]() {
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box)
            return;
        QAbstractButton *b = box->button(button);
        if (b)
            b->click();
        else
            box->reject();
        clicked = true;
    });
    timer.start();
    for (int waited = 0; !clicked && waited < timeoutMs; waited += 50)
        QTest::qWait(50);
    return clicked;
}

#undef QTEST_MAIN

#define QTEST_MAIN(TestObject) \
int main(int argc, char *argv[]) \
{ \
    Q_INIT_RESOURCE(htmls); \
    Q_INIT_RESOURCE(data); \
    /* EXT06: the BrowserApplication ctor brings up the real named \
       "arora" profile before any test leg can run — services wired \
       in prepareProfile() (extensions, cookies, history) then write \
       into the user's live profile.  runTests.sh already sandboxes \
       HOME for `make check`, but a test binary run standalone \
       leaked its installs into ~/.local/share/Arora (the \
       tst-upd/tst-old/tst-noext extensions came from exactly this). \
       QStandardPaths test mode moves every app data/config location \
       under a qttest/ subdir, and must be enabled before the app \
       constructs the profile below. */ \
    QStandardPaths::setTestModeEnabled(true); \
    BrowserApplication app(argc, argv); \
    /* SRCH09/SRCH11: the BrowserApplication ctor resets the \
       application name to "Arora", so a test that never calls \
       setApplicationName() wrote its QSettings into the user's real \
       ~/.config/Arora/Arora.conf — tst_tabwidget's opt-out leg \
       leaked urlloading/searchEngineFallback=false into the live \
       config every `make check` (the recurring "search bar doesn't \
       work" report).  Rename to the test binary's basename so every \
       test gets its own isolated scope; tests that want a custom \
       name still call setApplicationName() inside initTestCase, \
       which runs after this. */ \
    QCoreApplication::setApplicationName( \
        QFileInfo(QCoreApplication::applicationFilePath()) \
            .baseName()); \
    TestObject tc; \
    const int rc = QTest::qExec(&tc, argc, argv); \
    /* QUAL05: QtWebEngine's teardown is not stable under the \
       offscreen QPA — ~BrowserApplication releasing the profile \
       while Chromium still has live render objects can put the \
       crash handler into a "Received signal 11" re-fault loop that \
       never exits (tst_browsermainwindow burned whole \
       ARORA_TEST_TIMEOUT windows emitting multi-GB logs before the \
       guard killed it).  The test verdict is fully reported by the \
       time qExec returns, and destructors here only release \
       engine-side resources — flush and exit without running them. */ \
    fflush(nullptr); \
    _exit(rc); \
    return rc; \
}

#endif

