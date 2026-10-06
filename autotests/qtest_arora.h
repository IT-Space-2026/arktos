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
#include <qdialog.h>
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

