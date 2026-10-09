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

#include "urlcleaner.h"

#include <qsettings.h>
#include <qurlquery.h>

#include <iterator>

// Curated defaults — marketing/analytics click identifiers and
// campaign parameters observed across Google, Meta, Microsoft,
// Mailchimp, HubSpot, Yandex, Twitter/X, LinkedIn, TikTok and the
// common affiliate networks.  Trailing '*' = prefix match.
static const char *const kBuiltInTrackingParams[] = {
    // Google / DoubleClick / analytics
    "utm_*", "gclid", "gbraid", "wbraid", "dclid", "gclsrc",
    "_ga", "_gl", "_gac", "_bta_*",
    // Meta
    "fbclid", "fb_action_ids", "fb_action_types", "fb_ref",
    "fb_source",
    // Microsoft / Yahoo / Yandex / Twitter / LinkedIn / TikTok
    "msclkid", "yclid", "twclid", "li_fat_id", "ttclid", "s_kwcid",
    // Mailchimp
    "mc_cid", "mc_eid",
    // HubSpot / Marketo / other marketing automation
    "_hsenc", "_hsmi", "hsCtaTracking", "__hssc", "__hstc", "__hsfp",
    "mkt_tok", "vero_id", "wickedid", "vero_conv",
    // Instagram / social share
    "igshid", "igsh",
    // Generic referer-style trackers
    "ref_src", "ref_url", "spm", "scm", "si",
    // Ometrics / Matomo / affiliate
    "oly_anon_id", "oly_enc_id", "_openstat", "pk_*",
    "trk", "trkCampaign",
};

static QStringList s_additionalParams;

QStringList UrlCleaner::trackingParameters()
{
    QStringList params;
    params.reserve(int(std::size(kBuiltInTrackingParams)) + 16);
    for (const char *param : kBuiltInTrackingParams)
        params.append(QString::fromLatin1(param));
    // User-facing extension point: a QStringList or comma-separated
    // string of extra names/prefixes to strip.
    const QStringList extra =
        QSettings().value(QLatin1String("urlcleaner/extraParams"))
            .toStringList();
    params += extra;
    params += s_additionalParams;
    return params;
}

void UrlCleaner::setAdditionalParameters(const QStringList &parameters)
{
    s_additionalParams = parameters;
}

bool UrlCleaner::isTrackingParameter(const QString &name)
{
    const QString lower = name.toLower();
    const QStringList params = trackingParameters();
    for (const QString &param : params) {
        const QString pattern = param.toLower();
        if (pattern.endsWith(QLatin1Char('*'))) {
            if (lower.startsWith(pattern.left(pattern.size() - 1)))
                return true;
        } else if (lower == pattern) {
            return true;
        }
    }
    return false;
}

QUrl UrlCleaner::cleanedUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if ((scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            || !url.hasQuery())
        return url;

    const QUrlQuery query(url);
    const QList<QPair<QString, QString> > items =
        query.queryItems(QUrl::FullyDecoded);

    QList<QPair<QString, QString> > kept;
    kept.reserve(items.size());
    bool removed = false;
    for (const QPair<QString, QString> &item : items) {
        if (isTrackingParameter(item.first)) {
            removed = true;
        } else {
            kept.append(item);
        }
    }
    if (!removed)
        return url;

    QUrl cleaned = url;
    if (kept.isEmpty()) {
        // An empty query must drop the '?' entirely.
        cleaned.setQuery(QString());
    } else {
        QUrlQuery cleanQuery;
        cleanQuery.setQueryItems(kept);
        cleaned.setQuery(cleanQuery);
    }
    return cleaned;
}
