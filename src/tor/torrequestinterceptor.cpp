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

#include <qwebengineurlrequestinfo.h>

TorRequestInterceptor::TorRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
{
}

void TorRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread — no GUI state may be touched.
    const QUrl url = info.requestUrl();
    if (url.scheme() == QLatin1String("http")
        && !url.host().endsWith(QLatin1String(".onion"))) {
        QUrl https = url;
        https.setScheme(QLatin1String("https"));
        info.redirect(https);
        return;
    }
    m_adBlock->interceptRequest(info);
}
