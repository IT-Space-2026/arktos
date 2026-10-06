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

// libFuzzer target for NetworkCookieJar::restoreState — the persisted
// cookie trie (QDataStream format) is a hostile blob if the profile
// dir is corrupted or hand-edited.

#include "networkcookiejar.h"

#include <qnetworkcookie.h>
#include <qurl.h>

#include <stddef.h>
#include <stdint.h>

// The state (de)serialization API is protected — it's meant for a
// persisting owner, not public callers.  Surface it for the harness.
class FuzzJar : public NetworkCookieJar
{
public:
    using NetworkCookieJar::allCookies;
    using NetworkCookieJar::endSession;
    using NetworkCookieJar::restoreState;
    using NetworkCookieJar::saveState;
    using NetworkCookieJar::setAllCookies;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzJar jar;
    jar.restoreState(QByteArray(reinterpret_cast<const char *>(data),
                                qsizetype(size)));

    // Walk whatever state was restored.
    const QList<QNetworkCookie> cookies = jar.allCookies();
    jar.cookiesForUrl(QUrl(QLatin1String("http://example.com/")));
    jar.cookiesForUrl(QUrl(QLatin1String("https://sub.example.co.uk/a/b")));
    if (!cookies.isEmpty())
        jar.setCookiesFromUrl(cookies, QUrl(QLatin1String("http://example.com/")));
    jar.saveState();
    jar.endSession();
    return 0;
}
