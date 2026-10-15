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

// ICONS01: AroraIcon — the bundled themes embedded under :/icons are
// resolved through real freedesktop-theme machinery.  The interesting
// failure modes are a missing glyph (invisible toolbar button), a
// theme switch that does not reach already-created QIcons, and the
// -dark recolor variants not being picked.

#include <QtTest/QtTest>
#include <QtGui/QtGui>

#include "aroraicon.h"
#include "arorastyle.h"
#include "browsertheme.h"

class tst_AroraIcon : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void themeIds();
    void namesList();
    void bundledCoverage_data();
    void bundledCoverage();
    void defaultCoverage();
    void liveSwitch();
    void darkVariant();
    void nativeDarkVariant();
    void legacyFallback();
    void iconForTheme();
    void chromeIcon();
};

// Opaque pixels whose color strays more than rounding noise from the
// palette tint — a getChrome glyph must be perfectly monochrome.
static int offTintPixels(const QImage &image, const QColor &tint)
{
    int off = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() > 128
                    && (qAbs(c.red() - tint.red()) > 2
                        || qAbs(c.green() - tint.green()) > 2
                        || qAbs(c.blue() - tint.blue()) > 2))
                ++off;
        }
    return off;
}

static int opaquePixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() > 40)
                ++count;
    return count;
}

void tst_AroraIcon::initTestCase()
{
    QCoreApplication::setApplicationName("tst_aroraicon");
    QSettings settings;
    settings.clear();
}

void tst_AroraIcon::cleanupTestCase()
{
    AroraIcon::setTheme(QLatin1String("native"));
}

void tst_AroraIcon::themeIds()
{
    const QStringList ids = AroraIcon::themeIds();
    QCOMPARE(ids.count(), 5);
    QCOMPARE(ids.first(), QLatin1String("native"));
    for (const QString &id : ids)
        QVERIFY(!AroraIcon::themeDisplayName(id).isEmpty());
}

// The names the chrome actually asks for must exist in the bundle —
// a missing name renders as a blank button.
void tst_AroraIcon::namesList()
{
    const QStringList names = AroraIcon::names();
    QVERIFY2(names.count() >= 30,
             qPrintable(QString::number(names.count())));
    const QStringList required = {
        QLatin1String("window-new"), QLatin1String("window-close"),
        QLatin1String("document-open"), QLatin1String("document-print"),
        QLatin1String("document-print-preview"),
        QLatin1String("document-save-as"),
        QLatin1String("document-revert"),
        QLatin1String("application-exit"),
        QLatin1String("edit-undo"), QLatin1String("edit-redo"),
        QLatin1String("edit-cut"), QLatin1String("edit-copy"),
        QLatin1String("edit-paste"), QLatin1String("edit-select-all"),
        QLatin1String("edit-find"), QLatin1String("edit-clear"),
        QLatin1String("edit-clear-locationbar-ltr"),
        QLatin1String("edit-clear-locationbar-rtl"),
        QLatin1String("go-previous"), QLatin1String("go-next"),
        QLatin1String("go-home"), QLatin1String("process-stop"),
        QLatin1String("view-refresh"), QLatin1String("view-fullscreen"),
        QLatin1String("zoom-in"), QLatin1String("zoom-out"),
        QLatin1String("zoom-original"), QLatin1String("bookmark-new"),
        QLatin1String("user-bookmarks"), QLatin1String("folder-new"),
        QLatin1String("emblem-downloads"),
        QLatin1String("preferences-desktop-locale"),
        QLatin1String("tab-new"), QLatin1String("list-add"),
        QLatin1String("system-run"),
    };
    for (const QString &name : required)
        QVERIFY2(names.contains(name), qPrintable(name));
}

void tst_AroraIcon::bundledCoverage_data()
{
    QTest::addColumn<QString>("id");
    QTest::newRow("adwaita") << QStringLiteral("adwaita");
    QTest::newRow("breeze") << QStringLiteral("breeze");
    QTest::newRow("tabler") << QStringLiteral("tabler");
    QTest::newRow("lucide") << QStringLiteral("lucide");
}

void tst_AroraIcon::bundledCoverage()
{
    QFETCH(QString, id);
    AroraIcon::setTheme(id);
    for (const QString &name : AroraIcon::names()) {
        const QIcon icon = AroraIcon::get(name);
        const QImage image = icon.pixmap(QSize(24, 24)).toImage();
        QVERIFY2(!image.isNull(), qPrintable(id + '/' + name));
        QVERIFY2(opaquePixels(image) > 0, qPrintable(id + '/' + name));
    }
}

// ICONS10: the default pick is the bundled Tabler set, and every
// chrome name the code can ask for ships inside it — a name absent
// from the set's own directory falls through the Inherits chain to
// arora-legacy's 2009 art, the mixed-style regression the audit
// exists to prevent.  The -dark variant must cover the same surface.
void tst_AroraIcon::defaultCoverage()
{
    QCOMPARE(AroraIcon::theme(), QLatin1String("tabler"));
    const QDir light(QLatin1String(":/icons/arora-tabler/icons"));
    const QDir dark(QLatin1String(":/icons/arora-tabler-dark/icons"));
    for (const QString &name : AroraIcon::names()) {
        const QString file = name + QLatin1String(".svg");
        QVERIFY2(light.exists(file), qPrintable(name));
        QVERIFY2(dark.exists(file), qPrintable(name));
    }
}

