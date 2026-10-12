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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "pictureinpicture.h"

#include "browsermainwindow.h"
#include "engineinterface.h"
#include "pipwindow.h"
#include "tabwidget.h"
#include "webview.h"

#include <qapplication.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

static QString pipJsLiteral(const QVariant &value)
{
    // Serialize a QVariant as a JS literal.  QJsonDocument only stores
    // arrays/objects, so scalars ride inside a one-element array whose
    // brackets are stripped back off.
    QString json = QString::fromUtf8(QJsonDocument(
            QJsonArray::fromVariantList({value}))
        .toJson(QJsonDocument::Compact));
    return json.mid(1, json.size() - 2);
}

PipRequestBridge::PipRequestBridge(PictureInPicture *pip)
    : QObject(pip)
    , m_pip(pip)
{
}

void PipRequestBridge::requestPopOut(const QString &nonce)
{
    if (m_pip)
        m_pip->requestPopOut(nonce);
}

void PipRequestBridge::exitPopOut(const QString &nonce)
{
    if (m_pip)
        m_pip->closePopOut(nonce);
}

bool PictureInPicture::isActive() const
{
    return m_window != nullptr;
}

PipWindow *PictureInPicture::window() const
{
    return m_window;
}

PictureInPicture::PictureInPicture(WebView *view)
    : QObject(view)
    , m_view(view)
    , m_bridge(new PipRequestBridge(this))
    , m_nonceCounter(0)
{
    // The bridge rides the page's existing channel (same registration
    // point as "external"/"aroraReader").  Tor pages carry no channel
    // — the shim then rejects with NotSupportedError while the manual
    // pop-out paths (runJavaScript only) still work.
    if (QWebChannel *channel = m_view->page()->webChannel())
        channel->registerObject(QLatin1String("aroraPip"), m_bridge);

    // A real navigation kills the popped-out element's document — the
    // window cannot be returned to anything, so close it outright.
    connect(m_view->enginePage(), &Engine::Page::loadStarted,
            this, [this]() {
        if (m_window)
            m_window->close();
    });
    // If the source view dies first the window plays on standalone;
    // only "return to tab" stops making sense.
    connect(m_view, &QObject::destroyed, this, [this]() {
        if (m_window)
            m_window->setReturnEnabled(false);
    });
}

