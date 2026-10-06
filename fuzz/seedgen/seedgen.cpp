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

// Generates the binary seed corpus for the QDataStream-format fuzz
// targets (history blocks, cookie-jar state, bounded lists, sealed
// blobs).  The text-format targets seed from the existing test
// fixtures instead.
//
// Usage: seedgen <corpus-root>   — writes corpus/<target>/seedNN files.
// Rebuild + rerun after a format change; the outputs are committed so
// the fuzzers start from valid structure, not raw zeros.

#include "historyparser.h"
#include "networkcookiejar.h"
#include "securestore.h"
#include "streamingutils.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qnetworkcookie.h>
#include <qstandardpaths.h>

static void seed(const QString &dir, const QString &name, const QByteArray &data)
{
    QDir().mkpath(dir);
    QFile f(dir + QLatin1Char('/') + name);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setApplicationName(QLatin1String("Arora-fuzz"));
    QStandardPaths::setTestModeEnabled(true);

    const QString root = argc > 1
            ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("corpus");

    // --- historyformat: block-framed entry stream -----------------
    {
        const QString dir = root + QLatin1String("/historyformat");
        auto block = [](const QString &url, const QDateTime &dt,
                        const QString &title) {
            QByteArray b;
            QDataStream s(&b, QIODevice::WriteOnly);
            s << HistoryParser::Version << url << dt << title;
            return b;
        };
        QByteArray valid;
        {
            QDataStream out(&valid, QIODevice::WriteOnly);
            out << block(QStringLiteral("http://a.example/"),
                         QDateTime::fromSecsSinceEpoch(1700000000),
                         QStringLiteral("Page A"));
            out << block(QStringLiteral("http://b.example/x"),
                         QDateTime::fromSecsSinceEpoch(1699999000),
                         QStringLiteral("Page B"));
            out << block(QStringLiteral("http://b.example/x"), // dup
                         QDateTime::fromSecsSinceEpoch(1699999000),
                         QStringLiteral("Page B renamed"));
        }
        seed(dir, QStringLiteral("valid3"), valid);

        QByteArray unsorted;
        {
            QDataStream out(&unsorted, QIODevice::WriteOnly);
            out << block(QStringLiteral("http://old.example/"),
                         QDateTime::fromSecsSinceEpoch(1690000000),
                         QStringLiteral("old"));
            out << block(QStringLiteral("http://new.example/"),
                         QDateTime::fromSecsSinceEpoch(1705000000),
                         QStringLiteral("new"));
        }
        seed(dir, QStringLiteral("unsorted"), unsorted);

        QByteArray badVersion;
        {
            QDataStream out(&badVersion, QIODevice::WriteOnly);
            QByteArray b;
            {
                QDataStream s(&b, QIODevice::WriteOnly);
                s << quint32(1) << QStringLiteral("http://v1/")
                  << QDateTime::fromSecsSinceEpoch(1700000000)
                  << QStringLiteral("t");
            }
            out << b << block(QStringLiteral("http://ok/"),
                              QDateTime::fromSecsSinceEpoch(1700000100),
                              QStringLiteral("ok"));
        }
        seed(dir, QStringLiteral("badversion"), badVersion);

        seed(dir, QStringLiteral("truncated"), valid.left(valid.size() - 3));
        // Bogus block length prefix — the decoder must stop cleanly.
        seed(dir, QStringLiteral("badlen"), QByteArray("\x7f\xff\xff\xff", 4));
        seed(dir, QStringLiteral("empty"), QByteArray());
    }

    // --- cookiejarstate: NetworkCookieJar trie blob ----------------
    {
        const QString dir = root + QLatin1String("/cookiejarstate");
        // saveState/setAllCookies are protected on QNetworkCookieJar.
        class SeedJar : public NetworkCookieJar
        {
        public:
            using NetworkCookieJar::saveState;
            using NetworkCookieJar::setAllCookies;
        };
        SeedJar jar;
        const QDateTime future =
                QDateTime::currentDateTime().addDays(30);
        QList<QNetworkCookie> cookies;
        QNetworkCookie a("sid", "abc123");
        a.setDomain(QStringLiteral(".example.com"));
        a.setPath(QStringLiteral("/"));
        a.setExpirationDate(future);
        cookies << a;
        QNetworkCookie b("pref", "dark=1");
        b.setDomain(QStringLiteral("sub.example.co.uk"));
        b.setSecure(true);
        cookies << b;
        QNetworkCookie c("sess", "xyz");  // session cookie, no expiry
        c.setDomain(QStringLiteral("example.org"));
        cookies << c;
        jar.setAllCookies(cookies);
        const QByteArray state = jar.saveState();
        seed(dir, QStringLiteral("state3"), state);
        seed(dir, QStringLiteral("truncated"), state.left(state.size() / 2));
        seed(dir, QStringLiteral("magic-only"), state.left(8));
        seed(dir, QStringLiteral("empty"), QByteArray());
    }

    // --- streamingutils: bounded-list payloads ---------------------
    {
        const QString dir = root + QLatin1String("/streamingutils");
        QByteArray strings;
        {
            QDataStream out(&strings, QIODevice::WriteOnly);
            out << QStringList{QStringLiteral("a"), QStringLiteral("b=c"),
                               QStringLiteral("")};
        }
        seed(dir, QStringLiteral("stringlist"), strings);

        // QList<QNetworkCookie> serializes as a list of toRawForm()
        // byte arrays (the operators live in networkcookiejar_p.h and
        // are non-inline — don't include that header here).
        QByteArray cookiesBlob;
        {
            QDataStream out(&cookiesBlob, QIODevice::WriteOnly);
            QList<QByteArray> list;
            list << QNetworkCookie("k", "v").toRawForm()
                 << QNetworkCookie("k2", "v2").toRawForm();
            out << list;
        }
        seed(dir, QStringLiteral("cookielist"), cookiesBlob);

        // Form-shaped payload: elements + url + name + flag.
        QByteArray form;
        {
            QDataStream out(&form, QIODevice::WriteOnly);
            out << QStringList{QStringLiteral("user=u"),
                               QStringLiteral("pass=p")};
            out << QStringLiteral("http://site.example/login");
            out << QStringLiteral("loginform");
            out << true;
        }
        seed(dir, QStringLiteral("form"), form);

        // Hostile count prefix: claims 0x7fffffff items.
        seed(dir, QStringLiteral("hugecount"),
             QByteArray("\x7f\xff\xff\xff", 4));
        seed(dir, QStringLiteral("empty"), QByteArray());
    }

    // --- securestore: sealed blob ----------------------------------
    {
        const QString dir = root + QLatin1String("/securestore");
        if (SecureStore::isAvailable()) {
            const QByteArray sealed = SecureStore::seal(
                    QByteArrayLiteral("http://x/|user|pass"));
            seed(dir, QStringLiteral("sealed"), sealed);
            QByteArray tampered = sealed;
            if (tampered.size() > 20)
                tampered[tampered.size() - 5] ^= 0xff;
            seed(dir, QStringLiteral("tampered"), tampered);
        }
        seed(dir, QStringLiteral("magic-only"), QByteArrayLiteral("ARSEC1"));
        seed(dir, QStringLiteral("empty"), QByteArray());
        seed(dir, QStringLiteral("prefix"),
             QByteArrayLiteral("arsec1:aGVsbG8="));
    }

    return 0;
}
