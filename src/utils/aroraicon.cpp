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

#include "aroraicon.h"

#include "arorastyle.h"
#include "browsertheme.h"

#include <qapplication.h>
#include <qdir.h>
#include <qevent.h>
#include <qfile.h>
#include <qiconengine.h>
#include <qimage.h>
#include <qpainter.h>
#include <qpalette.h>
#include <qpixmap.h>
#include <qsettings.h>
#include <qstylehints.h>

namespace {

const QLatin1String s_resourcePrefix(":/icons");
const QLatin1String s_nativeId("native");
// ICONS10: bundled Tabler is the default set — "native" stays
// selectable but a sparse desktop theme served missing names as
// duplicate lookalikes (the "some icons look the same" report).
const QLatin1String s_defaultId("tabler");

QString s_systemTheme;
QString s_currentId;
bool s_initialized = false;

// QApplication::paletteChanged is deprecated; the same notification
// arrives as an event on the application object.
class PaletteWatcher : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationPaletteChange)
            AroraIcon::setTheme(s_currentId);
        return QObject::eventFilter(watched, event);
    }
};

void init()
{
    if (s_initialized)
        return;
    s_initialized = true;

    // Capture before anything can call setThemeName — this is what
    // "native" switches back to.
    s_systemTheme = QIcon::themeName();

    QStringList paths = QIcon::themeSearchPaths();
    if (!paths.contains(s_resourcePrefix))
        paths.append(s_resourcePrefix);
    QIcon::setThemeSearchPaths(paths);
    // The original 2009 artwork: last resort for every mode, and the
    // only icon set a theme-less desktop ever misses on.
    QIcon::setFallbackThemeName(QLatin1String("arora-legacy"));

    if (qApp) {
        // A forced light->dark flip (UIP01) and a desktop scheme
        // change both move the palette — re-resolve so the bundled
        // sets swap to/from their -dark variants.
        qApp->installEventFilter(new PaletteWatcher(qApp));
        QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, qApp, [](Qt::ColorScheme) {
            AroraIcon::setTheme(s_currentId);
        });
    }

    AroraIcon::applyFromSettings();
}

// ICONS05: does the captured system theme already serve
// light-on-dark glyphs?  Name heuristics cannot answer this — some
// "dark" desktops still ship dark-glyph sets Qt cannot
// currentColor-recolor (stock Adwaita symbolics stay #222 even when
// the desktop palette is dark), and a forced dark palette (THEME01)
// is invisible to the OS icon theme entirely — so render a probe
// icon through the system theme and measure it.  Cached: the system
// theme name is itself captured once at init.
bool systemThemeIsDarkAware()
{
    if (s_systemTheme.isEmpty())
        return false;
    static int aware = -1;
    if (aware != -1)
        return aware == 1;
    const QString restore = QIcon::themeName();
    QIcon::setThemeName(s_systemTheme);
    const QImage probe = QIcon::fromTheme(QLatin1String("go-previous"))
        .pixmap(QSize(16, 16)).toImage();
    QIcon::setThemeName(restore);
    qlonglong luminance = 0;
    int opaque = 0;
    for (int y = 0; y < probe.height(); ++y) {
        for (int x = 0; x < probe.width(); ++x) {
            const QColor color = probe.pixelColor(x, y);
            if (color.alpha() > 128) {
                luminance += qGray(color.rgb());
                ++opaque;
            }
        }
    }
    aware = (opaque > 0 && luminance / opaque > 140) ? 1 : 0;
    return aware == 1;
}

// ICONS06: repaint `source` as a flat `color` glyph — every pixel
// keeps its alpha and takes the new color (SourceIn compositing),
// so themed art of any palette resolves to the same light glyph the
// painted reader/onion buttons draw on dark chrome.
QPixmap tintedPixmap(const QPixmap &source, const QColor &color)
{
    if (source.isNull())
        return source;
    QImage image = source.toImage()
        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), color);
    painter.end();
    QPixmap out = QPixmap::fromImage(image);
    out.setDevicePixelRatio(source.devicePixelRatio());
    return out;
}

