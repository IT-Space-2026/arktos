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

// THEME01: the central chrome style source — spacing scale,
// palette-derived separator colors, the flat-chrome app stylesheet
// and the metrics proxy (frameless toolbars, semibold group-box
// titles), plus the tor stylesheet builders the violet window quotes.

#include "qtest_arora.h"

#include <arorastyle.h>
#include <browsermainwindow.h>
#include <browsertheme.h>

#include <qdir.h>
#include <qgroupbox.h>
#include <qlabel.h>
#include <qsettings.h>
#include <qstatusbar.h>
#include <qstyle.h>
#include <qstylefactory.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qboxlayout.h>

class tst_Arorastyle : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void spacingScale();
    void paletteDerivedColors();
    void chromeSheetResolvesPalette();
    void flatChromeInstalled();
    void groupBoxTitleWeight();
    void polishSurvivesSchemeFlip();
    void torSheetsShareOneSource();
    void chromeGrab();
};

void tst_Arorastyle::initTestCase()
{
    // Pin the scheme inputs so the flip leg restores deterministically
    // — a stray user pick would keep the palette dark.
    qputenv("ARORA_COLOR_SCHEME", "light");
    QSettings().remove(QLatin1String("browser/colorScheme"));
    BrowserTheme::applyColorScheme();
}

void tst_Arorastyle::cleanupTestCase()
{
    qunsetenv("ARORA_COLOR_SCHEME");
    BrowserTheme::applyColorScheme();
}

void tst_Arorastyle::spacingScale()
{
    QCOMPARE(AroraStyle::Spacing::Small, 4);
    QCOMPARE(AroraStyle::Spacing::Medium, 8);
    QCOMPARE(AroraStyle::Spacing::Large, 12);
}

void tst_Arorastyle::paletteDerivedColors()
{
    // Blend endpoints and the mix itself.
    QCOMPARE(AroraStyle::blend(Qt::black, Qt::white, 0.0),
             QColor(Qt::black));
    QCOMPARE(AroraStyle::blend(Qt::black, Qt::white, 1.0),
             QColor(Qt::white));

    // The separator must differ from the surface on both scheme ends
    // — light text on dark Window lifts it, dark text on light Window
    // sinks it — and stay subtle (nowhere near the text color).
    const QColor lightSep =
        AroraStyle::separatorColor(QApplication::palette());
    QVERIFY(lightSep.isValid());
    QVERIFY(lightSep
            != QApplication::palette().color(QPalette::Window));
    QVERIFY(qAbs(lightSep.lightness()
                 - QApplication::palette()
                       .color(QPalette::Window).lightness()) < 60);

    const QPalette dark = BrowserTheme::darkPalette();
    const QColor darkSep = AroraStyle::separatorColor(dark);
    QVERIFY(darkSep.isValid());
    QVERIFY(darkSep != dark.color(QPalette::Window));
    QVERIFY(darkSep.lightness()
            > dark.color(QPalette::Window).lightness());
    QVERIFY(lightSep != darkSep);
}

void tst_Arorastyle::chromeSheetResolvesPalette()
{
    const QString sheet = AroraStyle::chromeStyleSheet();
    QVERIFY(sheet.contains(QLatin1String("QToolBar")));
    QVERIFY(sheet.contains(QLatin1String("QStatusBar")));
    QVERIFY(sheet.contains(QLatin1String("border: none")));
    QVERIFY(sheet.contains(AroraStyle::separatorColor(
        QApplication::palette()).name()));
}

void tst_Arorastyle::flatChromeInstalled()
{
    // BrowserApplication's constructor ran install() — the app style
    // is the polish proxy and the chrome sheet is live.  The frame
    // metric carries the Small padding so the bar keeps its breathing
    // room while drawing nothing.
    QToolBar bar;
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QToolBar { border: none; padding: 4px; }")));
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QStatusBar::item { border: none; }")));
    // baseStyleName() unwraps the proxy — a creatable factory name.
    const QString baseName = AroraStyle::baseStyleName();
    QVERIFY(!baseName.isEmpty());
    QStyle *base = QStyleFactory::create(baseName);
    QVERIFY(base);
    delete base;
}

