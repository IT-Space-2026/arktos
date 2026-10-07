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

#include "adblockpresets.h"

#include <qcoreapplication.h>

namespace AdBlockPresets {

static QString tr(const char *sourceText)
{
    return QCoreApplication::translate("AdBlockPresets", sourceText);
}

// EasyList/uBlock sources: easylist.to and ublockorigin.github.io are
// the projects' own mirrors.  Lists on easylist-downloads.
// adblockplus.org are deliberately not used — that host's canonical
// regional files exist, but preferring the projects' GitHub trees keeps
// every preset off a host that has flapped before.
QList<AdBlockListPreset> all()
{
    const QString core = tr("Core & Privacy");
    const QString annoyances = tr("Annoyances");
    const QString security = tr("Security");
    const QString resourceAbuse = tr("Resource Abuse & Mining");
    const QString regional = tr("Regional");

    QList<AdBlockListPreset> presets;

    presets.append({ core,
        QLatin1String("uBlock filters \u2013 Privacy"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/privacy.txt"),
        tr("uBlock Origin's own tracking-protection rules, complementing EasyPrivacy.") });
    presets.append({ core,
        QLatin1String("uBlock filters \u2013 Unbreak"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/unbreak.txt"),
        tr("Repairs site breakage caused by the other filter lists.") });
    presets.append({ core,
        QLatin1String("uBlock filters \u2013 Quick fixes"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt"),
        tr("Fast-shipping fixes for urgent filter issues.") });

    presets.append({ annoyances,
        QLatin1String("EasyList Annoyances"),
        QLatin1String("https://easylist.to/easylist/fanboy-annoyance.txt"),
        tr("Blocks pop-ups, newsletter prompts, and other annoyances (the old Fanboy's Annoyances).") });
    presets.append({ annoyances,
        QLatin1String("EasyList Cookie Notices"),
        QLatin1String("https://secure.fanboy.co.nz/fanboy-cookiemonster.txt"),
        tr("Hides cookie-consent banners and GDPR notice pop-ups.") });
    presets.append({ annoyances,
        QLatin1String("EasyList Social"),
        QLatin1String("https://easylist.to/easylist/fanboy-social.txt"),
        tr("Removes social-media share buttons, widgets, and embedded feeds.") });
    presets.append({ annoyances,
        QLatin1String("uBlock filters \u2013 Cookie Notices"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/annoyances-cookies.txt"),
        tr("uBlock Origin's cookie-banner rules; can be combined with EasyList Cookie Notices.") });
    presets.append({ annoyances,
        QLatin1String("uBlock filters \u2013 Other Annoyances"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/annoyances-others.txt"),
        tr("uBlock Origin rules for overlays and other interruptions.") });
    presets.append({ annoyances,
        QLatin1String("uBlock filters \u2013 Link shorteners"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/ubo-link-shorteners.txt"),
        tr("Bypasses annoying link-shortener interstitials.") });

    presets.append({ security,
        QLatin1String("uBlock filters \u2013 Badware risks"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/badware.txt"),
        tr("Blocks domains known to distribute malware and unwanted software.") });
    presets.append({ security,
        QLatin1String("Online Malicious URL Blocklist"),
        QLatin1String("https://malware-filter.gitlab.io/malware-filter/urlhaus-filter-online.txt"),
        tr("Live abuse.ch URLhaus feed of URLs currently serving malware.") });
    presets.append({ security,
        QLatin1String("Phishing URL Blocklist"),
        QLatin1String("https://malware-filter.gitlab.io/malware-filter/phishing-filter.txt"),
        tr("Curated phishing-domain blocklist (the maintained successor to phishing.army).") });

    presets.append({ resourceAbuse,
        QLatin1String("uBlock filters \u2013 Resource abuse"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/resource-abuse.txt"),
        tr("Blocks sites that secretly abuse CPU/bandwidth, e.g. crypto miners (NoCoin successor).") });
    presets.append({ resourceAbuse,
        QLatin1String("NoCoin"),
        QLatin1String("https://raw.githubusercontent.com/hoshsadiq/adblock-nocoin-list/master/nocoin.txt"),
        tr("Dedicated browser crypto-mining blocklist, community maintained.") });

    presets.append({ regional,
        QLatin1String("EasyList Germany"),
        QLatin1String("https://easylist.to/easylistgermany/easylistgermany.txt"),
        tr("German-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Liste FR"),
        QLatin1String("https://raw.githubusercontent.com/easylist/listefr/master/liste_fr.txt"),
        tr("French-language site coverage.") });
    presets.append({ regional,
        QLatin1String("RU AdList"),
        QLatin1String("https://raw.githubusercontent.com/easylist/ruadlist/master/advblock.txt"),
        tr("Russian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Czech and Slovak"),
        QLatin1String("https://raw.githubusercontent.com/tomasko126/easylistczechandslovak/master/filters.txt"),
        tr("Czech and Slovak site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Lithuania"),
        QLatin1String("https://raw.githubusercontent.com/EasyList-Lithuania/easylist_lithuania/master/easylistlithuania.txt"),
        tr("Lithuanian site coverage.") });

    return presets;
}

}