// ICONS09: chrome icon engine — the themed art serves shape+alpha
// only; every opaque pixel is re-tinted to the palette's ButtonText
// (Disabled mode takes the palette's disabled tone rather than Qt's
// generated ghosting, same lesson as ICONS06; THEM03: Selected takes
// HighlightedText — the glyph rides a Highlight card on nav rails).
// The tint is resolved
// per pixmap() request — not baked at setIcon() — so a runtime
// light<->dark flip (THEME01 preview, desktop scheme change) or an
// icon-theme swap reaches already-installed icons without any widget
// having to listen for it.
class ChromeIconEngine : public QIconEngine
{
public:
    explicit ChromeIconEngine(const QString &name)
        : m_name(name)
    {
    }

    QIconEngine *clone() const override
    {
        return new ChromeIconEngine(m_name);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal scale = painter->device()
            ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect.topLeft(),
                            scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode,
                   QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode,
                         QIcon::State state, qreal scale) override
    {
        // Always Normal art from the theme — a set's own Disabled
        // rendering would double-dim under our disabled tint.
        QPixmap base = AroraIcon::get(m_name).pixmap(size * scale,
                                                   QIcon::Normal, state);
        if (base.isNull())
            return base;
        base.setDevicePixelRatio(scale);
        const QPalette palette = qApp ? qApp->palette() : QPalette();
        if (mode == QIcon::Disabled)
            return tintedPixmap(base, palette.color(
                QPalette::Disabled, QPalette::ButtonText));
        // THEM03: selected chrome glyphs ride a Highlight card (the
        // prefs nav rail, the sidebar section rail) — ButtonText on
        // Highlight can sink to unreadable, so the selected request
        // tints to the matching foreground.  THEM04: through
        // selectionTextColor, the same pick the sheets give the
        // card's text — a sub-AA platform pairing can't split the
        // glyph and label onto different colors.
        if (mode == QIcon::Selected)
            return tintedPixmap(base,
                                AroraStyle::selectionTextColor(palette));
        return tintedPixmap(base, palette.color(QPalette::Normal,
                                                QPalette::ButtonText));
    }

private:
    const QString m_name;
};

// The bundled -dark set closest to the captured system theme —
// where "native" resolves under a dark palette when the real system
// theme cannot serve light glyphs.  An empty/unknown system theme
// lands on Adwaita, the same set the legacy fallback chain already
// inherits, so the glyph family stays consistent.
QString bundledDarkThemeName()
{
    const QString sys = s_systemTheme.toLower();
    QString id = QLatin1String("adwaita");
    if (sys.contains(QLatin1String("breeze")))
        id = QLatin1String("breeze");
    else if (sys.contains(QLatin1String("tabler")))
        id = QLatin1String("tabler");
    else if (sys.contains(QLatin1String("lucide")))
        id = QLatin1String("lucide");
    return QLatin1String("arora-") + id + QLatin1String("-dark");
}

} // namespace

QIcon AroraIcon::get(const QString &name)
{
    init();
    return QIcon::fromTheme(name);
}

QIcon AroraIcon::getOnDark(const QString &name)
{
    init();
    const QString id = s_currentId;
    if (id != s_nativeId) {
        const QString base = s_resourcePrefix + QLatin1Char('/')
            + QLatin1String("arora-") + id;
        // File-backed, so it does not follow setThemeName — the tor
        // window is a separate process whose icon theme never changes
        // after startup anyway.
        for (const char *variant : { "-dark", "" }) {
            const QString path = base + QLatin1String(variant)
                + QLatin1String("/icons/") + name
                + QLatin1String(".svg");
            if (QFile::exists(path))
                return QIcon(path);
        }
    }
    return get(name);
}

