/*
 * Copyright 2008-2009 Benjamin K. Stuhl <bks24@cornell.edu>
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#include <qdatetime.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qsqldatabase.h>
#include <qsqlerror.h>
#include <qsqlquery.h>
#include <qtextstream.h>
#include <qvariant.h>

#include "singleapplication.h"
#include "historymanager.h"

static HistoryEntry formatEntry(const QString &url, const QString &title, qlonglong prdate)
{
    // Firefox stores visit_date as microseconds since the unix epoch.
    QDateTime dateTime = QDateTime::fromSecsSinceEpoch(prdate / 1000000)
            .addMSecs((prdate % 1000000) / 1000)
            .toLocalTime();
    HistoryEntry entry(url, dateTime, title);
    return entry;
}

int main(int argc, char **argv)
{
    SingleApplication application(argc, argv);
    // Match the browser's scope so QStandardPaths finds the same data dir.
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setOrganizationDomain(QLatin1String("arora-browser.org"));
    QCoreApplication::setApplicationName(QLatin1String("Arora"));

    if (application.sendMessage(QByteArray())) {
        qWarning() << "To prevent the loss of any history please exit Arora while this is tool is being run";
        return 1;
    }

    QStringList args = application.arguments();
    args.takeFirst();
    if (args.isEmpty()) {
        QTextStream stream(stdout);
        stream << "arora-placesimport is a tool for importing browser history from Firefox 3 and up" << Qt::endl;
        stream << "arora-placesinfo ~/.mozilla/firefox/[profile-dir]/places.sqlite" << Qt::endl;
        return 0;
    }

    QSqlDatabase placesDatabase = QSqlDatabase::addDatabase(QLatin1String("QSQLITE"));
    placesDatabase.setDatabaseName(args.first());

    if (!placesDatabase.open()) {
        qWarning("Unable to open database: %s", qPrintable(placesDatabase.lastError().text()));
        return 1;
    }

    QSqlQuery historyQuery(
        QLatin1String("SELECT moz_places.url, moz_places.title, moz_historyvisits.visit_date "
        "FROM moz_places, moz_historyvisits "
        "WHERE moz_places.id = moz_historyvisits.place_id;"));
    historyQuery.setForwardOnly(true);

    if (!historyQuery.exec()) {
        qWarning("Unable to extract history: %s.  Is Firefox running?", qPrintable(historyQuery.lastError().text()));
        return 1;
    }

    HistoryManager manager;
    QList<HistoryEntry> history = manager.history();
    while (historyQuery.next()) {
        QString url = historyQuery.value(0).toString();
        QString title = historyQuery.value(1).toString();
        qlonglong prdate = historyQuery.value(2).toLongLong();
        HistoryEntry entry = formatEntry(url, title, prdate);
        history.append(entry);
    }
    manager.setHistory(history);

    return 0;
}
