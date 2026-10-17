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
    const QString multipurpose = tr("Multipurpose");

    QList<AdBlockListPreset> presets;

    presets.append({ core,
        QLatin1String("uBlock filters"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/filters.txt"),
        tr("uBlock Origin's primary filter list — enabled by default in uBlock Origin.") });
    presets.append({ core,
        QLatin1String("EasyList"),
        QLatin1String("https://easylist.to/easylist/easylist.txt"),
        tr("The standard ad-blocking list; enabled by default in uBlock Origin.") });
    presets.append({ core,
        QLatin1String("EasyPrivacy"),
        QLatin1String("https://easylist.to/easylist/easyprivacy.txt"),
        tr("Tracker protection; enabled by default in uBlock Origin.") });
    presets.append({ core,
        QLatin1String("AdGuard Base"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/2.txt"),
        tr("AdGuard's own ad-blocking rules, complementary to EasyList.") });
    presets.append({ core,
        QLatin1String("AdGuard Tracking Protection"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/3.txt"),
        tr("Blocks trackers and analytics; complements EasyPrivacy.") });
    presets.append({ core,
        QLatin1String("AdGuard URL Tracking Protection"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/17.txt"),
        tr("Strips tracking parameters from URLs.") });
    presets.append({ core,
        QLatin1String("Block Outsider Intrusion into LAN"),
        QLatin1String("https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/lan-block.txt"),
        tr("Blocks public sites from probing your local network (anti-DNS-rebinding).") });
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
        QLatin1String("uBlock filters \u2013 Annoyances"),
        QLatin1String("https://ublockorigin.github.io/uAssets/filters/annoyances.txt"),
        tr("uBlock Origin's combined anti-annoyance rules.") });
    presets.append({ annoyances,
        QLatin1String("AdGuard Annoyances"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/14.txt"),
        tr("AdGuard's combined cookie/popup/social-widget annoyance rules.") });
    presets.append({ annoyances,
        QLatin1String("AdGuard Social Media"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/4.txt"),
        tr("Removes social-media widgets, like/share buttons, and counters.") });
    presets.append({ annoyances,
        QLatin1String("Anti-Facebook"),
        QLatin1String("https://secure.fanboy.co.nz/fanboy-antifacebook.txt"),
        tr("Blocks Facebook trackers and widgets embedded on other sites.") });
    presets.append({ annoyances,
        QLatin1String("Dandelion Sprout's Annoyances List"),
        QLatin1String("https://raw.githubusercontent.com/DandelionSprout/adfilt/master/AnnoyancesList"),
        tr("Extra rules for nag screens, surveys, and other annoyances.") });
    presets.append({ annoyances,
        QLatin1String("Actually Legitimate URL Shortener"),
        QLatin1String("https://raw.githubusercontent.com/DandelionSprout/adfilt/master/LegitimateURLShortener.txt"),
        tr("Strips tracking junk from shared links (shipped by uBlock Origin).") });
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

    presets.append({ multipurpose,
        QLatin1String("Dan Pollock's hosts file"),
        QLatin1String("https://someonewhocares.org/hosts/zero/hosts"),
        tr("Long-running hosts-file ad/malware domain blocklist.") });
    presets.append({ multipurpose,
        QLatin1String("MVPS HOSTS"),
        QLatin1String("https://winhelp2002.mvps.org/hosts.txt"),
        tr("Classic hosts-file ad/tracker domain blocklist.") });
    presets.append({ multipurpose,
        QLatin1String("Peter Lowe's Ad and tracking server list"),
        QLatin1String("https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&showintro=1&mimetype=plaintext"),
        tr("Compact list of well-known ad/tracking server domains.") });

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
        QLatin1String("AdGuard German"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/6.txt"),
        tr("AdGuard's own German-language site coverage.") });
    presets.append({ regional,
        QLatin1String("AdGuard French"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/16.txt"),
        tr("AdGuard's own French-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Czech and Slovak"),
        QLatin1String("https://raw.githubusercontent.com/tomasko126/easylistczechandslovak/master/filters.txt"),
        tr("Czech and Slovak site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Lithuania"),
        QLatin1String("https://raw.githubusercontent.com/EasyList-Lithuania/easylist_lithuania/master/easylistlithuania.txt"),
        tr("Lithuanian site coverage.") });
    presets.append({ regional,
        QLatin1String("hufilter"),
        QLatin1String("https://hufilter.github.io/hufilter/hufilter.txt"),
        tr("Hungarian site coverage — the community hufilter project, updated daily.") });
    presets.append({ regional,
        QLatin1String("EasyList Spanish"),
        QLatin1String("https://easylist-downloads.adblockplus.org/easylistspanish.txt"),
        tr("Spanish-language site coverage.") });
    presets.append({ regional,
        QLatin1String("AdGuard Spanish/Portuguese"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/9.txt"),
        tr("Spanish and Portuguese (incl. Brazilian) site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Italy"),
        QLatin1String("https://easylist-downloads.adblockplus.org/easylistitaly.txt"),
        tr("Italian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Liste AR"),
        QLatin1String("https://easylist-downloads.adblockplus.org/Liste_AR.txt"),
        tr("Arabic-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList China"),
        QLatin1String("https://easylist-downloads.adblockplus.org/easylistchina.txt"),
        tr("Chinese-language site coverage.") });
    presets.append({ regional,
        QLatin1String("IndianList"),
        QLatin1String("https://easylist-downloads.adblockplus.org/indianlist.txt"),
        tr("Indian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyDutch"),
        QLatin1String("https://raw.githubusercontent.com/EasyDutch-uBO/easydutch/main/EasyDutch.txt"),
        tr("Dutch-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Hebrew"),
        QLatin1String("https://raw.githubusercontent.com/easylist/EasyListHebrew/master/EasyListHebrew.txt"),
        tr("Hebrew-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Official Polish Filters"),
        QLatin1String("https://raw.githubusercontent.com/MajkiIT/polish-ads-filter/master/polish-adblock-filters/adblock.txt"),
        tr("Polish-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Adblock Bulgarian"),
        QLatin1String("https://raw.githubusercontent.com/RealEnder/adblockbg/master/adblockbg.txt"),
        tr("Bulgarian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("ABPVN"),
        QLatin1String("https://raw.githubusercontent.com/abpvn/abpvn/master/filter/abpvn.txt"),
        tr("Vietnamese-language site coverage.") });
    presets.append({ regional,
        QLatin1String("AdGuard Chinese"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/224.txt"),
        tr("AdGuard's own Chinese-language site coverage.") });
    presets.append({ regional,
        QLatin1String("KO Classic"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/227.txt"),
        tr("Korean-language site coverage.") });
    presets.append({ regional,
        QLatin1String("AdGuard Japanese"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/7.txt"),
        tr("Japanese-language site coverage.") });
    presets.append({ regional,
        QLatin1String("AdGuard Turkish"),
        QLatin1String("https://filters.adtidy.org/extension/ublock/filters/13.txt"),
        tr("Turkish-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Greek Adblock Filter"),
        QLatin1String("https://raw.githubusercontent.com/kargig/greek-adblockplus-filter/master/void-gr-filters.txt"),
        tr("Greek-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Estonian List"),
        QLatin1String("https://adblock.ee/list.php"),
        tr("Estonian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Latvian List"),
        QLatin1String("https://raw.githubusercontent.com/Latvian-List/adblock-latvian/master/lists/latvian-list.txt"),
        tr("Latvian-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Nordic Filters"),
        QLatin1String("https://raw.githubusercontent.com/DandelionSprout/adfilt/master/NorwegianList.txt"),
        tr("Norwegian + Nordic site coverage (DandelionSprout).") });
    presets.append({ regional,
        QLatin1String("Icelandic ABP List"),
        QLatin1String("https://adblock.gardar.net/is.abp.txt"),
        tr("Icelandic-language site coverage.") });
    presets.append({ regional,
        QLatin1String("Frellwit's Swedish Filter"),
        QLatin1String("https://raw.githubusercontent.com/lassekongo83/Frellwits-filter-lists/master/Frellwits-Swedish-Filter.txt"),
        tr("Swedish-language site coverage (shipped by uBlock Origin).") });
    presets.append({ regional,
        QLatin1String("Finnish List"),
        QLatin1String("https://raw.githubusercontent.com/finnish-easylist-addition/finnish-easylist-addition/master/Finland_adb.txt"),
        tr("Finnish-language site coverage.") });
    presets.append({ regional,
        QLatin1String("EasyList Thailand"),
        QLatin1String("https://raw.githubusercontent.com/easylist-thailand/easylist-thailand/master/subscription/easylist-thailand.txt"),
        tr("Thai-language site coverage.") });
    presets.append({ regional,
        QLatin1String("ABPindo"),
        QLatin1String("https://raw.githubusercontent.com/ABPindo/indonesianadblockrules/master/subscriptions/abpindo.txt"),
        tr("Indonesian-language site coverage.") });

    return presets;
}

}
