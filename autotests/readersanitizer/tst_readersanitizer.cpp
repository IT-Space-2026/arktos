/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include <QtTest/QtTest>
#include "qtest_arktos.h"

#include <readersanitizer.h>

#include <qjsondocument.h>
#include <qjsonobject.h>

#ifdef ARKTOS_RUSTCORE
#include <rustcore.h>
#endif

// RC12/READ05: the reader-mode boundary — the rustcore readability
// extraction (rc_readability_probe / rc_readability_extract) and the
// ReaderSanitizer policy the overlay applies on top of it.  The Rust
// legs run the shipping path end to end over the shared fixture
// corpus (src/rustcore/testdata/reader, the same files cargo tests
// iterate): extract -> verdict -> C++ policy pass -> READ03 floor.
// Under a no-rust build the FFI legs skip; the sanitizer-policy legs
// still pin the C++ contract the JS fallback path relies on.
class tst_ReaderSanitizer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // C++ policy on a fragment shaped like what extraction emits —
    // identical on the rust and no-rust build paths.
    void sanitizerPolicy();
    void sanitizerMeasuresBody();

    void ffiCorpus_data();
    void ffiCorpus();
    void ffiDeterministic();
    void ffiBadInput();
};

void tst_ReaderSanitizer::initTestCase()
{
    QCoreApplication::setApplicationName("tst_readersanitizer");
    Q_INIT_RESOURCE(readersanitizer);
}

void tst_ReaderSanitizer::sanitizerPolicy()
{
    const QString fragment = QStringLiteral(
        "<div><p>Ordinary article copy that any reasonable filter "
        "keeps.</p>"
        "<p onclick=\"steal()\">handler paragraph</p>"
        "<a href=\"javascript:steal()\">bad link</a>"
        "<a href=\"https://example.com/more\">good link</a>"
        "<img src=\"https://example.com/i.png\">"
        "<img src=\"javascript:steal()\">"
        "<script>alert(1)</script>"
        "<iframe src=\"https://evil.example/\"></iframe>"
        "<object data=\"x.swf\"></object>"
        "<form><input name=\"c\">wrapped copy stays</form>"
        "<noscript><p>noscript copy stays</p></noscript>"
        "</div>");
    const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(fragment);
    QVERIFY2(!r.html.contains(QLatin1String("<script")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("onclick")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("javascript:")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("<iframe")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("<form")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("<input")),
             qPrintable(r.html));
    QVERIFY2(!r.html.contains(QLatin1String("<object")),
             qPrintable(r.html));
    QVERIFY2(r.html.contains(QLatin1String("Ordinary article copy")),
             qPrintable(r.html));
    QVERIFY2(r.html.contains(QLatin1String("good link")),
             qPrintable(r.html));
    QVERIFY2(r.html.contains(QLatin1String("https://example.com/more")),
             qPrintable(r.html));
    // Transparent wrappers: the tag dies, the copy survives.
    QVERIFY2(r.html.contains(QLatin1String("wrapped copy stays")),
             qPrintable(r.html));
    QVERIFY2(r.html.contains(QLatin1String("noscript copy stays")),
             qPrintable(r.html));
    // The surviving <img> keeps the media flag honest.
    QVERIFY(r.hasMedia);
    QVERIFY(r.textLength > 50);
}

void tst_ReaderSanitizer::sanitizerMeasuresBody()
{
    // READ03's floor inputs: a heading-only fragment measures thin and
    // carries no media — the caller refuses it instead of opening a
    // nearly empty overlay.
    const ReaderSanitizer::Result thin =
        ReaderSanitizer::sanitize(QStringLiteral("<h1>Title</h1>"));
    QVERIFY(thin.textLength < 500);
    QVERIFY(!thin.hasMedia);

    const ReaderSanitizer::Result mediaOnly =
        ReaderSanitizer::sanitize(QStringLiteral(
            "<figure><img src=\"https://example.com/chart.png\">"
            "<figcaption>a chart</figcaption></figure>"));
    QVERIFY(mediaOnly.hasMedia);
}

#ifdef ARKTOS_RUSTCORE
static QJsonObject extractVerdict(const QByteArray &html,
                                  const QByteArray &baseUrl)
{
    RcBuffer out{};
    const RcStatus status = rc_readability_extract(
        reinterpret_cast<const uint8_t *>(html.constData()),
        size_t(html.size()), baseUrl.constData(), &out);
    const QByteArray json(reinterpret_cast<const char *>(out.data),
                          qsizetype(out.len));
    if (out.data)
        rc_buffer_free(out);
    if (status != RC_OK)
        return QJsonObject();
    return QJsonDocument::fromJson(json).object();
}
#endif

