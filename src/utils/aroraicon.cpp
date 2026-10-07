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

#include "browsertheme.h"

#include <qapplication.h>
#include <qdir.h>
#include <qevent.h>
#include <qfile.h>
#include <qsettings.h>
#include <qstylehints.h>

namespace {

const QLatin1String s_resourcePrefix(":/icons");
const QLatin1String s_nativeId("native");

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

} // namespace

QIcon AroraIcon::get(const QString &name)
{
    init();
    return QIcon::fromTheme(name);
}

QStringList AroraIcon::themeIds()
{
    return { s_nativeId,
             QLatin1String("adwaita"),
             QLatin1String("breeze"),
             QLatin1String("tabler") };
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
    return id;
}

QString AroraIcon::theme()
{
    const QString id = QSettings()
        .value(QLatin1String("MainWindow/iconTheme"), s_nativeId)
        .toString();
    return themeIds().contains(id) ? id : s_nativeId;
}

QString AroraIcon::effectiveThemeName(const QString &id)
{
    init();
    if (id == s_nativeId)
        return s_systemTheme;
    QString name = QLatin1String("arora-") + id;
    const bool dark = qApp && BrowserTheme::isDarkPalette(qApp->palette());
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
    s_currentId = themeIds().contains(id) ? id : s_nativeId;
    QIcon::setThemeName(effectiveThemeName(s_currentId));
}

void AroraIcon::applyFromSettings()
{
    setTheme(theme());
}

QStringList AroraIcon::names()
{
    init();
    QStringList out = QDir(QLatin1String(":/icons/arora-adwaita/icons"))
        .entryList(QStringList(QLatin1String("*.svg")), QDir::Files, QDir::Name);
    for (QString &n : out)
        n.chop(4);
    return out;
}

QIcon AroraIcon::iconForTheme(const QString &id, const QString &name)
{
    init();
    if (id == s_nativeId)
        return QIcon::fromTheme(name);
    const QString path = s_resourcePrefix + QLatin1Char('/')
        + QLatin1String("arora-") + id
        + QLatin1String("/icons/") + name + QLatin1String(".svg");
    if (!QFile::exists(path))
        return QIcon();
    return QIcon(path);
}
