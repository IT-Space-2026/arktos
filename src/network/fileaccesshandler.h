/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2009 Arora Browser
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

#ifndef FILEACCESSHANDLER_H
#define FILEACCESSHANDLER_H

#include "schemeaccesshandler.h"

#include <qpointer.h>

class QWebEngineUrlRequestJob;
class FileAccessHandler : public SchemeAccessHandler
{
    Q_OBJECT

public:
    FileAccessHandler(QObject *parent = nullptr);

    QByteArray scheme() const override;
    static QByteArray schemeName();
    static QUrl urlForLocalPath(const QString &path);

    // WK17: the engine-agnostic serving halves — the SEC02 initiator
    // allowlist and the directory-listing builder, shared with the
    // non-WebEngine scheme adapters (Servo's custom-protocol path)
    // so a second backend refuses and serves identically.
    // directoryListing() is the GUI-thread work replyToJob() always
    // did; iconSizePx is caller-supplied (backend-0 reads it from
    // QWebEngineSettings, other engines pick their own metric).
    static bool initiatorAllowed(const QString &scheme);
    enum class ListingError { None, NotFound, Unreadable, Internal };
    static ListingError directoryListing(const QString &path,
                                         int iconSizePx,
                                         QByteArray *html);

    virtual void requestStarted(QWebEngineUrlRequestJob *job) override;

private:
    void replyToJob(QPointer<QWebEngineUrlRequestJob> job);
};

#endif // FILEACCESSHANDLER_H
