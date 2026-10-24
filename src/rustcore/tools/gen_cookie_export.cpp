// Generates testdata/cookies/export.txt — a real
// QNetworkCookie::toRawForm() capture so the RC15 import path is
// tested against Qt's exact serialization, not a clean-room guess.
//
// Build+run (Qt 6):
//   g++ -fPIC gen_cookie_export.cpp -o gen \
//       $(pkg-config --cflags --libs Qt6Network) && ./gen > export.txt
//
// Rows chosen to cover: a domain cookie with secure+HttpOnly+Lax and
// a Netscape-date expiry; a session cookie on a no-dot (host-exact)
// domain; Strict and None+secure; an already-expired eviction row;
// a host cookie with no Domain attribute (imports are expected to
// skip it — there is no request host to key it on); a UTF-8 value.

#include <QDateTime>
#include <QNetworkCookie>
#include <stdio.h>

int main()
{
    // 1. persistent domain cookie, secure+httponly, SameSite=Lax
    QNetworkCookie a("sid", "a1b2c3");
    a.setDomain(".example.com");
    a.setPath("/");
    a.setSecure(true);
    a.setHttpOnly(true);
    a.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
    a.setExpirationDate(QDateTime::fromSecsSinceEpoch(1893456000)); // 2030-01-01
    // 2. session cookie, no-dot domain (Qt treats it as host-exact)
    QNetworkCookie b("pref", "dark%20mode");
    b.setDomain("sub.example.com");
    b.setPath("/app");
    // 3. persistent, SameSite=Strict
    QNetworkCookie c("tok", "xyz");
    c.setDomain(".strict.example");
    c.setPath("/deep/path");
    c.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
    c.setExpirationDate(QDateTime::fromSecsSinceEpoch(1956528000)); // 2032-01-01
    // 4. SameSite=None + secure
    QNetworkCookie d("track", "t1");
    d.setDomain(".none.example");
    d.setPath("/");
    d.setSecure(true);
    d.setSameSitePolicy(QNetworkCookie::SameSite::None);
    d.setExpirationDate(QDateTime::fromSecsSinceEpoch(1893456000));
    // 5. expired cookie (eviction marker)
    QNetworkCookie e("old", "gone");
    e.setDomain(".example.com");
    e.setPath("/");
    e.setExpirationDate(QDateTime::fromSecsSinceEpoch(1)); // 1970
    // 6. no Domain attribute at all — a host cookie in Qt's model
    QNetworkCookie g("hostcook", "hv");
    g.setPath("/x");
    // 7. UTF-8 value
    QNetworkCookie h("uname", QString::fromUtf8("täst").toUtf8());
    h.setDomain(".example.com");
    h.setPath("/");
    for (const QNetworkCookie &ck : {a, b, c, d, e, g, h}) {
        fwrite(ck.toRawForm().constData(), 1,
               ck.toRawForm().size(), stdout);
        fputc('\n', stdout);
    }
    return 0;
}
