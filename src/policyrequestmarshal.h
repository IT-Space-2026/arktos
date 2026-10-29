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

#ifndef POLICYREQUESTMARSHAL_H
#define POLICYREQUESTMARSHAL_H

#include "engine/navigationpolicy.h"
#include "scriptcontrolmanager.h"

#include <qwebengineurlrequestinfo.h>

// CA08: the QWebEngineUrlRequestInfo -> Engine::PolicyRequest marshal
// the privacy and tor request interceptors each carried privately.
// navigationpolicy.h stays engine-agnostic by contract (no
// QtWebEngine types across it), so the adapter lives with the
// interceptors instead.  The caller fills the policy-side fields the
// marshal cannot know (scope, torMode, minRefererLevel).
namespace PolicyRequestMarshal {

inline Engine::HeaderList toHeaderList(
    const QHash<QByteArray, QByteArray> &headers)
{
    Engine::HeaderList list;
    list.reserve(headers.size());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        list.append(qMakePair(it.key(), it.value()));
    return list;
}

// The request fields every interceptor feeds the policy, plus the
// JSCTL grant resolution for script-execution resource types (the
// per-site grant is data the policy cannot reach — it is
// pre-resolved here, only for the types that run script).
inline Engine::PolicyRequest fromRequestInfo(
    QWebEngineUrlRequestInfo &info)
{
    Engine::PolicyRequest req;
    req.url = info.requestUrl();
    req.firstPartyUrl = info.firstPartyUrl();
    req.resourceType = static_cast<int>(info.resourceType());
    req.method = info.requestMethod();
    req.headers = toHeaderList(info.httpHeaders());
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
    return req;
}

} // namespace PolicyRequestMarshal

#endif // POLICYREQUESTMARSHAL_H
