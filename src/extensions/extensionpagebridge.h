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

#ifndef EXTENSIONPAGEBRIDGE_H
#define EXTENSIONPAGEBRIDGE_H

#include <qobject.h>

#include <qjsonobject.h>   // moc needs the complete type for the
                          // by-value invokable signatures
#include <qpointer.h>
#include <qstring.h>
#include <qurl.h>
#include <qwebenginepage.h>  // moc instantiates QPointer<QWebEnginePage>
                           // — needs the complete type

class QJsonValue;

// ER01 — the page-side half of the hosted extension runtime.  One
// instance per WebPage, registered as the 'arktosExt' object on the
// page's ArktosChannelWorld QWebChannel and reachable only through
// the SEC24 whitelisted postMessage forwarder (the channel world is
// invisible to page script; the forwarder's METHODS/SIGNALS tables
// are the only surface anything below it can touch).
//
// The page agent (src/data/extagent.js — injected per extension into
// its own script world on covered pages) calls invoke() to deliver
// chrome.runtime.sendMessage/connect traffic into the hosted
// background; extEvent pushes carry the replies and the host->page
// deliveries (ports, ER02's message fan-in).
//
// The bridge is the SECURITY BOUNDARY, not the transport: any world
// on the page — including hostile main-world script — can post the
// same envelopes, so invoke() re-verifies per call that the named
// extension's host_permissions or declared content-script matches
// cover this page's url (ExtensionBackgroundRuntime::agentCovers).
// Uncovered calls answer a typed 'no-host-access' error — the
// absent-API reply, never a silent no-op.
class ExtensionPageBridge : public QObject
{
    Q_OBJECT

public:
    explicit ExtensionPageBridge(QWebEnginePage *page);
    ~ExtensionPageBridge() override;

    QWebEnginePage *page() const { return m_page; }

    // The single narrow entry the forwarder whitelists — 'op' picks
    // the verb, 'args' carries {ext, mid/msg/chan/portId/...}.
    Q_INVOKABLE QJsonObject invoke(const QString &op,
                                   const QJsonObject &args);

    // Chrome-shaped MessageSender for traffic originating in this
    // page: {id, url (frame url), origin (page origin), frameId, tab}
    // — sender.tab arrives whenever the page resolves to a real tab.
    // Exposed for the runtime's port routing, which stamps the same
    // sender onto '__arktos.portConnect' pushes.
    QJsonObject senderFor(const QString &extensionId,
                          const QString &frameUrl) const;

signals:
    // Host->page push channel — the agent subscribes through the
    // forwarder (SIGNALS['arktosExt']['extEvent']).
    void extEvent(const QString &name, const QJsonObject &payload);

private:
    QPointer<QWebEnginePage> m_page;
};

#endif // EXTENSIONPAGEBRIDGE_H
