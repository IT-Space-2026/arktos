/*
 * Copyright 2026 The Arktos Authors
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

#include "extensionpagebridge.h"

#include "browsermainwindow.h"
#include "extensionbackgroundhost.h"
#include "extensionshimbackend.h"
#include "tabwidget.h"
#include "webview.h"

#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qwebenginepage.h>
#include <qwebengineview.h>

namespace {

QJsonObject pageBridgeErr(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

} // namespace

ExtensionPageBridge::ExtensionPageBridge(QWebEnginePage *page)
    : QObject(page)
    , m_page(page)
{
}

ExtensionPageBridge::~ExtensionPageBridge()
{
    // Drop the pending sends and port ends keyed to this page so the
    // hosted side sees the honest onDisconnect instead of stale
    // endpoints.  The runtime is a process-level singleton and
    // outlives the page.
    ExtensionBackgroundRuntime::instance()
        ->unregisterPageContext(this);
}

QJsonObject ExtensionPageBridge::senderFor(const QString &extensionId,
                                           const QString &frameUrl) const
{
    QJsonObject sender;
    sender.insert(QStringLiteral("id"), extensionId);
    if (!frameUrl.isEmpty()) {
        sender.insert(QStringLiteral("url"), frameUrl);
        const QUrl u(frameUrl);
        if (u.scheme() == QLatin1String("http")
                || u.scheme() == QLatin1String("https")) {
            QString origin = u.scheme() + QStringLiteral("://")
                    + u.host();
            if (u.port() > 0)
                origin += QLatin1Char(':') + QString::number(u.port());
            sender.insert(QStringLiteral("origin"), origin);
        }
        // data:/about:/file: frames carry no serializable origin —
        // Chrome serializes them as "null"; the field stays absent.
    }
    sender.insert(QStringLiteral("frameId"), 0);  // main frame only
                                                  // (ER01 scope)

    // sender.tab when the page resolves to a browser tab — the field
    // Bitwarden's message router switches on (vs an extension-page
    // sender which has none).
    if (m_page) {
        if (QWebEngineView *engineView
                = QWebEngineView::forPage(m_page)) {
            if (WebView *view = qobject_cast<WebView*>(engineView)) {
                TabWidget *tw = view->tabWidget();
                BrowserMainWindow *window
                        = BrowserMainWindow::parentWindow(view);
                if (tw && window) {
                    const int index = tw->webViewIndex(view);
                    const int windowId
                        = ExtensionShimBackend::windowIdFor(window);
                    sender.insert(QStringLiteral("tab"),
                                  ExtensionShimBackend::tabObject(
                                      view, index, windowId));
                }
            }
        }
    }
    return sender;
}

QJsonObject ExtensionPageBridge::invoke(const QString &op,
                                        const QJsonObject &args)
{
    ExtensionBackgroundRuntime *runtime
            = ExtensionBackgroundRuntime::instance();

    if (op == QLatin1String("hello")) {
        const QString extensionId
                = args.value(QLatin1String("ext")).toString();
        QJsonObject reply;
        if (extensionId.isEmpty()) {
            reply.insert(QStringLiteral("ok"), false);
            reply.insert(QStringLiteral("error"),
                         QStringLiteral("invalid-argument"));
            return reply;
        }
        // The handshake IS the scoping check — a page not covered by
        // the extension's grants gets the typed absent-API reply.
        if (!runtime->agentCovers(extensionId,
                                  m_page ? m_page->url() : QUrl())) {
            reply.insert(QStringLiteral("ok"), false);
            reply.insert(QStringLiteral("error"),
                         QStringLiteral("no-host-access"));
            return reply;
        }
        if (!runtime->isHosted(extensionId)) {
            reply.insert(QStringLiteral("ok"), false);
            reply.insert(QStringLiteral("error"),
                         QStringLiteral("not-hosted"));
            return reply;
        }
        reply.insert(QStringLiteral("ok"), true);
        reply.insert(QStringLiteral("id"), extensionId);
        return reply;
    }

    const QString extensionId
            = args.value(QLatin1String("ext")).toString();
    if (extensionId.isEmpty())
        return pageBridgeErr(QStringLiteral("invalid-argument"));
    if (!m_page)
        return pageBridgeErr(QStringLiteral("no-page"));
    if (!runtime->agentCovers(extensionId, m_page->url()))
        return pageBridgeErr(QStringLiteral("no-host-access"));

    // Sender identity is taken from the committed page url, never
    // from a caller-supplied field — an agent (or forged forwarder
    // traffic) cannot masquerade as another origin.  ER01 routes
    // main-frame sends only; a frameUrl arg arrives with ER02's
    // sub-frame support.
    const QString senderUrl = m_page->url().toString();

    // Relay keys are namespaced "<extId>#...": the ext the caller
    // claims must own every mid/chan it presents — an agent covered
    // for extension A can never touch extension B's channels.
    const QString mid = args.value(QLatin1String("mid")).toString();
    const QString chan = args.value(QLatin1String("chan")).toString();
    if (!mid.isEmpty()
            && !mid.startsWith(extensionId + QLatin1Char('#')))
        return pageBridgeErr(QStringLiteral("invalid-mid"));
    if (!chan.isEmpty()
            && !chan.startsWith(extensionId + QLatin1Char('#')))
        return pageBridgeErr(QStringLiteral("invalid-chan"));

    if (op == QLatin1String("storage")) {
        // The content-script chrome.* surface includes full
        // chrome.storage — proxy it to the extension's own backend
        // (same store the hosted context uses), gated on the
        // manifest's 'storage' permission.
        if (!runtime->hasExtensionPermission(
                extensionId, QStringLiteral("storage")))
            return pageBridgeErr(
                QStringLiteral("no-storage-permission"));
        ExtensionBackgroundHost *host =
                runtime->host(extensionId);
        if (!host || !host->bridge() || !host->bridge()->backend())
            return pageBridgeErr(QStringLiteral("not-hosted"));
        const QString method =
                args.value(QLatin1String("method")).toString();
        QJsonObject reply = host->bridge()->backend()->invoke(
            QStringLiteral("storage"), method,
            args.value(QLatin1String("args")).toObject());
        // Mutations fan the change record out: the extension's
        // other contexts see a real storage.onChanged (the shim
        // fires it via __args), this page's agent gets the push on
        // its own event name.
        if (reply.value(QLatin1String("ok")).toBool()) {
            const QJsonObject result =
                reply.value(QLatin1String("result")).toObject();
            const QJsonObject changes =
                result.value(QLatin1String("changes")).toObject();
            if (!changes.isEmpty()) {
                const QString area =
                        method.section(QLatin1Char('.'), 0, 0);
                runtime->deliverEvent(
                    extensionId,
                    QStringLiteral("storage.onChanged"),
                    QJsonObject{{QStringLiteral("__args"),
                                 QJsonArray{changes, area}}});
                emit extEvent(
                    QStringLiteral("__arktos.storageChanged"),
                    QJsonObject{{QStringLiteral("changes"), changes},
                                {QStringLiteral("areaName"), area}});
            }
        }
        return reply;
    }

    if (op == QLatin1String("send"))
        return runtime->pageSend(
            this, extensionId, mid,
            args.value(QLatin1String("msg")),
            senderFor(extensionId, senderUrl));
    if (op == QLatin1String("respond"))
        return runtime->pageRespond(
            this, mid,
            args.value(QLatin1String("response")));
    if (op == QLatin1String("portConnect"))
        return runtime->pagePortConnect(
            this, extensionId, chan,
            args.value(QLatin1String("portId")).toString(),
            args.value(QLatin1String("name")).toString(),
            senderFor(extensionId, senderUrl));
    if (op == QLatin1String("portAccept"))
        return runtime->pagePortAccept(
            this, chan,
            args.value(QLatin1String("portId")).toString());
    if (op == QLatin1String("portMessage"))
        return runtime->pagePortMessage(
            this, chan,
            args.value(QLatin1String("portId")).toString(),
            args.value(QLatin1String("msg")));
    if (op == QLatin1String("portDisconnect"))
        return runtime->pagePortDisconnect(
            this, chan,
            args.value(QLatin1String("portId")).toString());
    return pageBridgeErr(QStringLiteral("unknown-op"));
}
