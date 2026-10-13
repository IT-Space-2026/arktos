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

#include "readermode.h"

#include "browserapplication.h"
#include "engineinterface.h"
#include "readersanitizer.h"
#include "webenginebackend.h"
#include "webview.h"

#include <qapplication.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpalette.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>

// The script world the reader driver runs in — non-zero, so page
// JavaScript can neither see nor tamper with __aroraReader,
// Readability, or the extraction pipeline (READ05).  The DOM is
// shared across worlds, so probing, cloning, and overlay
// construction behave exactly as they did in the main world.
static const quint32 ReaderWorldId = QWebEngineScript::UserWorld + 100;

void ReaderBridge::notifyExited()
{
    if (m_readerMode)
        m_readerMode->scriptExited();
}

void ReaderBridge::setPreference(const QString &key, const QVariant &value)
{
    if (m_readerMode)
        m_readerMode->scriptPreference(key, value);
}

ReaderMode::ReaderMode(WebView *view)
    : QObject(view)
    , m_view(view)
    , m_bridge(new ReaderBridge(this))
    , m_active(false)
    , m_available(false)
    , m_busy(false)
{
    // The bridge rides the page's existing channel (same registration
    // point as "external"/"aroraAutofill").  Tor pages carry a bare
    // channel (TOR09 — the qrc start page's "arora" object only), so
    // the registration itself is gated on the mode: the overlay still
    // works there, minus persistence and JS-side exit notification.
    if (!BrowserApplication::isTorMode()) {
        if (QWebChannel *channel = m_view->page()->webChannel())
            channel->registerObject(QLatin1String("aroraReader"), m_bridge);

        // READ05: the driver lives in an isolated world that has no
        // channel transport, so its chrome reports through DOM
        // CustomEvents; reader-forward.js relays them to the bridge
        // in the main world.
        QFile forwardFile(QLatin1String(":reader-forward.js"));
        if (forwardFile.open(QIODevice::ReadOnly)) {
            QWebEngineScript forward;
            forward.setName(QLatin1String("arora:reader-forward"));
            forward.setInjectionPoint(QWebEngineScript::DocumentCreation);
            forward.setWorldId(QWebEngineScript::MainWorld);
            forward.setRunsOnSubFrames(false);
            forward.setSourceCode(
                QString::fromUtf8(forwardFile.readAll()));
            m_view->page()->scripts().insert(forward);
        }
    }

    connect(m_view->page(), &QWebEnginePage::loadStarted,
            this, &ReaderMode::onLoadStarted);
    connect(m_view, &QWebEngineView::loadFinished,
            this, &ReaderMode::onLoadFinished);
}

int ReaderMode::fontSize()
{
    QSettings settings;
    return qBound(12, settings.value(QLatin1String("reader/fontSize"), 19)
                      .toInt(), 30);
}

QString ReaderMode::theme()
{
    QSettings settings;
    const QString stored =
        settings.value(QLatin1String("reader/theme")).toString();
    if (stored == QLatin1String("light") || stored == QLatin1String("dark"))
        return stored;
    // Unset = follow the application theme.
    return QApplication::palette().color(QPalette::Window).value() < 128
        ? QStringLiteral("dark") : QStringLiteral("light");
}

QString ReaderMode::scriptBundle() const
{
    static const QString bundle = [] {
        QString source;
        const char *files[] = {
            ":/Readability-readerable.js",
            ":/Readability.js",
            ":/reader.js",
        };
        for (const char *path : files) {
            QFile file(QString::fromLatin1(path));
            if (!file.open(QIODevice::ReadOnly)) {
                qWarning() << "ReaderMode:" << "Unable to open" << path;
                continue;
            }
            source += QString::fromUtf8(file.readAll());
            source += QLatin1Char('\n');
        }
        return source;
    }();
    return bundle;
}

void ReaderMode::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged(active);
    emit availableChanged(isAvailable());
}

void ReaderMode::setAvailable(bool available)
{
    available = available || m_active;
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged(available);
}

void ReaderMode::onLoadStarted()
{
    // A navigation drops the overlay with the old document.
    m_busy = false;
    setActive(false);
    setAvailable(false);
}

void ReaderMode::onLoadFinished(bool ok)
{
    if (ok && !m_active)
        probe();
}

void ReaderMode::scriptExited()
{
    // The in-overlay "Exit reader view" button already removed the
    // host element — just adopt the state change.
    setActive(false);
}

