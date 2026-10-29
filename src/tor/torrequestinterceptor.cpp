/*
 * Copyright 2026 Arktos Browser - IT-Space
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
#include "policyrequestmarshal.h"
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

TorRequestInterceptor::TorRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
{
}

void TorRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread — no GUI state may be touched.
    // POL04: settle the host's cleaning tally on every new top-level
    // request (the marked follow-up of a cleaning redirect keeps it).
    if (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
        PrivacyRequestInterceptor::noteMainFrameRequest(info.requestUrl());
    PolicyRequest req = PolicyRequestMarshal::fromRequestInfo(info);
    req.torMode = true;
    req.minRefererLevel = PrivacyRequestInterceptor::RefererTrimmed;

    const PolicyVerdict verdict = NavigationPolicy::evaluate(req);
    switch (verdict.action) {
    case PolicyVerdict::Action::Block:
        info.block(true);
        return;
    case PolicyVerdict::Action::Redirect:
        info.redirect(verdict.redirectUrl);
        // POL04: tally the cleaning stage's dropped tracker units for
        // the shield panel's per-page count — same shared table the
        // clearnet interceptor writes.  Main-frame only, like there.
        if (info.resourceType()
            == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
            PrivacyRequestInterceptor::noteCleanedRedirect(
                verdict.redirectUrl, verdict.cleanedCount);
        return;
    case PolicyVerdict::Action::Allow:
        if (verdict.refererSet)
            info.setHttpHeader("Referer", verdict.refererValue);
        // PRIV08: the tor verdict's sendGpc is always true — the
        // uniform-constant doctrine (see PolicyVerdict::sendGpc), so
        // every tor window emits the same "Sec-GPC: 1".
        if (verdict.sendGpc)
            info.setHttpHeader("Sec-GPC", "1");
        m_adBlock->interceptRequest(info);
        return;
    case PolicyVerdict::Action::Pass:
        return;
    }
}
