/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

// COV02: the volatile QNetworkCookieJar used by the app-side
// QNetworkAccessManager (MIG04).  Pure logic — domain matching,
// path/secure/expiry filtering, second-level-domain blacklist,
// saveState/restoreState — so it is exercised directly rather than
// through a QNAM fetch.

#include <QtTest/QtTest>
#include <networkcookiejar.h>

class tst_NetworkCookieJar : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void cookiesForUrl_data();
    void cookiesForUrl();
    void cookieOrdering();
    void setCookiesFromUrl_data();
    void setCookiesFromUrl();
    void replaceCookie();
    void expiredSetDeletesExisting();
    void secondLevelDomains();
    void saveRestore();
    void restoreStateInvalid();
    void endSession();
    void allCookies();
};

// Subclass that exposes the protected functions.
class SubNetworkCookieJar : public NetworkCookieJar
{
public:
    QByteArray call_saveState() const
        { return SubNetworkCookieJar::saveState(); }
    bool call_restoreState(const QByteArray &state)
        { return SubNetworkCookieJar::restoreState(state); }
    void call_endSession()
        { SubNetworkCookieJar::endSession(); }
    QList<QNetworkCookie> call_allCookies() const
        { return SubNetworkCookieJar::allCookies(); }
    void call_setAllCookies(const QList<QNetworkCookie> &cookieList)
        { SubNetworkCookieJar::setAllCookies(cookieList); }
    void call_setSecondLevelDomains(const QStringList &domains)
        { SubNetworkCookieJar::setSecondLevelDomains(domains); }
};

static QNetworkCookie makeCookie(const QByteArray &name, const QByteArray &value,
                                 const QString &domain = QString(),
                                 const QString &path = QString())
{
    QNetworkCookie cookie(name, value);
    if (!domain.isEmpty())
        cookie.setDomain(domain);
    if (!path.isEmpty())
        cookie.setPath(path);
    return cookie;
}

void tst_NetworkCookieJar::initTestCase()
{
    QCoreApplication::setApplicationName("tst_networkcookiejar");
}

void tst_NetworkCookieJar::cleanupTestCase()
{
}

void tst_NetworkCookieJar::init()
{
}

void tst_NetworkCookieJar::cleanup()
{
}

void tst_NetworkCookieJar::cookiesForUrl_data()
{
    QTest::addColumn<QUrl>("setUrl");
    QTest::addColumn<QNetworkCookie>("cookie");
    QTest::addColumn<QUrl>("getUrl");
    QTest::addColumn<bool>("present");

    // Set at the site root so cookies inherit "/" as their default
    // path (a deeper url would pin them under /dir/).
    QUrl site("http://example.com/");
    QNetworkCookie hostCookie = makeCookie("a", "b");
    // setCookiesFromUrl assigns the url's host to domain-less cookies,
    // but the stored copy (and thus cookiesForUrl input) has no domain.
    QTest::newRow("host-cookie") << site << hostCookie
                                 << QUrl("http://example.com/") << true;

    QTest::newRow("domain-exact") << site << makeCookie("a", "b", "example.com")
                                 << QUrl("http://example.com/") << true;
    QTest::newRow("domain-subdomain") << site << makeCookie("a", "b", "example.com")
                                      << QUrl("http://www.example.com/") << true;
    QTest::newRow("domain-other-host") << site << makeCookie("a", "b", "other.com")
                                       << QUrl("http://www.example.com/") << false;
    QTest::newRow("domain-suffix-not-matched") << site << makeCookie("a", "b", "ample.com")
                                               << QUrl("http://example.com/") << false;

    // Path matching: /a matches /a/b but not /other.
    QTest::newRow("path-match") << QUrl("http://example.com/a/b")
                                << makeCookie("a", "b", "example.com", "/a")
                                << QUrl("http://example.com/a/c") << true;
    QTest::newRow("path-mismatch") << QUrl("http://example.com/a/b")
                                   << makeCookie("a", "b", "example.com", "/a")
                                   << QUrl("http://example.com/other") << false;

    QNetworkCookie secure = makeCookie("a", "b", "example.com");
    secure.setSecure(true);
    QTest::newRow("secure-over-http") << site << secure
                                      << QUrl("http://example.com/") << false;
    QTest::newRow("secure-over-https") << site << secure
                                       << QUrl("https://example.com/") << true;

    QNetworkCookie expired = makeCookie("a", "b", "example.com");
    expired.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(-1));
    QTest::newRow("expired") << site << expired
                             << QUrl("http://example.com/") << false;

    // file:// urls map to host "localhost".
    QTest::newRow("file-localhost") << QUrl("file:///tmp/x")
                                    << makeCookie("a", "b", "localhost")
                                    << QUrl("file:///tmp/y") << true;
}

