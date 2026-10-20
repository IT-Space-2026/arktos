/*
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

#ifndef OPENSEARCHENGINEDELEGATE_H
#define OPENSEARCHENGINEDELEGATE_H

#include <qnetworkaccessmanager.h>
#include <qnetworkrequest.h>

// Contract: OpenSearchEngine never takes ownership of a delegate
// passed to setDelegate() — the delegate must outlive every engine it
// is set on, or be unset first.  performSearchRequest() is invoked
// synchronously from requestSearchResults() on the engine's thread;
// \a data carries the POST body (empty for GET).
class OpenSearchEngineDelegate
{
public:
    OpenSearchEngineDelegate();
    virtual ~OpenSearchEngineDelegate();

    virtual void performSearchRequest(const QNetworkRequest &request,
                                      QNetworkAccessManager::Operation operation,
                                      const QByteArray &data) = 0;
};

#endif // OPENSEARCHENGINEDELEGATE_H
