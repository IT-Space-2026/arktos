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

#ifndef TORREQUESTINTERCEPTOR_H
#define TORREQUESTINTERCEPTOR_H

#include <qwebengineurlrequestinterceptor.h>

class AdBlockRequestInterceptor;
class AdBlockNetwork;

// TOR02: the tor profile's request interceptor.  A profile accepts
// exactly one QWebEngineUrlRequestInterceptor, so the HTTPS-first
// upgrade and the adblock matcher are composed here — plain http:
// requests are redirected to https: before the adblock rules run.
//
// The upgrade covers every http: request, not just main-frame
// navigations: anything crossing an exit relay on port 80 is
// plaintext to that exit.  .onion hosts are exempt — onion services
// are end-to-end encrypted by tor itself and many serve http only.
class TorRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    TorRequestInterceptor(AdBlockNetwork *network, QObject *parent = nullptr);

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

private:
    AdBlockRequestInterceptor *m_adBlock;
};

#endif // TORREQUESTINTERCEPTOR_H