QIcon AroraIcon::tinted(const QIcon &icon, const QColor &color,
                        const QColor &disabledColor)
{
    QIcon out;
    // Cover the sizes chrome buttons actually request so QIcon never
    // has to rescale a tinted pixmap (or fall back to generating the
    // Disabled mode itself — that ghosting was the wash-out).
    static const int sizes[] = { 16, 18, 20, 22, 24, 28, 32, 48 };
    for (int size : sizes) {
        const QPixmap normal = icon.pixmap(QSize(size, size));
        if (normal.isNull())
            continue;
        out.addPixmap(tintedPixmap(normal, color), QIcon::Normal);
        out.addPixmap(tintedPixmap(normal, disabledColor),
                      QIcon::Disabled);
    }
    return out;
}

QIcon AroraIcon::getTinted(const QString &name, const QColor &color,
                           const QColor &disabledColor)
{
    return tinted(getOnDark(name), color, disabledColor);
}

QIcon AroraIcon::getChrome(const QString &name)
{
    init();
    return QIcon(new ChromeIconEngine(name));
}

QStringList AroraIcon::themeIds()
{
    return { s_nativeId,
             QLatin1String("adwaita"),
             QLatin1String("breeze"),
             QLatin1String("tabler"),
             QLatin1String("lucide") };
}

QString AroraIcon::themeDisplayName(const QString &id)
{
    if (id == s_nativeId)
        return QCoreApplication::translate("AroraIcon", "Native");
    if (id == QLatin1String("adwaita"))
        return QLatin1String("Adwaita");
    if (id == QLatin1String("breeze"))
        return QLatin1String("Breeze");
    if (id == QLatin1String("tabler"))
        return QLatin1String("Tabler");
    if (id == QLatin1String("lucide"))
        return QLatin1String("Lucide");
    return id;
}

QString AroraIcon::theme()
{
    const QString id = QSettings()
        .value(QLatin1String("MainWindow/iconTheme"), s_defaultId)
        .toString();
    return themeIds().contains(id) ? id : s_defaultId;
}

QString AroraIcon::effectiveThemeName(const QString &id)
{
    init();
    const bool dark = qApp && BrowserTheme::isDarkPalette(qApp->palette());
    if (id == s_nativeId) {
        // A dark application palette cannot reach the OS icon theme —
        // when it is provably still serving dark glyphs the bundled
        // -dark art takes over so every chrome surface stays legible.
        if (dark && !systemThemeIsDarkAware())
            return bundledDarkThemeName();
        return s_systemTheme;
    }
    QString name = QLatin1String("arora-") + id;
    if (dark
        && QFile::exists(s_resourcePrefix + QLatin1Char('/')
                         + name + QLatin1String("-dark/index.theme"))) {
        name += QLatin1String("-dark");
    }
    return name;
}

void AroraIcon::setTheme(const QString &id)
{
    init();
    s_currentId = themeIds().contains(id) ? id : s_defaultId;
    QIcon::setThemeName(effectiveThemeName(s_currentId));
}

void AroraIcon::applyFromSettings()
{
    setTheme(theme());
}

QStringList AroraIcon::names()
{
    init();
    QStringList out;
    // The union across every bundled set — ICONS10: tabler ships the
    // full chrome surface, so reading only one set's directory would
    // under-report what callers can ask for.
    for (const char *set : { "arora-adwaita", "arora-breeze",
                            "arora-tabler", "arora-lucide",
                            "arora-legacy" }) {
        QStringList entries = QDir(s_resourcePrefix + QLatin1Char('/')
            + QLatin1String(set) + QLatin1String("/icons"))
            .entryList(QStringList(QLatin1String("*.svg")),
                       QDir::Files, QDir::Name);
        for (QString &name : entries) {
            name.chop(4);
            if (!out.contains(name))
                out.append(name);
        }
    }
    out.sort();
    return out;
}

QIcon AroraIcon::iconForTheme(const QString &id, const QString &name)
{
    init();
    if (id == s_nativeId)
        return QIcon::fromTheme(name);
    // effectiveThemeName picks the -dark recolor under a dark
    // palette — previews should show what the set will actually look
    // like once applied.
    const QString path = s_resourcePrefix + QLatin1Char('/')
        + effectiveThemeName(id)
        + QLatin1String("/icons/") + name + QLatin1String(".svg");
    if (!QFile::exists(path))
        return QIcon();
    return QIcon(path);
}