void tst_Arorastyle::groupBoxTitleWeight()
{
    // The proxy draws QGroupBox titles at DemiBold; a twin box on the
    // bare base style must render its title differently — count ink
    // in the title band (heavier glyphs cover more pixels).
    QWidget host;
    host.resize(320, 80);
    QHBoxLayout *layout = new QHBoxLayout(&host);
    QGroupBox *polished = new QGroupBox(QLatin1String("Section"), &host);
    QGroupBox *plain = new QGroupBox(QLatin1String("Section"), &host);
    QStyle *fusion = QStyleFactory::create(QLatin1String("fusion"));
    QVERIFY(fusion);
    plain->setStyle(fusion);
    layout->addWidget(polished);
    layout->addWidget(plain);
    host.show();
    QApplication::processEvents();

    const auto titleInk = [](QGroupBox *box) {
        const QImage image = box->grab().toImage();
        qlonglong ink = 0;
        const int band = qMin(box->fontMetrics().height() + 4,
                              image.height());
        for (int y = 0; y < band; ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (qAbs(c.lightness()
                         - box->palette().color(
                               QPalette::Window).lightness()) > 60)
                    ++ink;
            }
        return ink;
    };
    const qlonglong heavy = titleInk(polished);
    const qlonglong normal = titleInk(plain);
    QVERIFY(heavy > 0);
    QVERIFY(heavy != normal);

    // The weight is applied at paint time only — the widget's own
    // font (and its children's) stays untouched.
    QVERIFY(polished->font().weight() == plain->font().weight());

    host.hide();
    delete fusion;
}

void tst_Arorastyle::polishSurvivesSchemeFlip()
{
    // A forced dark flip can replace the app style underneath the
    // proxy; the chromeSchemeChanged hook must re-wrap and re-resolve
    // the palette-derived sheet colors.
    const QString before = qApp->styleSheet();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();

    QToolBar bar;
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        AroraStyle::separatorColor(QApplication::palette()).name()));
    if (BrowserTheme::isDarkPalette(QApplication::palette()))
        QVERIFY(qApp->styleSheet() != before);

    BrowserTheme::applyColorScheme();
    QApplication::processEvents();
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QToolBar { border: none; padding: 4px; }")));
}

void tst_Arorastyle::torSheetsShareOneSource()
{
    const QString chrome = AroraStyle::torChromeColor().name();
    QCOMPARE(chrome, QLatin1String("#4a3372"));
    const QStringList sheets = {
        AroraStyle::torBarStyleSheet(),
        AroraStyle::torMenuBarStyleSheet(),
        AroraStyle::torStatusBarStyleSheet(),
        AroraStyle::torTabBarStyleSheet(),
        AroraStyle::torTabWidgetStyleSheet(),
        AroraStyle::torWindowStyleSheet(),
    };
    for (const QString &sheet : sheets)
        QVERIFY2(sheet.contains(chrome), qPrintable(sheet));

    // The selected tor tab borrows the field colors from the live
    // palette so it tracks the location bar in both schemes.
    QVERIFY(AroraStyle::torTabBarStyleSheet().contains(
        QApplication::palette().color(QPalette::Base).name()));
    // Hover/pressed/text steps come from the same family.
    QVERIFY(AroraStyle::torBarStyleSheet().contains(
        AroraStyle::torChromeHoverColor().name()));
    QVERIFY(AroraStyle::torStatusBarStyleSheet().contains(
        AroraStyle::torDimTextColor().name()));
    QVERIFY(AroraStyle::torToolButtonStyleSheet().contains(
        AroraStyle::torChromePressedColor().name()));
}

void tst_Arorastyle::chromeGrab()
{
    // Offscreen grab for visual review — set ARORA_CHROME_GRAB_DIR to
    // a directory and the main window lands there as mainwindow.png.
    const QByteArray grabDir = qgetenv("ARORA_CHROME_GRAB_DIR");
    if (grabDir.isEmpty())
        QSKIP("ARORA_CHROME_GRAB_DIR not set");

    BrowserMainWindow *window = new BrowserMainWindow;
    window->resize(1000, 640);
    window->show();
    QApplication::processEvents();

    QDir().mkpath(QString::fromLocal8Bit(grabDir));
    QVERIFY(window->grab().save(
        QString::fromLocal8Bit(grabDir)
        + QLatin1String("/mainwindow.png")));

    delete window;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

QTEST_MAIN(tst_Arorastyle)
#include "tst_arorastyle.moc"