// public QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const
void tst_NetworkCookieJar::cookiesForUrl()
{
    QFETCH(QUrl, setUrl);
    QFETCH(QNetworkCookie, cookie);
    QFETCH(QUrl, getUrl);
    QFETCH(bool, present);

    SubNetworkCookieJar jar;
    jar.setCookiesFromUrl(QList<QNetworkCookie>() << cookie, setUrl);

    QList<QNetworkCookie> result = jar.cookiesForUrl(getUrl);
    QCOMPARE(result.isEmpty(), !present);
    if (present)
        QCOMPARE(result.first().name(), cookie.name());
}

// Longer (more specific) paths sort first.
void tst_NetworkCookieJar::cookieOrdering()
{
    SubNetworkCookieJar jar;
    QUrl setUrl("http://example.com/a/b/c");
    QList<QNetworkCookie> list;
    list << makeCookie("shallow", "1", "example.com", "/a")
         << makeCookie("deep", "2", "example.com", "/a/b");
    jar.setCookiesFromUrl(list, setUrl);

    QList<QNetworkCookie> result = jar.cookiesForUrl(QUrl("http://example.com/a/b/x"));
    QCOMPARE(result.count(), 2);
    QCOMPARE(result.at(0).name(), QByteArray("deep"));
    QCOMPARE(result.at(1).name(), QByteArray("shallow"));
}

void tst_NetworkCookieJar::setCookiesFromUrl_data()
{
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<QNetworkCookie>("cookie");
    QTest::addColumn<bool>("accepted");

    QUrl site("http://example.com/dir/page.html");

    QTest::newRow("host-cookie") << site << makeCookie("a", "b") << true;
    QTest::newRow("own-domain") << site << makeCookie("a", "b", "example.com") << true;
    QTest::newRow("leading-dot") << site << makeCookie("a", "b", ".example.com") << true;
    // A subdomain may set cookies for its parent domain.
    QTest::newRow("subdomain-sets-parent")
            << QUrl("http://www.example.com/") << makeCookie("a", "b", "example.com") << true;
    QTest::newRow("foreign-domain")
            << site << makeCookie("a", "b", "other.com") << false;
    // "ample.com" is a suffix string but not a domain suffix.
    QTest::newRow("suffix-not-domain")
            << site << makeCookie("a", "b", "ample.com") << false;
    // Two-label cookies on a blacklisted second-level domain are rejected.
    QTest::newRow("blacklisted-co.uk")
            << QUrl("http://foo.co.uk/") << makeCookie("a", "b", ".co.uk") << false;
    QTest::newRow("deeper-co.uk")
            << QUrl("http://www.foo.co.uk/") << makeCookie("a", "b", "foo.co.uk") << true;
    // Single-label domains: only localhost is accepted.
    QTest::newRow("localhost-file")
            << QUrl("file:///tmp/x") << makeCookie("a", "b", "localhost") << true;
    QTest::newRow("single-label")
            << site << makeCookie("a", "b", "com") << false;
    // Insane path lengths are dropped.
    QNetworkCookie longPath = makeCookie("a", "b", "example.com", QString(2000, QLatin1Char('a')));
    QTest::newRow("path-too-long") << site << longPath << false;
    // Cookies that are already dead on arrival are not stored.
    QNetworkCookie dead = makeCookie("a", "b", "example.com");
    dead.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(-1));
    QTest::newRow("already-expired") << site << dead << false;
}

// public bool setCookiesFromUrl(const QList<QNetworkCookie> &cookieList, const QUrl &url)
void tst_NetworkCookieJar::setCookiesFromUrl()
{
    QFETCH(QUrl, url);
    QFETCH(QNetworkCookie, cookie);
    QFETCH(bool, accepted);

    SubNetworkCookieJar jar;
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>() << cookie, url), accepted);
    QCOMPARE(jar.call_allCookies().isEmpty(), !accepted);
}