void ReaderMode::scriptPreference(const QString &key, const QVariant &value)
{
    QSettings settings;
    if (key == QLatin1String("fontSize")) {
        settings.setValue(QLatin1String("reader/fontSize"),
                          qBound(12, value.toInt(), 30));
    } else if (key == QLatin1String("theme")) {
        const QString theme = value.toString();
        if (theme == QLatin1String("light") || theme == QLatin1String("dark"))
            settings.setValue(QLatin1String("reader/theme"), theme);
    }
}

void ReaderMode::runDriver(
        const QString &call,
        const std::function<void(const QVariant &)> &callback)
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page) {
        if (callback)
            callback(QVariant());
        return;
    }
    const QString program = scriptBundle() + call;
    // JSCTL/SECLVL: the lift keeps the driver reaching pages whose
    // scripts are blocked — runJavaScriptLiftedOn owns the
    // JavascriptEnabled gate juggling, and the worldId lands every
    // call in the reader's isolated world.
    WebEnginePageAdapter::runJavaScriptLiftedOn(page, program, callback,
                                              ReaderWorldId);
}

void ReaderMode::probe()
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page)
        return;
    const QString scheme = page->url().scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("file")) {
        setAvailable(false);
        return;
    }
    QPointer<ReaderMode> self(this);
    runDriver(QLatin1String("window.__aroraReader.probe();"),
            [self](const QVariant &result) {
        if (self)
            self->setAvailable(result.toBool());
    });
}

void ReaderMode::enter()
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page || m_busy)
        return;
    if (m_active) {
        // Belt-and-braces for a stale flag: verify the overlay is
        // actually there before declaring victory.
        setActive(false);
    }

    m_busy = true;
    QPointer<ReaderMode> self(this);
    // Phase one: extract() in the isolated world returns the raw
    // Readability fragment.  Nothing it produced is trusted yet.
    runDriver(QLatin1String("window.__aroraReader.extract();"),
            [self](const QVariant &result) {
        if (!self)
            return;
        const QVariantMap map = result.toMap();
        if (!map.value(QLatin1String("ok")).toBool()) {
            self->m_busy = false;
            emit self->message(
                tr("Reader view is not available on this page."));
            return;
        }

        // Phase two: sanitize the fragment in C++ — the policy cannot
        // be reached or rewritten by page JavaScript — then hand the
        // safe markup back to the overlay builder.  The READ03
        // thin-content floor moved with the sanitizer: a body that
        // comes out under the character threshold with no media is a
        // degenerate extraction, refused honestly.
        const ReaderSanitizer::Result sanitized =
            ReaderSanitizer::sanitize(
                map.value(QLatin1String("content")).toString());
        if (sanitized.textLength < 500 && !sanitized.hasMedia) {
            self->m_busy = false;
            emit self->message(
                tr("Reader view is not available on this page."));
            return;
        }

        QJsonObject labels;
        labels[QLatin1String("tag")] = tr("Reader");
        labels[QLatin1String("fontDec")] = tr("Smaller text");
        labels[QLatin1String("fontInc")] = tr("Larger text");
        labels[QLatin1String("theme")] =
            tr("Switch between light and dark");
        labels[QLatin1String("exit")] = tr("Exit reader view");
        QJsonObject opts;
        opts[QLatin1String("fontSize")] = fontSize();
        opts[QLatin1String("theme")] = theme();
        opts[QLatin1String("labels")] = labels;
        opts[QLatin1String("title")] =
            map.value(QLatin1String("title")).toString();
        opts[QLatin1String("byline")] =
            map.value(QLatin1String("byline")).toString();
        opts[QLatin1String("siteName")] =
            map.value(QLatin1String("siteName")).toString();
        opts[QLatin1String("content")] = sanitized.html;
        const QString call = QLatin1String("window.__aroraReader.enter(")
            + QString::fromUtf8(
                QJsonDocument(opts).toJson(QJsonDocument::Compact))
            + QLatin1String(");");

        self->runDriver(call, [self](const QVariant &showResult) {
            if (!self)
                return;
            self->m_busy = false;
            if (showResult.toMap().value(QLatin1String("ok")).toBool()) {
                self->setActive(true);
            } else {
                emit self->message(
                    tr("Reader view is not available on this page."));
            }
        });
    });
}

void ReaderMode::exit()
{
    if (!m_active)
        return;
    QPointer<ReaderMode> self(this);
    runDriver(QLatin1String("window.__aroraReader.exit();"),
            [self](const QVariant &) {
        if (self)
            self->setActive(false);
    });
    // Do not wait on the callback: on a tor page there is no bridge
    // and a wedged renderer would leave the check state stuck.
    setActive(false);
}

void ReaderMode::toggle()
{
    if (m_active)
        exit();
    else
        enter();
}
