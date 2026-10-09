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
#include "navigationpolicy.h"
#include "privacyrequestinterceptor.h"
#include "scriptcontrolmanager.h"

#include <qwebengineurlrequestinfo.h>

// ENG02: thin adapter — the tor pipeline variant runs inside
// Engine::NavigationPolicy (tor_mode manifest flag): unconditional
// clearnet http->https upgrade on every resource type, .onion exempt,
// shared strip/blocklist/ping/resource/ws/script stages, at least the
// Trimmed referer policy.  No branching policy logic lives here.

using Engine::HeaderList;
using Engine::NavigationPolicy;
using Engine::PolicyRequest;
using Engine::PolicyVerdict;

static HeaderList toHeaderList(const QHash<QByteArray, QByteArray> &headers)
{
    HeaderList list;
    list.reserve(headers.size());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        list.append(qMakePair(it.key(), it.value()));
    return list;
}

TorRequestInterceptor::TorRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
{
}

void TorRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread — no GUI state may be touched.
    PolicyRequest req;
    req.url = info.requestUrl();
    req.firstPartyUrl = info.firstPartyUrl();
    req.resourceType = static_cast<int>(info.resourceType());
    req.method = info.requestMethod();
    req.headers = toHeaderList(info.httpHeaders());
    req.torMode = true;
    req.minRefererLevel = PrivacyRequestInterceptor::RefererTrimmed;
    switch (info.resourceType()) {
    case QWebEngineUrlRequestInfo::ResourceTypeScript:
    case QWebEngineUrlRequestInfo::ResourceTypeWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeSharedWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeServiceWorker:
        req.scriptAllowed = ScriptControlManager::isAllowedHostSnapshot(
            req.firstPartyUrl.host());
        break;
    default:
        break;
    }

    const PolicyVerdict verdict = NavigationPolicy::evaluate(req);
    switch (verdict.action) {
    case PolicyVerdict::Action::Block:
        info.block(true);
        return;
    case PolicyVerdict::Action::Redirect:
        info.redirect(verdict.redirectUrl);
        return;
    case PolicyVerdict::Action::Allow:
        if (verdict.refererSet)
            info.setHttpHeader("Referer", verdict.refererValue);
        m_adBlock->interceptRequest(info);
        return;
    case PolicyVerdict::Action::Pass:
        return;
    }
}