void tst_ReaderSanitizer::ffiCorpus_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("wantOk");
    QTest::addColumn<QString>("marker");
    QTest::addColumn<QString>("lang");

    QTest::newRow("news-article")
        << QStringLiteral("news-article.html") << true
        << QStringLiteral("MARKER_NEWS_LEDE") << QStringLiteral("en");
    QTest::newRow("blog-post")
        << QStringLiteral("blog-post.html") << true
        << QStringLiteral("MARKER_BLOG_LEDE") << QStringLiteral("en");
    QTest::newRow("recipe-page")
        << QStringLiteral("recipe-page.html") << true
        << QStringLiteral("MARKER_RECIPE_LEDE") << QStringLiteral("en");
    QTest::newRow("docs-page")
        << QStringLiteral("docs-page.html") << true
        << QStringLiteral("MARKER_DOCS_LEDE") << QStringLiteral("en");
    QTest::newRow("spa-main")
        << QStringLiteral("spa-main.html") << true
        << QStringLiteral("MARKER_SPA_LEDE") << QStringLiteral("hu");
    QTest::newRow("search-results")
        << QStringLiteral("search-results.html") << false
        << QString() << QStringLiteral("en");
    QTest::newRow("hostile-article")
        << QStringLiteral("hostile-article.html") << true
        << QStringLiteral("MARKER_HOSTILE_LEDE") << QStringLiteral("en");
}

void tst_ReaderSanitizer::ffiCorpus()
{
#if defined(ARKTOS_RUSTCORE)
    QFETCH(QString, file);
    QFETCH(bool, wantOk);
    QFETCH(QString, marker);
    QFETCH(QString, lang);

    QFile f(QStringLiteral(":/reader/") + file);
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(file));
    const QByteArray html = f.readAll();
    const QByteArray baseUrl =
        QByteArrayLiteral("https://example.com/") + file.toUtf8();

    // The probe is the reader-affordance verdict — it must agree with
    // what extraction decides.
    QCOMPARE(rc_readability_probe(
                 reinterpret_cast<const uint8_t *>(html.constData()),
                 size_t(html.size())),
             wantOk ? 1 : 0);

    const QJsonObject v = extractVerdict(html, baseUrl);
    QVERIFY2(!v.isEmpty(), "extract returned an error status");
    QCOMPARE(v.value(QLatin1String("ok")).toBool(), wantOk);
    QCOMPARE(v.value(QLatin1String("probably")).toBool(), wantOk);
    QVERIFY(v.contains(QLatin1String("lang")));
    if (!wantOk)
        return;

    QCOMPARE(v.value(QLatin1String("lang")).toString(), lang);
    QVERIFY(v.value(QLatin1String("length")).toInt() > 400);
    QVERIFY2(!v.value(QLatin1String("title")).toString().isEmpty(),
             qPrintable(file));

    // Parity leg: the fragment that crossed the FFI boundary now runs
    // through the C++ ReaderSanitizer exactly as ReaderMode::enter()
    // does.  A clean extraction stays above the READ03 thin-content
    // floor and keeps the body marker — anything the Rust pre-pass
    // missed, the app-side policy still strips.
    const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(
        v.value(QLatin1String("content")).toString());
    QVERIFY2(r.textLength >= 500 || r.hasMedia,
             qPrintable(QStringLiteral(
                 "%1: thin after sanitize (chars %2 media %3)")
                 .arg(file).arg(r.textLength).arg(r.hasMedia)));
    QVERIFY2(r.html.contains(marker), qPrintable(file));
    for (const char *bad :
         {"<script", "javascript:", "<iframe", "<form", "<object"}) {
        QVERIFY2(!r.html.contains(QLatin1String(bad)),
                 qPrintable(QStringLiteral("%1: %2 survived")
                            .arg(file, QLatin1String(bad))));
    }
#else
    QSKIP("rustcore not compiled in — FFI verdicts unavailable");
#endif
}

void tst_ReaderSanitizer::ffiDeterministic()
{
#if defined(ARKTOS_RUSTCORE)
    QFile f(QStringLiteral(":/reader/news-article.html"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray html = f.readAll();
    const QByteArray baseUrl = QByteArrayLiteral("https://ex.com/p");

    RcBuffer a{}, b{};
    QCOMPARE(rc_readability_extract(
                 reinterpret_cast<const uint8_t *>(html.constData()),
                 size_t(html.size()), baseUrl.constData(), &a),
             RC_OK);
    QCOMPARE(rc_readability_extract(
                 reinterpret_cast<const uint8_t *>(html.constData()),
                 size_t(html.size()), baseUrl.constData(), &b),
             RC_OK);
    const QByteArray first(reinterpret_cast<const char *>(a.data),
                           qsizetype(a.len));
    const QByteArray second(reinterpret_cast<const char *>(b.data),
                            qsizetype(b.len));
    if (a.data)
        rc_buffer_free(a);
    if (b.data)
        rc_buffer_free(b);
    QCOMPARE(second, first);
#else
    QSKIP("rustcore not compiled in — FFI verdicts unavailable");
#endif
}

void tst_ReaderSanitizer::ffiBadInput()
{
#if defined(ARKTOS_RUSTCORE)
    const QByteArray baseUrl = QByteArrayLiteral("https://ex.com/");
    RcBuffer out{};
    QCOMPARE(rc_readability_extract(nullptr, 4, baseUrl.constData(),
                                    &out),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rc_readability_probe(nullptr, 4), 0);
    const QByteArray junk = QByteArrayLiteral("\xff\xfe\x00x");
    QCOMPARE(rc_readability_probe(
                 reinterpret_cast<const uint8_t *>(junk.constData()),
                 size_t(junk.size())),
             0);
#else
    QSKIP("rustcore not compiled in — FFI verdicts unavailable");
#endif
}

QTEST_MAIN(tst_ReaderSanitizer)
#include "tst_readersanitizer.moc"
