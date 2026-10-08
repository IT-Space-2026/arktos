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

#include "torrequestinterceptor.h"

#include "adblockrequestinterceptor.h"
#include "privacyrequestinterceptor.h"

#include <qwebengineurlrequestinfo.h>

TorRequestInterceptor::TorRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
{
}

void TorRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread — no GUI state may be touched.
    //
    // STALL01: internal/non-web requests (devtools:, chrome:, qrc:,
    // arora-*:, abp:, ...) pass through untouched, same as in the
    // privacy and adblock interceptors.
    if (!AdBlockRequestInterceptor::isWebRequestScheme(
            info.requestUrl().scheme()))
        return;
    const QUrl url = info.requestUrl();
    if (url.scheme() == QLatin1String("http")
        && !url.host().endsWith(QLatin1String(".onion"))) {
        QUrl https = url;
        https.setScheme(QLatin1String("https"));
        info.redirect(https);
        return;
    }
    // PING01: beacons, <a ping> audits and CSP reports are telemetry
    // uploads — the shared privacy toggle drops them in tor windows
    // too; a deanonymizing POST is the last thing that should slip.
    if (PrivacyRequestInterceptor::blockPingsEnabled()
        && (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypePing
            || info.resourceType()
                   == QWebEngineUrlRequestInfo::ResourceTypeCspReport)) {
        info.block(true);
        return;
    }
    // SAFE04: prefetch loads connect to sites the user never visited —
    // the shared privacy toggles apply in tor windows too, where an
    // unsolicited connection is the most suspicious traffic of all.
    if (PrivacyRequestInterceptor::shouldBlockResource(
            info.resourceType(), info.httpHeaders())) {
        info.block(true);
        return;
    }
    // XSLEAK03: the opt-in third-party WebSocket block applies in tor
    // windows too — a cross-site socket's connect outcome is the same
    // state oracle, and the upgrade is interceptor-visible.
    if (PrivacyRequestInterceptor::shouldBlockWebSocket(
            info.firstPartyUrl(), info.requestUrl(),
            info.resourceType())) {
        info.block(true);
        return;
    }
    // SECLVL: the only http: left here is .onion — Safer drops its
    // script-execution fetches just like on the normal profile.
    if (PrivacyRequestInterceptor::shouldBlockScript(
            info.firstPartyUrl(), info.resourceType())) {
        info.block(true);
        return;
    }
    // REF01: a tor window must never leak where the user came from —
    // at least the Trimmed policy applies here even if the normal
    // profiles run EngineDefault; a stricter user choice still wins.
    PrivacyRequestInterceptor::applyRefererPolicy(
        info, PrivacyRequestInterceptor::RefererTrimmed);
    m_adBlock->interceptRequest(info);
}
