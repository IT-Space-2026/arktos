/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2009 Arora Browser
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

#ifndef NETWORKPROXYFACTORY_H
#define NETWORKPROXYFACTORY_H

#include <qnetworkproxy.h>

// A factory installed on a QNetworkAccessManager replaces the
// process-global QNetworkProxy::applicationProxy() for that manager —
// except that a returned entry typed QNetworkProxy::DefaultProxy
// defers back to the application proxy at connect time.  TOR02 relies
// on this: in tor mode no settings proxy is loaded, so queryProxy()
// yields {DefaultProxy} and every app-side request still resolves
// through the managed tor SOCKS5 listener.  It must therefore never
// hand back a configured proxy or NoProxy under tor mode.
class NetworkProxyFactory : public QNetworkProxyFactory
{
public:
    NetworkProxyFactory();

    void setHttpProxy(const QNetworkProxy &proxy);
    void setGlobalProxy(const QNetworkProxy &proxy);

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query = QNetworkProxyQuery()) override;

private:
    QNetworkProxy m_httpProxy;
    QNetworkProxy m_globalProxy;
};

#endif // NETWORKPROXYFACTORY_H