// Same name+domain+path replaces the stored cookie.
void tst_NetworkCookieJar::replaceCookie()
{
    SubNetworkCookieJar jar;
    QUrl url("http://example.com/");
    jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("n", "v1", "example.com"), url);
    jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("n", "v2", "example.com"), url);

    QList<QNetworkCookie> cookies = jar.call_allCookies();
    QCOMPARE(cookies.count(), 1);
    QCOMPARE(cookies.first().value(), QByteArray("v2"));
}

// Setting an already-expired cookie deletes the live copy.  `changed`
// stays false — the jar reports no new cookie was stored.
void tst_NetworkCookieJar::expiredSetDeletesExisting()
{
    SubNetworkCookieJar jar;
    QUrl url("http://example.com/");
    jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("n", "v1", "example.com"), url);
    QCOMPARE(jar.call_allCookies().count(), 1);

    QNetworkCookie dead = makeCookie("n", "v2", "example.com");
    dead.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(-1));
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>() << dead, url), false);
    QCOMPARE(jar.call_allCookies().count(), 0);
}

// protected void setSecondLevelDomains(const QStringList &)
void tst_NetworkCookieJar::secondLevelDomains()
{
    SubNetworkCookieJar jar;
    // "uk" is in the bundled blacklist.
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("a", "b", ".co.uk"),
                                   QUrl("http://foo.co.uk/")), false);

    // A custom list replaces the bundled one.
    jar.call_setSecondLevelDomains(QStringList() << QLatin1String("zz"));
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("a", "b", ".foo.zz"),
                                   QUrl("http://www.foo.zz/")), false);
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>() << makeCookie("a", "b", ".co.uk"),
                                   QUrl("http://foo.co.uk/")), true);
}

// protected QByteArray saveState()/bool restoreState() round-trip
void tst_NetworkCookieJar::saveRestore()
{
    SubNetworkCookieJar jar;
    jar.setCookiesFromUrl(QList<QNetworkCookie>()
                          << makeCookie("a", "1", "example.com")
                          << makeCookie("b", "2", "foo.co.uk"),
                          QUrl("http://example.com/"));
    // The second cookie's domain does not match the url — set it via
    // setAllCookies so the jar holds two domains.
    jar.call_setAllCookies(QList<QNetworkCookie>()
                           << makeCookie("a", "1", "example.com")
                           << makeCookie("b", "2", "foo.co.uk"));

    SubNetworkCookieJar jar2;
    QVERIFY(jar2.call_restoreState(jar.call_saveState()));
    QList<QNetworkCookie> restored = jar2.call_allCookies();
    QCOMPARE(restored.count(), 2);
    QVERIFY(restored.contains(makeCookie("a", "1", "example.com")));
    QVERIFY(restored.contains(makeCookie("b", "2", "foo.co.uk")));
}

void tst_NetworkCookieJar::restoreStateInvalid()
{
    SubNetworkCookieJar jar;
    QCOMPARE(jar.call_restoreState(QByteArray()), false);
    QCOMPARE(jar.call_restoreState(QByteArray("junk")), false);
}

// protected void endSession()
void tst_NetworkCookieJar::endSession()
{
    SubNetworkCookieJar jar;
    QNetworkCookie session = makeCookie("s", "1", "example.com");
    QNetworkCookie persistent = makeCookie("p", "2", "example.com");
    persistent.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(1));
    QNetworkCookie expired = makeCookie("e", "3", "example.com");
    expired.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(-1));
    jar.call_setAllCookies(QList<QNetworkCookie>() << session << persistent << expired);

    jar.call_endSession();
    QList<QNetworkCookie> cookies = jar.call_allCookies();
    QCOMPARE(cookies.count(), 1);
    QCOMPARE(cookies.first().name(), QByteArray("p"));
}

// protected QList<QNetworkCookie> allCookies()/setAllCookies()
void tst_NetworkCookieJar::allCookies()
{
    SubNetworkCookieJar jar;
    QCOMPARE(jar.call_allCookies(), QList<QNetworkCookie>());

    QList<QNetworkCookie> list;
    list << makeCookie("a", "1", "example.com")
         << makeCookie("b", "2", ".foo.com");
    jar.call_setAllCookies(list);
    QCOMPARE(jar.call_allCookies().count(), 2);

    jar.call_setAllCookies(QList<QNetworkCookie>());
    QCOMPARE(jar.call_allCookies().count(), 0);
}

QTEST_MAIN(tst_NetworkCookieJar)
#include "tst_networkcookiejar.moc"
