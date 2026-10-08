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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// POPUP01: the popup-blocking gate — global enable flag, persistent
// + session host exceptions, normalization and parent-domain matching,
// and the scheme check that keeps internal pages out of the blocker.

#include <QtTest/QtTest>

#include <qsettings.h>

#include "popupblocker.h"

class tst_PopupBlocker : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void init();

private slots:
    void enabledFlag();
    void shouldBlockSchemes();
    void hostExceptions();
    void sessionExceptions();
    void persistence();
    void normalization();
    void clearAll();
};

void tst_PopupBlocker::initTestCase()
{
    QCoreApplication::setApplicationName("tst_popupblocker");
    QSettings settings;
    settings.clear();
}

void tst_PopupBlocker::init()
{
    // QSettings-backed state persists between runs — start each test
    // from the enabled-with-no-exceptions default.
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->clearAllowedHosts();
    blocker->clearSessionHosts();
    blocker->setEnabled(true);
}

// The Settings > Privacy checkbox owns this flag; off means nothing
// is ever blocked.
void tst_PopupBlocker::enabledFlag()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    QVERIFY(blocker->isEnabled());
    QVERIFY(blocker->shouldBlock(QUrl("http://example.com/")));

    blocker->setEnabled(false);
    QVERIFY(!blocker->isEnabled());
    QVERIFY(!blocker->shouldBlock(QUrl("http://example.com/")));
}

// Only http/https pages have pop-ups worth gating — host-less and
// internal schemes keep their windows (the allow list could not
// express them anyway).
void tst_PopupBlocker::shouldBlockSchemes()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    QVERIFY(blocker->shouldBlock(QUrl("http://example.com/")));
    QVERIFY(blocker->shouldBlock(QUrl("https://example.com/")));
    QVERIFY(!blocker->shouldBlock(QUrl("file:///tmp/x.html")));
    QVERIFY(!blocker->shouldBlock(QUrl("about:blank")));
    QVERIFY(!blocker->shouldBlock(QUrl("data:text/html,<p>x</p>")));
    QVERIFY(!blocker->shouldBlock(QUrl("qrc:/startpage.html")));
    QVERIFY(!blocker->shouldBlock(QUrl()));
}

void tst_PopupBlocker::hostExceptions()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    QVERIFY(!blocker->isAllowedHost(QLatin1String("example.com")));

    blocker->allowHost(QLatin1String("example.com"));
    QVERIFY(blocker->isAllowedHost(QLatin1String("example.com")));
    // A rule recorded on the parent covers subdomains — Firefox's
    // "Block pop-up windows -> Exceptions" semantics.
    QVERIFY(blocker->isAllowedHost(QLatin1String("www.example.com")));
    QVERIFY(blocker->isAllowedHost(
        QLatin1String("deep.sub.example.com")));
    // ...but never siblings or partial strings.
    QVERIFY(!blocker->isAllowedHost(QLatin1String("notexample.com")));
    QVERIFY(!blocker->isAllowedHost(QLatin1String("ample.com")));
    QVERIFY(!blocker->isAllowedHost(QLatin1String("example.org")));

    QVERIFY(!blocker->shouldBlock(QUrl("http://example.com/")));
    QVERIFY(!blocker->shouldBlock(QUrl("http://www.example.com/x")));
    QVERIFY(blocker->shouldBlock(QUrl("http://other.org/")));

    blocker->removeAllowedHost(QLatin1String("example.com"));
    QVERIFY(blocker->shouldBlock(QUrl("http://example.com/")));
}

void tst_PopupBlocker::sessionExceptions()
{
    PopupBlocker *blocker = PopupBlocker::instance();

    // Session exceptions behave like persistent ones for matching...
    blocker->allowHost(QLatin1String("session.test"), false);
    QVERIFY(blocker->isAllowedHost(QLatin1String("session.test")));
    QVERIFY(blocker->isAllowedHost(QLatin1String("www.session.test")));
    // ...but never land in the persistent list or on disk.
    QVERIFY(blocker->allowedHosts().isEmpty());
    QVERIFY(!QSettings().contains(
        QLatin1String("popupExceptions/allowed")));

    blocker->clearSessionHosts();
    QVERIFY(!blocker->isAllowedHost(QLatin1String("session.test")));

    // removeAllowedHost also scrubs the session overlay.
    blocker->allowHost(QLatin1String("session.test"), false);
    blocker->removeAllowedHost(QLatin1String("session.test"));
    QVERIFY(!blocker->isAllowedHost(QLatin1String("session.test")));
}

// The persistent layer round-trips through QSettings under
// popupExceptions/allowed — a fresh instance sees the same rules.
void tst_PopupBlocker::persistence()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->allowHost(QLatin1String("persistent.example"));

    const QStringList stored = QSettings().value(
        QLatin1String("popupExceptions/allowed")).toStringList();
    QCOMPARE(stored, QStringList() << QLatin1String("persistent.example"));

    PopupBlocker reloaded;
    QVERIFY(reloaded.isAllowedHost(QLatin1String("persistent.example")));
    QVERIFY(reloaded.isAllowedHost(QLatin1String("a.persistent.example")));
    QCOMPARE(reloaded.allowedHosts(),
             QStringList() << QLatin1String("persistent.example"));

    blocker->removeAllowedHost(QLatin1String("persistent.example"));
    QVERIFY(!QSettings().contains(
        QLatin1String("popupExceptions/allowed")));
}

// Case, surrounding dots and trailing dots all fold to the same rule.
void tst_PopupBlocker::normalization()
{
    PopupBlocker *blocker = PopupBlocker::instance();

    blocker->allowHost(QLatin1String(".Example.COM."));
    QVERIFY(blocker->isAllowedHost(QLatin1String("example.com")));
    QVERIFY(blocker->isAllowedHost(QLatin1String("EXAMPLE.COM")));
    QCOMPARE(blocker->allowedHosts(),
             QStringList() << QLatin1String("example.com"));

    // Re-adding the same host under another spelling does not stack
    // duplicates.
    blocker->allowHost(QLatin1String("example.com"));
    QCOMPARE(blocker->allowedHosts().count(), 1);

    // Empty and punctuation-only hosts are refused outright.
    blocker->allowHost(QString());
    blocker->allowHost(QLatin1String("..."));
    QCOMPARE(blocker->allowedHosts().count(), 1);
}

void tst_PopupBlocker::clearAll()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->allowHost(QLatin1String("a.example"));
    blocker->allowHost(QLatin1String("b.example"));
    QCOMPARE(blocker->allowedHosts().count(), 2);

    blocker->clearAllowedHosts();
    QVERIFY(blocker->allowedHosts().isEmpty());
    QVERIFY(!blocker->isAllowedHost(QLatin1String("a.example")));
    QVERIFY(!QSettings().contains(
        QLatin1String("popupExceptions/allowed")));
}

QTEST_MAIN(tst_PopupBlocker)
#include "tst_popupblocker.moc"
