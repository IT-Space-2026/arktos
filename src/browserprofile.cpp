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

#include "browserprofile.h"

#include "acceptlanguagedialog.h"

#include <qapplication.h>
#include <qfile.h>
#include <qsettings.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>

namespace BrowserProfile {

QWebEngineProfile *normalProfile()
{
    // Lazily-created so callers do not need to coordinate with main():
    // every user (main.cpp, CookieJar::instance(), the settings dialog)
    // gets the same named "arora" profile.
    static QWebEngineProfile *profile = 0;
    if (!profile)
        profile = new QWebEngineProfile(QLatin1String("arora"), qApp);
    return profile;
}

QWebEngineProfile *privateProfile()
{
    // An unnamed profile is off-the-record: nothing hits disk.
    static QWebEngineProfile *profile = 0;
    if (!profile)
        profile = new QWebEngineProfile(qApp);
    return profile;
}

// QWebEngineScript has no "replace" — remove a previously installed
// user style sheet by name before inserting the new one.
static void installUserStyleSheet(QWebEngineProfile *profile, const QUrl &url)
{
    const QString name = QLatin1String("aroraUserStyleSheet");
    QWebEngineScriptCollection *scripts = profile->scripts();
    const QList<QWebEngineScript> installed = scripts->toList();
    for (const QWebEngineScript &script : installed) {
        if (script.name() == name)
            scripts->remove(script);
    }
    if (url.isEmpty())
        return;

    QString css;
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (file.open(QIODevice::ReadOnly))
            css = QString::fromUtf8(file.readAll());
    }

    // Escape a string for embedding as a JS string literal.
    const auto jsQuote = [](QString text) {
        text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        text.replace(QLatin1Char('"'), QLatin1String("\\\""));
        text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
        text.remove(QLatin1Char('\r'));
        return text;
    };

    QString source;
    if (!css.isEmpty()) {
        source = QStringLiteral(
            "(function(){"
            "var style=document.createElement('style');"
            "style.type='text/css';"
            "style.appendChild(document.createTextNode(\"%1\"));"
            "document.documentElement.appendChild(style);})();")
            .arg(jsQuote(css));
    } else if (url.scheme() == QLatin1String("http")
               || url.scheme() == QLatin1String("https")) {
        // Remote css cannot be read synchronously; link it instead.
        source = QStringLiteral(
            "(function(){"
            "var link=document.createElement('link');"
            "link.rel='stylesheet';link.type='text/css';link.href=\"%1\";"
            "document.documentElement.appendChild(link);})();")
            .arg(jsQuote(QString::fromUtf8(url.toEncoded())));
    }
    if (source.isEmpty())
        return;

    QWebEngineScript script;
    script.setName(name);
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setRunsOnSubFrames(true);
    // A separate world keeps page scripts from tripping over the
    // injection; the DOM is shared so the style still applies.
    script.setWorldId(QWebEngineScript::ApplicationWorld);
    script.setSourceCode(source);
    scripts->insert(script);
}

void applySettings(QWebEngineProfile *profile)
{
    QWebEngineSettings *engineSettings = profile->settings();

    QSettings settings;
    settings.beginGroup(QLatin1String("websettings"));

    QFont standardFont(engineSettings->fontFamily(QWebEngineSettings::StandardFont),
                       engineSettings->fontSize(QWebEngineSettings::DefaultFontSize));
    standardFont = settings.value(QLatin1String("standardFont"), standardFont).value<QFont>();
    engineSettings->setFontFamily(QWebEngineSettings::StandardFont, standardFont.family());
    engineSettings->setFontSize(QWebEngineSettings::DefaultFontSize, standardFont.pointSize());
    int minimumFontSize = settings.value(QLatin1String("minimumFontSize"),
                                         engineSettings->fontSize(QWebEngineSettings::MinimumFontSize)).toInt();
    engineSettings->setFontSize(QWebEngineSettings::MinimumFontSize, minimumFontSize);

    QFont fixedFont(engineSettings->fontFamily(QWebEngineSettings::FixedFont),
                    engineSettings->fontSize(QWebEngineSettings::DefaultFixedFontSize));
    fixedFont = settings.value(QLatin1String("fixedFont"), fixedFont).value<QFont>();
    engineSettings->setFontFamily(QWebEngineSettings::FixedFont, fixedFont.family());
    engineSettings->setFontSize(QWebEngineSettings::DefaultFixedFontSize, fixedFont.pointSize());

    engineSettings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                 !settings.value(QLatin1String("blockPopupWindows"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                 settings.value(QLatin1String("enableJavascript"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::PluginsEnabled,
                                 settings.value(QLatin1String("enablePlugins"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::AutoLoadImages,
                                 settings.value(QLatin1String("enableImages"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::LocalStorageEnabled,
                                 settings.value(QLatin1String("enableLocalStorage"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, true);

    installUserStyleSheet(profile, settings.value(QLatin1String("userStyleSheet")).toUrl());
    settings.endGroup();

    // The app's network cache preference drives both the profile's
    // Chromium cache and (through NetworkAccessManager::loadSettings)
    // the app-side fetch cache.
    settings.beginGroup(QLatin1String("network"));
    bool cacheEnabled = settings.value(QLatin1String("cacheEnabled"), true).toBool();
    profile->setHttpCacheType(cacheEnabled ? QWebEngineProfile::DiskHttpCache
                                           : QWebEngineProfile::NoCache);
    profile->setHttpCacheMaximumSize(
        settings.value(QLatin1String("maximumCacheSize"), 50).toInt() * 1024 * 1024);
    profile->setHttpAcceptLanguage(
        QString::fromUtf8(AcceptLanguageDialog::httpString(AcceptLanguageDialog::acceptLanguages())));
    settings.endGroup();
}

} // namespace BrowserProfile
