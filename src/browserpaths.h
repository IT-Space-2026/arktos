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

#ifndef BROWSERPATHS_H
#define BROWSERPATHS_H

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qstandardpaths.h>

// Application data-directory helpers that used to live on
// BrowserApplication.  They are header-inline so ported modules can use
// them while browserapplication.cpp is still uncompiled (MIG15).
namespace BrowserPaths {

// Returns <app data dir>/<fileName>, creating the directory if needed.
// Was BrowserApplication::dataFilePath(); MIG15 should delegate to this.
inline QString dataFilePath(const QString &fileName)
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        directory = QDir::homePath() + QLatin1String("/.") + QCoreApplication::applicationName();
    if (!QFile::exists(directory)) {
        QDir dir;
        dir.mkpath(directory);
    }
    return directory + QLatin1Char('/') + fileName;
}

} // namespace BrowserPaths

#endif // BROWSERPATHS_H
