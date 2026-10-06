/*
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

// TST01: rewritten for the Qt WebEngine CookieJar.  The jar now wraps
// QWebEngineCookieStore; writes to the store are asynchronous, so
// assertions about the cookie mirror use QTRY_VERIFY.

#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>
#include <QNetworkCookie>
#include <cookiejar.h>

class tst_CookieJar : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void cookiejar();

    void acceptPolicy_data();
    void acceptPolicy();
    void allowedCookies_data();
    void allowedCookies();
    void allowForSessionCookies_data();
    void allowForSessionCookies();
    void blockedCookies_data();
    void blockedCookies();
    void clear();
    void cookiesForUrl();
    void keepPolicy_data();
    void keepPolicy();
    void loadSettings();
    void setAcceptPolicy_data();
    void setAcceptPolicy();
    void setAllowedCookies_data();
    void setAllowedCookies();
    void setAllowForSessionCookies_data();
    void setAllowForSessionCookies();
    void setBlockedCookies_data();
    void setBlockedCookies();
    void setCookiesFromUrl_data();
    void setCookiesFromUrl();
    void setKeepPolicy_data();
    void setKeepPolicy();
    void cookiesChanged();
    void isOnDomainList_data();
    void isOnDomainList();
};

// Subclass that exposes the protected functions.
class SubCookieJar : public CookieJar
{
public:
    void call_cookiesChanged()
        { return SubCookieJar::cookiesChanged(); }

    static bool call_isOnDomainList(QStringList const &list, QString const &domain)
        { return SubCookieJar::isOnDomainList(list, domain); }
};

static QNetworkCookie makeCookie(const QString &domain, const QByteArray &name = "a",
                                 const QByteArray &value = "b")
{
    QNetworkCookie cookie(name, value);
    cookie.setDomain(domain);
    return cookie;
}

// This will be called before the first test function is executed.
// It is only called once.
void tst_CookieJar::initTestCase()
{
    QCoreApplication::setApplicationName("tst_cookiejar");
    QSettings settings;
    settings.clear();
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_CookieJar::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_CookieJar::init()
{
    // Prior tests may have persisted policy/exception settings via the
    // AutoSaver in ~CookieJar; start each test from a clean slate.
    QSettings settings;
    settings.clear();
}

// This will be called after every test function.
void tst_CookieJar::cleanup()
{
}

void tst_CookieJar::cookiejar()
{
    SubCookieJar jar;
    QCOMPARE(jar.acceptPolicy(), CookieJar::AcceptOnlyFromSitesNavigatedTo);
    QCOMPARE(jar.allowedCookies(), QStringList());
    QCOMPARE(jar.allowForSessionCookies(), QStringList());
    QCOMPARE(jar.blockedCookies(), QStringList());
    QCOMPARE(jar.keepPolicy(), CookieJar::KeepUntilExpire);
    jar.loadSettings();
    QCOMPARE(jar.setCookiesFromUrl(QList<QNetworkCookie>(), QUrl()), false);
    QCOMPARE(jar.call_isOnDomainList(QStringList(), QString()), false);
}

Q_DECLARE_METATYPE(CookieJar::AcceptPolicy)
void tst_CookieJar::acceptPolicy_data()
{
    QTest::addColumn<CookieJar::AcceptPolicy>("acceptPolicy");
    QTest::newRow("default") << CookieJar::AcceptOnlyFromSitesNavigatedTo;
}

// public CookieJar::AcceptPolicy acceptPolicy() const
void tst_CookieJar::acceptPolicy()
{
    QFETCH(CookieJar::AcceptPolicy, acceptPolicy);

    SubCookieJar jar;
    QCOMPARE(jar.acceptPolicy(), acceptPolicy);
}

void tst_CookieJar::allowedCookies_data()
{
    QTest::addColumn<QStringList>("allowedCookies");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public QStringList allowedCookies() const
void tst_CookieJar::allowedCookies()
{
    QFETCH(QStringList, allowedCookies);

    SubCookieJar jar;
    jar.setAllowedCookies(allowedCookies);
    QCOMPARE(jar.allowedCookies(), allowedCookies);
}

void tst_CookieJar::allowForSessionCookies_data()
{
    QTest::addColumn<QStringList>("allowForSessionCookies");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public QStringList allowForSessionCookies() const
void tst_CookieJar::allowForSessionCookies()
{
    QFETCH(QStringList, allowForSessionCookies);

    SubCookieJar jar;
    jar.setAllowForSessionCookies(allowForSessionCookies);
    QCOMPARE(jar.allowForSessionCookies(), allowForSessionCookies);
}

void tst_CookieJar::blockedCookies_data()
{
    QTest::addColumn<QStringList>("blockedCookies");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public QStringList blockedCookies() const
void tst_CookieJar::blockedCookies()
{
    QFETCH(QStringList, blockedCookies);

    SubCookieJar jar;
    jar.setBlockedCookies(blockedCookies);
    QCOMPARE(jar.blockedCookies(), blockedCookies);
}

// public void clear()
void tst_CookieJar::clear()
{
    SubCookieJar jar;
    jar.setCookies(QList<QNetworkCookie>() << makeCookie("foo.com"));
    QCOMPARE(jar.cookies().count(), 1);

    QSignalSpy spy(&jar, SIGNAL(cookiesChanged()));
    jar.clear();
    QCOMPARE(jar.cookies().count(), 0);
    QCOMPARE(spy.count(), 1);
}

// public QList<QNetworkCookie> cookiesForUrl(QUrl const &url) const
void tst_CookieJar::cookiesForUrl()
{
    SubCookieJar jar;
    QCOMPARE(jar.cookiesForUrl(QUrl()), QList<QNetworkCookie>());

    QNetworkCookie foo = makeCookie("foo.com");
    QNetworkCookie bar = makeCookie("bar.com", "c", "d");
    jar.setCookies(QList<QNetworkCookie>() << foo << bar);

    QCOMPARE(jar.cookiesForUrl(QUrl("http://foo.com/")),
             QList<QNetworkCookie>() << foo);
    QCOMPARE(jar.cookiesForUrl(QUrl("http://bar.com/")),
             QList<QNetworkCookie>() << bar);
    QCOMPARE(jar.cookiesForUrl(QUrl("http://baz.com/")),
             QList<QNetworkCookie>());
}

Q_DECLARE_METATYPE(CookieJar::KeepPolicy)
void tst_CookieJar::keepPolicy_data()
{
    QTest::addColumn<CookieJar::KeepPolicy>("keepPolicy");
    QTest::newRow("default") << CookieJar::KeepUntilExpire;
}

// public CookieJar::KeepPolicy keepPolicy() const
void tst_CookieJar::keepPolicy()
{
    QFETCH(CookieJar::KeepPolicy, keepPolicy);

    SubCookieJar jar;
    QCOMPARE(jar.keepPolicy(), keepPolicy);
}

// public void loadSettings()
void tst_CookieJar::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("cookies"));
    settings.setValue(QLatin1String("acceptCookies"), QLatin1String("AcceptAlways"));
    settings.setValue(QLatin1String("keepCookiesUntil"), QLatin1String("KeepUntilExit"));
    settings.endGroup();

    SubCookieJar jar;
    QCOMPARE(jar.acceptPolicy(), CookieJar::AcceptAlways);
    QCOMPARE(jar.keepPolicy(), CookieJar::KeepUntilExit);

    settings.beginGroup(QLatin1String("cookies"));
    settings.remove(QLatin1String("acceptCookies"));
    settings.remove(QLatin1String("keepCookiesUntil"));
    settings.endGroup();
}

void tst_CookieJar::setAcceptPolicy_data()
{
    QTest::addColumn<CookieJar::AcceptPolicy>("policy");
    QTest::newRow("always") << CookieJar::AcceptAlways;
    QTest::newRow("never") << CookieJar::AcceptNever;
    QTest::newRow("navigated") << CookieJar::AcceptOnlyFromSitesNavigatedTo;
}

// public void setAcceptPolicy(CookieJar::AcceptPolicy policy)
void tst_CookieJar::setAcceptPolicy()
{
    QFETCH(CookieJar::AcceptPolicy, policy);

    SubCookieJar jar;
    jar.setAcceptPolicy(policy);
    QCOMPARE(jar.acceptPolicy(), policy);
}

void tst_CookieJar::setAllowedCookies_data()
{
    QTest::addColumn<QStringList>("list");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public void setAllowedCookies(QStringList const &list)
void tst_CookieJar::setAllowedCookies()
{
    QFETCH(QStringList, list);

    SubCookieJar jar;
    jar.setAllowedCookies(list);
    QCOMPARE(jar.allowedCookies(), list);
}

void tst_CookieJar::setAllowForSessionCookies_data()
{
    QTest::addColumn<QStringList>("list");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public void setAllowForSessionCookies(QStringList const &list)
void tst_CookieJar::setAllowForSessionCookies()
{
    QFETCH(QStringList, list);

    SubCookieJar jar;
    jar.setAllowForSessionCookies(list);
    QCOMPARE(jar.allowForSessionCookies(), list);
}

void tst_CookieJar::setBlockedCookies_data()
{
    QTest::addColumn<QStringList>("list");
    QTest::newRow("null") << QStringList();
    QTest::newRow("one") << (QStringList() << "foo.com");
}

// public void setBlockedCookies(QStringList const &list)
void tst_CookieJar::setBlockedCookies()
{
    QFETCH(QStringList, list);

    SubCookieJar jar;
    jar.setBlockedCookies(list);
    QCOMPARE(jar.blockedCookies(), list);
}

void tst_CookieJar::setCookiesFromUrl_data()
{
    QTest::addColumn<QList<QNetworkCookie>>("cookieList");
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<CookieJar::AcceptPolicy>("policy");
    QTest::addColumn<QStringList>("blocked");
    QTest::addColumn<QStringList>("allowed");
    QTest::addColumn<bool>("result");

    QList<QNetworkCookie> oneCookie = QList<QNetworkCookie>() << makeCookie("foo.com");

    QTest::newRow("null") << QList<QNetworkCookie>() << QUrl()
                          << CookieJar::AcceptAlways << QStringList() << QStringList() << false;
    QTest::newRow("always") << oneCookie << QUrl("http://foo.com/")
                            << CookieJar::AcceptAlways << QStringList() << QStringList() << true;
    QTest::newRow("never") << oneCookie << QUrl("http://foo.com/")
                           << CookieJar::AcceptNever << QStringList() << QStringList() << false;
    QTest::newRow("never-but-allowed") << oneCookie << QUrl("http://foo.com/")
                           << CookieJar::AcceptNever << QStringList() << (QStringList() << "foo.com") << true;
    QTest::newRow("blocked") << oneCookie << QUrl("http://foo.com/")
                             << CookieJar::AcceptAlways << (QStringList() << "foo.com") << QStringList() << false;
    QTest::newRow("blocked-wins-over-allow")
            << oneCookie << QUrl("http://foo.com/")
            << CookieJar::AcceptAlways << (QStringList() << "foo.com") << (QStringList() << "foo.com") << false;
}

// public bool setCookiesFromUrl(QList<QNetworkCookie> const &cookieList, QUrl const &url)
void tst_CookieJar::setCookiesFromUrl()
{
    QFETCH(QList<QNetworkCookie>, cookieList);
    QFETCH(QUrl, url);
    QFETCH(CookieJar::AcceptPolicy, policy);
    QFETCH(QStringList, blocked);
    QFETCH(QStringList, allowed);
    QFETCH(bool, result);

    SubCookieJar jar;
    jar.setAcceptPolicy(policy);
    jar.setBlockedCookies(blocked);
    jar.setAllowedCookies(allowed);

    QCOMPARE(jar.setCookiesFromUrl(cookieList, url), result);
}

void tst_CookieJar::setKeepPolicy_data()
{
    QTest::addColumn<CookieJar::KeepPolicy>("policy");
    QTest::newRow("expire") << CookieJar::KeepUntilExpire;
    QTest::newRow("exit") << CookieJar::KeepUntilExit;
    QTest::newRow("timelimit") << CookieJar::KeepUntilTimeLimit;
}

// public void setKeepPolicy(CookieJar::KeepPolicy policy)
void tst_CookieJar::setKeepPolicy()
{
    QFETCH(CookieJar::KeepPolicy, policy);

    SubCookieJar jar;
    jar.setKeepPolicy(policy);
    QCOMPARE(jar.keepPolicy(), policy);
}

// protected void cookiesChanged()
void tst_CookieJar::cookiesChanged()
{
    SubCookieJar jar;

    QSignalSpy spy(&jar, SIGNAL(cookiesChanged()));
    jar.call_cookiesChanged();
    QCOMPARE(spy.count(), 1);

    // setCookies() touches the mirror synchronously and notifies.
    jar.setCookies(QList<QNetworkCookie>() << makeCookie("foo.com"));
    QCOMPARE(spy.count(), 2);
}

void tst_CookieJar::isOnDomainList_data()
{
    QTest::addColumn<QStringList>("list");
    QTest::addColumn<QString>("domain");
    QTest::addColumn<bool>("isOnDomainList");

    QTest::newRow("null") << QStringList() << QString() << false;
    QTest::newRow("exact-match") << (QStringList() << "foo.com") << "foo.com" << true;

    QTest::newRow("check-0") << (QStringList() << "foo.com") << "foo.com" << true;
    QTest::newRow("check-1") << (QStringList() << "foo.com") << ".foo.com" << true;
    QTest::newRow("check-2") << (QStringList() << ".foo.com") << "foo.com" << true;
    QTest::newRow("check-3") << (QStringList() << ".foo.com") << ".foo.com" << true;
    QTest::newRow("check-4") << (QStringList() << "foo.com") << "abcfoo.com" << false;
    QTest::newRow("check-5") << (QStringList() << "foo.com") << "abc.foo.com" << true;
    QTest::newRow("check-6") << (QStringList() << ".foo.com") << "abcfoo.com" << false;
    QTest::newRow("check-7") << (QStringList() << ".foo.com") << "abc.foo.com" << true;

    QTest::newRow("check-8") << (QStringList() << "abc.foo.com") << "foo.com" << false;
    QTest::newRow("check-9") << (QStringList() << "abc.foo.com") << ".foo.com" << false;


    QTest::newRow("edgecheck-0") << (QStringList() << "") << ".foo.com" << false;
    QTest::newRow("edgecheck-1") << (QStringList() << "") << "foo.com" << false;
    QTest::newRow("edgecheck-2") << (QStringList() << ".") << ".foo.com" << false;
    QTest::newRow("edgecheck-3") << (QStringList() << ".") << "foo.com" << false;
    QTest::newRow("edgecheck-4") << (QStringList() << "abc.foo.com") << "" << false;
    QTest::newRow("edgecheck-5") << (QStringList() << "a") << "ab" << false;
}

// protected static bool isOnDomainList(QStringList const &list, QString const &domain)
void tst_CookieJar::isOnDomainList()
{
    QFETCH(QStringList, list);
    QFETCH(QString, domain);
    QFETCH(bool, isOnDomainList);

    QCOMPARE(SubCookieJar::call_isOnDomainList(list, domain), isOnDomainList);
}

QTEST_MAIN(tst_CookieJar)
#include "tst_cookiejar.moc"
