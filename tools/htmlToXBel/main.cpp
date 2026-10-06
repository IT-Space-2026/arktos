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

#include "converter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QFile>

#include <tuple>

/*!
    A tool to convert html bookmark files into the xbel format.

    The html bookmark files should be DOCTYPE: NETSCAPE-Bookmark-file-1

    More information about XBel can be found here: http://pyxml.sourceforge.net/topics/xbel/
*/

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    QFile inFile;
    QFile outFile;

    // Either read in from stdin and output to stdout
    // or read in from a file and output to a file
    // Example: ./app foo.html -o bar.xbel
    bool setInput = false;
    bool setOutput = false;
    QStringList args = application.arguments();
    args.takeFirst();
    for (const QString &arg : args) {
        if (arg == QLatin1String("-o")) {
            setOutput = true;
        } else if (setOutput) {
            outFile.setFileName(arg);
            if (!outFile.open(QIODevice::WriteOnly)) {
                qWarning() << "Unable to open" << arg << "for writing";
                return 1;
            }
        } else if (QFile::exists(arg)) {
            setInput = true;
            inFile.setFileName(arg);
            if (!inFile.open(QIODevice::ReadOnly)) {
                qWarning() << "Unable to open" << arg << "for reading";
                return 1;
            }
        } else {
            qWarning() << "Usage: htmlToXBel"
                       << "[stdin|htmlfile]" << "[stdout|-o outFile]";
            return 1;
        }
    }

    if (!setInput)
        std::ignore = inFile.open(stdin, QIODevice::ReadOnly);
    if (!setOutput)
        std::ignore = outFile.open(stdout, QIODevice::WriteOnly);
    if (inFile.openMode() == QIODevice::NotOpen
        || outFile.openMode() == QIODevice::NotOpen) {
        qWarning() << "Unable to open streams";
        return 1;
    }

    const QString html = QString::fromUtf8(inFile.readAll());

    const int rc = convertHtmlToXbel(html, &outFile);
    if (rc != 0)
        qWarning() << "Error while extracting bookmarks.";
    return rc;
}
