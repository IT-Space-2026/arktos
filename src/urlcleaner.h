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

#ifndef URLCLEANER_H
#define URLCLEANER_H

#include <qurl.h>
#include <qstringlist.h>

// POL01: strips known tracking query parameters from a URL — the
// engine behind 'Copy Clean Link' in the context menus.  The blocklist
// is data-driven: the curated defaults below plus user additions from
// the urlcleaner/extraParams QSettings key, and runtime additions for
// tests.  An entry ending in '*' is a prefix match ("utm_*" removes
// utm_source, utm_medium, ...); everything else is an exact,
// case-insensitive parameter-name match.
class UrlCleaner
{
public:
    // Returns the URL with tracking parameters removed.  Only http(s)
    // URLs carry trackers — other schemes (mailto:, data:,
    // javascript:, ...) are returned untouched, as are URLs without a
    // query.  All other query items keep their order; the fragment,
    // userinfo, port and path are preserved.
    static QUrl cleanedUrl(const QUrl &url);

    // The effective blocklist: built-in defaults followed by the
    // QSettings and runtime additions.
    static QStringList trackingParameters();

    // Merges more parameter names/prefixes into the blocklist for this
    // process (used by tests; persists for the session only — the
    // urlcleaner/extraParams QSettings key is the user-facing list).
    static void setAdditionalParameters(const QStringList &parameters);

private:
    static bool isTrackingParameter(const QString &name);
};

#endif
