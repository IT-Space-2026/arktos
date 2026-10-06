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
    BrowserApplication app(argc, argv); \
    TestObject tc; \
    return QTest::qExec(&tc, argc, argv); \
}

#endif