// The same QIcon must pick up a theme switch — that is what makes
// the Settings combo apply without touching each widget.
void tst_AroraIcon::liveSwitch()
{
    AroraIcon::setTheme(QLatin1String("adwaita"));
    const QIcon icon = AroraIcon::get(QLatin1String("go-previous"));
    const QImage before = icon.pixmap(QSize(24, 24)).toImage();
    AroraIcon::setTheme(QLatin1String("tabler"));
    const QImage after = icon.pixmap(QSize(24, 24)).toImage();
    QVERIFY(before != after);
}

// A dark application palette selects the -dark recolor variant —
// Qt does not recolor currentColor against the palette, so without
// this the monochrome sets would be near-invisible on dark chrome.
void tst_AroraIcon::darkVariant()
{
    const QPalette saved = qApp->palette();

    qApp->setPalette(BrowserTheme::darkPalette());
    QCOMPARE(AroraIcon::effectiveThemeName(QLatin1String("tabler")),
             QLatin1String("arora-tabler-dark"));
    QCOMPARE(AroraIcon::effectiveThemeName(QLatin1String("adwaita")),
             QLatin1String("arora-adwaita-dark"));
    // ICONS11: lucide ships the same -dark recolor variant.
    QCOMPARE(AroraIcon::effectiveThemeName(QLatin1String("lucide")),
             QLatin1String("arora-lucide-dark"));

    AroraIcon::setTheme(QLatin1String("tabler"));
    const QImage darkImage = AroraIcon::get(QLatin1String("go-previous"))
        .pixmap(QSize(24, 24)).toImage();
    // The dark variant's stroke is light: the average opaque pixel
    // should sit well above a mid-gray luminance.
    qlonglong luminance = 0;
    int opaque = 0;
    for (int y = 0; y < darkImage.height(); ++y) {
        for (int x = 0; x < darkImage.width(); ++x) {
            const QColor c = darkImage.pixelColor(x, y);
            if (c.alpha() > 128) {
                luminance += qGray(c.rgb());
                ++opaque;
            }
        }
    }
    QVERIFY(opaque > 0);
    QVERIFY(luminance / opaque > 140);

    qApp->setPalette(saved);
    QCOMPARE(AroraIcon::effectiveThemeName(QLatin1String("tabler")),
             QLatin1String("arora-tabler"));
}

// ICONS05: "native" under a dark palette must not render dark
// glyphs.  A forced dark palette is invisible to the OS icon theme,
// so unless the system theme provably serves light-on-dark art the
// effective theme falls back to a bundled -dark set.  Assert the
// rendered outcome, not the mapping — desktops whose native theme
// really is dark-aware keep their theme and still pass.
void tst_AroraIcon::nativeDarkVariant()
{
    const QPalette saved = qApp->palette();
    qApp->setPalette(BrowserTheme::darkPalette());

    AroraIcon::setTheme(QLatin1String("native"));
    const QString effective = AroraIcon::effectiveThemeName(
        QLatin1String("native"));
    QVERIFY2(!effective.isEmpty(), "native must resolve to a theme");

    const QImage image = AroraIcon::get(QLatin1String("go-previous"))
        .pixmap(QSize(24, 24)).toImage();
    qlonglong luminance = 0;
    int opaque = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() > 128) {
                luminance += qGray(c.rgb());
                ++opaque;
            }
        }
    }
    QVERIFY2(opaque > 0, qPrintable(effective));
    QVERIFY2(luminance / opaque > 140, qPrintable(
        effective + QLatin1String(" mean luminance ")
        + QString::number(luminance / opaque)));

    // Back on a light palette native never resolves to a bundled
    // -dark set — the substitution is palette-driven, not sticky.
    qApp->setPalette(saved);
    const QString light = AroraIcon::effectiveThemeName(
        QLatin1String("native"));
    QVERIFY2(!(light.startsWith(QLatin1String("arora-"))
               && light.endsWith(QLatin1String("-dark"))),
             qPrintable(light));
    AroraIcon::setTheme(QLatin1String("native"));
}

// With no resolvable theme at all the chain must still reach the
// original 2009 artwork (arora-legacy) — and names missing there
// must keep falling through to the bundled Adwaita inheritance.
void tst_AroraIcon::legacyFallback()
{
    QIcon::setThemeName(QLatin1String("arora-does-not-exist"));
    QVERIFY(!AroraIcon::get(QLatin1String("tab-new")).pixmap(QSize(16, 16))
                 .isNull());
    QVERIFY(!AroraIcon::get(QLatin1String("window-close")).pixmap(QSize(16, 16))
                 .isNull());
    // Only in the bundled sets — proves the fallback theme's
    // Inherits=arora-adwaita is walked.
    QVERIFY(!AroraIcon::get(QLatin1String("go-previous")).pixmap(QSize(16, 16))
                 .isNull());
}