QString PictureInPicture::scriptBundle() const
{
    static const QString bundle = [] {
        QFile file(QLatin1String(":pip.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "PictureInPicture: Unable to open :pip.js";
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }();
    return bundle;
}

void PictureInPicture::runPageJs(
        const QString &call,
        const std::function<void(const QVariant &)> &callback)
{
    Engine::Page *page = m_view ? m_view->enginePage() : nullptr;
    if (!page) {
        if (callback)
            callback(QVariant());
        return;
    }
    const QString program = scriptBundle() + QLatin1Char('\n') + call;
    // JSCTL/SECLVL: a plain <video> still plays with scripts blocked
    // — the lifted path keeps pop-out reaching it.
    page->runJavaScriptLifted(program, callback);
}

void PictureInPicture::resolvePageRequest(const QString &nonce, bool ok,
        const QString &reason)
{
    if (!nonce.isEmpty() && m_bridge)
        emit m_bridge->popOutResolved(nonce, ok, reason);
}

void PictureInPicture::popOut()
{
    beginPopOut(QLatin1String("m") + QString::number(++m_nonceCounter),
                QVariantMap());
}

void PictureInPicture::popOutContext(const QUrl &mediaUrl,
        const QPoint &viewPos)
{
    QVariantMap spec;
    if (!mediaUrl.isEmpty())
        spec[QLatin1String("mediaUrl")] = mediaUrl.toString();
    // The request position is in view (widget) pixels;
    // elementFromPoint wants CSS pixels.
    const qreal zoom = m_view && m_view->page()
        ? m_view->page()->zoomFactor() : 1.0;
    if (zoom > 0) {
        spec[QLatin1String("x")] = viewPos.x() / zoom;
        spec[QLatin1String("y")] = viewPos.y() / zoom;
    }
    beginPopOut(QLatin1String("m") + QString::number(++m_nonceCounter),
                spec);
}

void PictureInPicture::requestPopOut(const QString &nonce)
{
    QVariantMap spec;
    spec[QLatin1String("tag")] = nonce;
    beginPopOut(nonce, spec);
}

void PictureInPicture::exitPopOut()
{
    closePopOut(m_activeNonce);
}

void PictureInPicture::closePopOut(const QString &nonce)
{
    // Closing drives the restore through the closing() signal, so the
    // window's X button and this path share one code path.
    if (m_window && (nonce.isEmpty() || nonce == m_activeNonce))
        m_window->close();
}

void PictureInPicture::beginPopOut(const QString &nonce,
        const QVariantMap &spec)
{
    // A second pop-out on the same view replaces the live one — its
    // close() restores the previous video through closing() first.
    if (m_window)
        m_window->close();
    QPointer<PictureInPicture> self(this);
    const QString call = QLatin1String("__aroraPipCtl.tag(")
        + pipJsLiteral(nonce) + QLatin1String(", ")
        + pipJsLiteral(spec) + QLatin1String(");");
    runPageJs(call, [self, nonce](const QVariant &result) {
        if (self)
            self->onTagged(nonce, result);
    });
}

void PictureInPicture::onTagged(const QString &nonce,
        const QVariant &result)
{
    const QVariantMap info = result.toMap();
    if (!info.value(QLatin1String("ok")).toBool()) {
        const QString reason = info.value(QLatin1String("reason"))
            .toString();
        emit message(reason == QLatin1String("disabled")
            ? tr("This video does not allow Picture-in-Picture.")
            : tr("No video found on this page."));
        resolvePageRequest(nonce, false, reason);
        return;
    }
    if (info.value(QLatin1String("src")).toString().isEmpty()) {
        emit message(tr("The video has no playable source."));
        resolvePageRequest(nonce, false, QLatin1String("nosource"));
        return;
    }
    if (!m_view)
        return;

    m_activeNonce = nonce;
    // Parent to the source window so Qt's child teardown destroys the
    // player page before QWebEngineProfile is released at app exit.
    m_window = new PipWindow(m_view->page()->profile(), info,
                             m_view->page()->url(), m_view->window());

    QPointer<PictureInPicture> self(this);
    connect(m_window, &PipWindow::ready, this,
            [self, nonce]() { if (self) self->detach(nonce); });
    connect(m_window, &PipWindow::failed, this,
            [self, nonce](const QString &reason) {
        if (!self)
            return;
        emit self->message(reason);
        self->resolvePageRequest(nonce, false, reason);
        if (self->m_window)
            self->m_window->close();
    });
    connect(m_window, &PipWindow::closing, this,
            [self](const QVariantMap &state) {
        if (self)
            self->onWindowClosing(state, false);
    });
    connect(m_window, &PipWindow::returnToTab, this,
            [self](const QVariantMap &state) {
        if (!self)
            return;
        // Grab the pointer first: onWindowClosing() clears m_window.
        // The follow-up closeEvent re-emits closing() which no-ops —
        // the nonce is already cleared.
        PipWindow *window = self->m_window;
        self->onWindowClosing(state, true);
        if (window)
            window->close();
    });
    // QT_QPA_PLATFORM=offscreen CHECK-fails (SIGTRAP) inside
    // QtWebEngineCore::Compositor::bind when a second top-level view
    // maps a compositor — nothing is ever visible there anyway, so
    // leave the window unshown; the page still loads and plays.
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
        m_window->show();
    emit activeChanged(true);
}

void PictureInPicture::detach(const QString &nonce)
{
    QPointer<PictureInPicture> self(this);
    const QString call = QLatin1String("__aroraPipCtl.detach(")
        + pipJsLiteral(nonce) + QLatin1String(");");
    runPageJs(call, [self, nonce](const QVariant &result) {
        if (!self)
            return;
        if (result.toMap().value(QLatin1String("ok")).toBool()) {
            self->resolvePageRequest(nonce, true);
            return;
        }
        emit self->message(
            tr("The video is no longer on the page."));
        self->resolvePageRequest(nonce, false,
                                 QLatin1String("gone"));
        if (self->m_window)
            self->m_window->close();
    });
}

void PictureInPicture::onWindowClosing(const QVariantMap &state,
        bool raiseTab)
{
    const QString nonce = m_activeNonce;
    const bool wasActive = m_window != nullptr || !nonce.isEmpty();
    m_window = nullptr;
    m_activeNonce.clear();
    if (wasActive)
        emit activeChanged(false);
    if (!nonce.isEmpty()) {
        // Restore the source element; the shim's popOutClosed signal
        // resolves any pending document.exitPictureInPicture() and
        // fires the leave event if restore() already removed the tag.
        if (m_view) {
            const QString call =
                QLatin1String("__aroraPipCtl.restore(")
                + pipJsLiteral(nonce) + QLatin1String(", ")
                + pipJsLiteral(state) + QLatin1String(");");
            runPageJs(call);
        }
        if (m_bridge)
            emit m_bridge->popOutClosed(nonce);
    }
    if (raiseTab && m_view) {
        if (TabWidget *tabs = m_view->tabWidget()) {
            const int index = tabs->webViewIndex(m_view);
            if (index >= 0)
                tabs->setCurrentIndex(index);
        }
        if (QWidget *window = m_view->window()) {
            window->raise();
            window->activateWindow();
        }
    }
}