// Per-option preview icons for the Settings combo: each bundled set
// resolves the same name to its own artwork.
void tst_AroraIcon::iconForTheme()
{
    QHash<QString, QImage> previews;
    const QStringList sets = { QLatin1String("adwaita"),
                               QLatin1String("breeze"),
                               QLatin1String("tabler"),
                               QLatin1String("lucide") };
    for (const QString &id : sets) {
        const QIcon icon = AroraIcon::iconForTheme(
            id, QLatin1String("view-refresh"));
        QVERIFY2(!icon.isNull(), qPrintable(id));
        previews[id] = icon.pixmap(QSize(24, 24)).toImage();
        QVERIFY2(opaquePixels(previews[id]) > 0, qPrintable(id));
    }
    QVERIFY(previews.value(QLatin1String("adwaita"))
            != previews.value(QLatin1String("tabler")));
    // ICONS11: view-refresh is a maintained glyph — tabler and lucide
    // draw it at identical geometry/color, so the distinctness probe
    // uses a name where upstream art differs (zoom-original:
    // zoom-reset vs scan-search).
    const QImage lucideGlyph = AroraIcon::iconForTheme(
        QLatin1String("lucide"), QLatin1String("zoom-original"))
        .pixmap(QSize(24, 24)).toImage();
    const QImage tablerGlyph = AroraIcon::iconForTheme(
        QLatin1String("tabler"), QLatin1String("zoom-original"))
        .pixmap(QSize(24, 24)).toImage();
    QVERIFY(lucideGlyph != tablerGlyph);
}

// ICONS09: the chrome contract — getChrome flattens the themed art
// to the palette's ButtonText (whichever set serves the shape),
// Disabled takes the palette's own disabled tone, and the tint is
// resolved per pixmap() request so a live palette flip restyles an
// already-constructed icon while a theme swap still moves the shape.
void tst_AroraIcon::chromeIcon()
{
    const QPalette saved = qApp->palette();
    const QColor tint = saved.color(QPalette::Normal,
                                    QPalette::ButtonText);
    const QColor disabledTint = saved.color(QPalette::Disabled,
                                            QPalette::ButtonText);

    const QIcon icon =
        AroraIcon::getChrome(QLatin1String("go-previous"));

    // A palette flip only posts ApplicationPaletteChange — the
    // PaletteWatcher's setTheme (and any -dark art swap it picks)
    // happens when the event is delivered, so settle the queue
    // before each measurement or the base shape is nondeterministic.
    const auto settleTheme = []() {
        QCoreApplication::sendPostedEvents(
            qApp, QEvent::ApplicationPaletteChange);
    };
    settleTheme();

    const QImage image = icon.pixmap(QSize(24, 24)).toImage();
    QVERIFY(opaquePixels(image) > 0);
    QCOMPARE(offTintPixels(image, tint), 0);

    const QImage disabled =
        icon.pixmap(QSize(24, 24), QIcon::Disabled).toImage();
    QVERIFY(opaquePixels(disabled) > 0);
    QCOMPARE(offTintPixels(disabled, disabledTint), 0);
    QVERIFY(disabled != image);

    // THEM03/THEM04: a selected request tints to the Highlight
    // card's foreground — selectionTextColor, the same pick the
    // sheets pair with the card's text.
    const QImage selected =
        icon.pixmap(QSize(24, 24), QIcon::Selected).toImage();
    QCOMPARE(offTintPixels(selected,
                           AroraStyle::selectionTextColor(saved)),
             0);

    // The same QIcon re-tints across a palette flip — nothing
    // re-set it; that is the whole point of the lazy engine.
    qApp->setPalette(BrowserTheme::darkPalette());
    settleTheme();
    const QImage dark = icon.pixmap(QSize(24, 24)).toImage();
    QCOMPARE(offTintPixels(dark, BrowserTheme::darkPalette()
                           .color(QPalette::ButtonText)),
             0);
    QVERIFY(dark != image);
    qApp->setPalette(saved);
    settleTheme();
    QCOMPARE(icon.pixmap(QSize(24, 24)).toImage(), image);

    // Theme swap still changes the shape under the fixed tint.
    AroraIcon::setTheme(QLatin1String("adwaita"));
    const QImage adwaita = icon.pixmap(QSize(24, 24)).toImage();
    QCOMPARE(offTintPixels(adwaita, tint), 0);
    AroraIcon::setTheme(QLatin1String("tabler"));
    const QImage tabler = icon.pixmap(QSize(24, 24)).toImage();
    QCOMPARE(offTintPixels(tabler, tint), 0);
    QVERIFY(adwaita != tabler);

    AroraIcon::setTheme(QLatin1String("native"));
}

QTEST_MAIN(tst_AroraIcon)
#include "tst_aroraicon.moc"
